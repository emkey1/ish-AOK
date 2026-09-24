# iSH-AOK roadmap

What the project intends to do next, and in what order. Written 2026-09-07,
during the 554 release run.

**This is the only document in the tree that says what happens next.** The
others deliberately do not:

- [docs/TODO.md](TODO.md) is a lab notebook. It records what is *known* about
  work that is not done -- the measurement, the rejected designs, the reason --
  and says nothing about whether anyone will do it.
- `docs/build_<N>_musts.md` is a commitment for exactly one release: the work
  deferred out of the last one with the diagnosis already made.
- [docs/book/ch42-where-it-could-go.md](book/ch42-where-it-could-go.md) is the
  narrative version, and it is careful to separate *proven possible* from
  *scheduled* from *thought experiment*. This document is the scheduled column.

A roadmap in a project like this one is a claim that can be checked, and it
should be revised when it is wrong rather than quietly outlived. Every item
below says what is **established** today, what the **next step** is, and how to
**prove** it -- the same shape the rest of the tree uses, for the same reason.

---

## Where 554 leaves things

Not roadmap, but the roadmap does not make sense without it. 554 is 115 commits
past 553 and closes three things that were open questions for a cycle or more:

**Simulated swap**, phases 0 through 2, finishing in this release. It ships off
by default, enabled in Settings with a user-specified size. Phase 3 device
validation is where the value has been: a 3 GB iPhone SE reaches states a 16 GB
iPad never does, and it found that the growth guard did not cover the page-fault
path at all -- a guest could commit 704 MiB past the point where the same total
in separate `mmap`s was refused, and the app was jetsam-killed while every
number AOK watched said it was fine.

**Memory truth.** `mem_resident_page_count` is real, and `/proc/meminfo`
describes the guest instead of the phone. This one matters to the roadmap
directly: it is the prerequisite the swap plan deferred `MemTotal` behind, and
it is also the thing that makes a *resident* page distinguishable from a
*mapped* one -- which is what any checkpoint of a guest has to know.

**The amd64 JIT.** A full amd64 regression suite runs with zero interpreter
fallbacks.

The through-line for what follows: **AOK now has a pager that can take a guest's
memory away and give it back, and knows which pages are really there.** Two of
the three 555 items are that capability pointed somewhere new.

---

## 555 -- persistence

The theme is that a session and a machine should survive things they do not
survive today: the app being killed, and a mistake.

### OS snapshot

Take a point-in-time copy of a machine, and go back to it.

**Established.** A root is a directory in the App Group container:
`<roots>/<name>/data/` holding the host files, and `<roots>/<name>/meta.db`, a
SQLite database holding the uid/gid/mode/device-node metadata the host
filesystem cannot carry. `Roots` (app/Roots.h) already implements import from an
archive, export to an archive, destroy, rename and expose-at-`/AOK/roots/<name>`.

So a snapshot is *expressible* today -- export to a tar and import it back under
a new name -- and nobody uses it that way, because a full archive round trip of
a multi-gigabyte root is minutes of work and a second full copy of the bytes.

**What makes it cheap is a host primitive the tree does not use at all.** APFS
clones: `clonefile(2)` copies a directory tree copy-on-write, so the clone is
near-instant and costs no space until the two copies diverge. Source and
destination must be on the same volume, which they are -- both are inside the
container. This was greenfield when the item was written -- zero uses of
`clonefile` or `COPYFILE_CLONE` anywhere in the tree -- and `fs/fake-snapshot.c`
is now the first, with a recursive-copy fallback (trying `FICLONE` per file) so
the Linux build still compiles and degrades honestly rather than pretending to
be cheap.

**The correctness problem is `meta.db`, not `data/`.** It is SQLite, and cloning
a live database while a guest is writing to it produces a snapshot that may not
open. That has a known answer here: AOK already quiesces the fakefs at suspend
-- `fakefs_quiesce_begin(2000, &stragglers)` in `applicationDidEnterBackground:`
(app/AppDelegate.m) -- for very nearly this reason, being mid-write when the
state is frozen. A snapshot of a *running* root takes the same quiesce, clones
`data/`, and takes `meta.db` through SQLite's own backup rather than a raw
clone.

**Restore of the booted root is a relaunch, and must be.** `Roots.h` already
carries the note explaining why: renaming or deleting the running root moves `/`
out from under the live guest. Restoring one is strictly worse. So restore
follows `defaultRoot` -- it takes effect at the next launch, and the UI says so
rather than pretending otherwise.

**Next step.** ~~Prototype on the CLI first~~ -- **done 2026-09-08**
(`fs/fake-snapshot.c`, `/proc/ish/snapshot`, commit `a9429f028`). It quiesces
the fakefs, clones `<root>/data` with `clonefile(2)`, and takes `meta.db`
through `sqlite3_backup_*` rather than cloning it, because a raw copy of a WAL
database is only valid if nothing is mid-write and `-shm` must never be copied
at all. **What is left is the app side**: the Machines screen, and restore.

**Prove it -- and the timing criterion below was wrong, so it is restated.**
The original read "complete in under a second" for a multi-gigabyte root. That
assumed cost scales with bytes. It does not:

|              root | entries | size    | clone time (interleaved x3) |
|-------------------|--------:|--------:|-----------------------------|
| alpine-arm64-test | 215,504 |  77 GiB | 4676 / 4781 / 5050 ms       |
| devuan-amd64-test |  13,916 | 606 MiB |  357 /  383 /  440 ms       |

127x the data, 12x the time -- the *entry-count* ratio, at roughly 25 us per
directory entry either way. **Snapshot cost is O(files), not O(bytes).** The
space half of the claim held exactly: cloning 77 GiB grew the container by
63 MiB. So the honest criterion is "**a stock root in about a second, and
proportional to file count beyond that**" -- which also means this cannot be a
synchronous UI-thread operation, and the Machines screen needs progress and a
cancel rather than a spinner.

The rest of the criteria are **met**, on the CLI, verified by booting the
result: a snapshot of a *running* root taken from inside the guest completes in
308 ms fully quiesced, and the clone gives back file contents, mode 0741, uid
123, gid 456, a character device with rdev 42:43, nested directories and
symlinks -- so the `meta.db` half is right, not merely the file half. A file
written *after* the snapshot is absent from it. The two diverge without
corrupting each other, in both directions. And under load -- two concurrent
create/chmod/rename/unlink loops -- it still reached a **full** quiesce in
478 ms, with the resulting root walking 13,945 entries at 0 unstatable and 0
read errors and `pragma integrity_check` returning ok.

That last one is the "snapshot a root that is running a build" case, and it is
the one that mattered: it is the evidence the quiesce gate actually drains
rather than merely being called.

**Compression: two products, not one flag** (maintainer's call, 2026-09-09 --
"snapshots and suspends should support compress/decompress of some sort, at
least optionally").

For a *snapshot* this cannot be an option on the clone, because compression
costs exactly what the clone saves. The clone is cheap only because the blocks
are SHARED: 63 MiB for 77 GiB. Compressing means reading and rewriting every
byte, so time goes back to O(bytes) and space to O(compressed size), and the
copy-on-write divergence property is gone. That is not a snapshot with a
checkbox; it is an archive.

**And the archive already exists and already compresses**: `fakefs_export`
(tools/fakefs.c) writes pax through `archive_write_add_filter_gzip`, surfaced as
`exportRootNamed:toArchive:`. So the two things to offer are:

- **Snapshot** -- instant, ~free, same device, for "try something and go back".
- **Export** -- slow, small, portable, survives a device move or a backup.

They differ by three orders of magnitude in cost, so the screen must name which
one the user is getting rather than hide it behind a "compress" toggle. What is
missing is not compression; it is that export is not presented as the archival
half of the same idea. Worth considering a zstd filter over gzip when that is
touched -- libarchive already has one and it is markedly faster at similar
ratios -- but that is a tuning question, not this decision.

**Where it lives in the UI is [#575](https://github.com/emkey1/ish-AOK/issues/575)
-- and that issue is not what this document said it was.** It read as "add a
delete button". Read 2026-09-09, the button already exists: `deleteFilesystem`
in `RootDetailViewController` (app/RootsTableViewController.m), with a
confirmation alert, correct disabling for the booted and default root, and a
footer explaining which of the two applies. `destroyRootNamed:` behind it is
sound, down to re-exposing the root if the removal half-fails.

**What is actually missing is reachability.** In the machines *list*,
`canEditRowAtIndexPath` returns YES only for the cached-archives section, so
swipe-to-delete on an installed machine does nothing at all -- no row action, no
refusal, no explanation. Delete exists only after tapping into the detail
screen. "Unable to delete machines" is the expected report from someone who
swiped and gave up, and it is a discoverability bug rather than a missing
capability. Unverified against the reporter's device, but it is the only path in
this screen that silently does nothing.

Second, and separate: when the machine *is* the booted or default one, the
footer states the rule and offers no remedy. A user with a single machine has no
route forward at all. Whatever ships should say what to do, not only what is
refused.

Snapshot, restore and delete still belong in one place, so the screen is being
touched regardless -- but the delete work is a row action and a sentence, not a
button and a capability.

### Suspend to disk

Save a running guest and bring it back after the app is gone.

**Why this is newly plausible, and it is not ambition.** The swap pager is most
of the machinery already, built and shipping in 554:

- a backing store with a slot allocator, and frames that own their slots;
- exact mapping ownership, with per-frame entry counts on `struct data`;
- eviction that writes a guest frame out, and a fault path that brings it back;
- `mem_resident_page_count` and a per-entry state byte, so *which* pages are
  real is a measurement rather than a guess;
- an address-space barrier that quiesces every thread of a process;
- a suspension gate already wired to the iOS lifecycle, next to
  `fakefs_quiesce_begin` and `sockrestart_on_suspend`.

Suspend to disk is that pager told to evict *everything*, plus the state the
pager does not carry: page tables, task and thread state, and the file
descriptor table.

**What it is not.** Not CRIU, and the scope has to say so out loud, because the
gap between "checkpoint a process" and "checkpoint *any* process" is where this
class of feature usually dies. CRIU on real Linux, with a real kernel's
cooperation, is still partial after a decade. The v1 that is worth having is
narrower and more useful than the general one: **survive the app being killed.**
iOS terminates this app routinely -- jetsam, memory pressure, a user swiping it
away -- and today that loses the session unconditionally. Same device, same
root, same build, back where you were. That is the whole promise.

**The three hard parts, honestly.**

*Host file descriptors.* Every `struct fd` carries a `real_fd` into the host,
and on restore that number means nothing. There are **eighteen** `fd_ops`
families in the tree -- realfs, tmpfs, aokfs, fuse, procfs, sysfs, proc_ns,
devpts, socket, fscontext, opath_link, epoll, eventfd, inotify, memfd, pidfd,
signalfd, timerfd -- and each needs its own re-materialisation rule. A regular
file re-opens by path and seeks to its offset, and fakefs paths are stable, so
the common case is genuinely easy. An
unlinked file, a pipe with bytes in it, a pty, a live TCP connection: each is
its own decision, and for some the honest answer is that it cannot come back.
**`sockrestart` is the precedent and the right model** -- it does not restore a
listening socket, it records enough to *rebuild* one, because iOS destroys the
original either way.

*Native programs are host code with a C stack.* This is the deepest one, and the
project already has the rule that names it: a native program is a function call.
There is no serialising a host C stack, and the shell the user is typing at is
usually native bash. But the answer already exists in miniature: bash knows how
to describe itself and re-launch -- `AOK_BASH_DUMP_STATE`
(`deps/bash/aok_fork.c:823`), written for a different reason entirely. So the
rule generalises: **a native program either knows how to dump its own state, or
the checkpoint refuses while it is running.** That is a capability boundary that
can be stated and reported, which is the difference between a limit and a lie.

*What a quiet point is.* v1 checkpoints at a boundary -- every guest task at a
syscall, no native program on the stack -- rather than at an arbitrary
instruction. That is a real restriction and it should be written into the design
rather than discovered.

**Compression belongs here, and more than it belongs to snapshots** (same
maintainer call, 2026-09-09). A checkpoint is the opposite shape from a clone: a
serialized blob of guest pages, written once and read once, with no block
sharing to lose. Guest pages compress well -- 2-4x is typical for anonymous
memory -- so it is close to pure win, and it should be optional the way swap's
size is.

**The pager already compresses -- this argument was made and then acted on, and
what it produced is now a prerequisite rather than a sequel.** Built in 555:
`kernel/zpool.c` and `kernel/zswap.c`, in two modes, with a Settings switch.
zswap puts the pool in front of the swap file so only what does not compress
reaches flash; zram has no file at all. The measurement this section asked for
was taken: lz4 at 2.46 us per 4 KiB page on an A9, 2.2-2.8x on real workloads,
2.11x with slab fragmentation counted, and decompress two orders of magnitude
under a flash read.

**What that changes for a checkpoint is the shape of the work, not just the
timing.** A checkpoint no longer needs its own compressor: the tier already
takes a guest frame, compresses it, and hands it back byte-identical, and
`zswap_store`/`zswap_load` are the same pair a checkpoint writer would have had
to invent. It also already answers the questions a first attempt gets wrong --
what to do with an incompressible frame (decline, leave it resident), and what
happens if the pool is dropped while a slot still names an object in it (refuse
the teardown loudly rather than hand back plausible garbage).

**And it settled one design question for suspend by accident.** RAM-only reclaim
turned out to need no watermark at all -- it fills the pool with cold frames
whenever there is room, because with no file there is no flash cost to ration
(555). A suspend is the same operation with the ceiling removed: evict
everything rather than everything cold. The eviction path a checkpoint needs is
therefore not just present but exercised continuously, on every device with
compressed memory switched on.

**SHIPPED IN 555, and further than phase 0 asked for.** What follows from here
to the end of this section is the plan as it was written; it is kept because the
reasoning still holds and because what actually got built can be read against
it. What exists now:

- `kernel/checkpoint.c`, `/proc/ish/checkpoint` (`save <path>` and `suspend`),
  `ISH_RESTORE`/`ISH_SESSION` on the CLI, and a Settings switch in the app that
  saves on backgrounding and resumes on launch. Off by default.
- **A freezer**, which is the part AOK can do and CRIU cannot: AOK owns the
  scheduler. A task running guest code parks at the top of task_run_current's
  loop; a task blocked INSIDE a syscall is woken, its wait returns EINTR, and
  the dispatcher rewinds the program counter over the syscall instruction, so
  it arrives at the loop top about to re-execute the call it was in. The image
  says "about to call read", and the restored guest calls it.
- **More than one process**, with the process tree, pids, sessions and process
  groups, and zombies whose status a parent has not collected yet. (The
  zombies only since 2026-09-21: the task collection the save used skipped
  them, so every one was lost and its parent's wait() failed with ECHILD.)
- **Threads.** A thread group comes back as one process: one address space,
  written once, and one descriptor table, fs and set of handlers, shared the
  way clone() shares them, with each thread's own registers, signal mask and
  clear-tid address. The image records which earlier task owns each shared
  object, so a vfork child sharing its parent's memory comes back sharing it
  too. A leader that exited ahead of its threads is recorded as well: AOK keeps
  it until the last thread goes, and the process's exit is reported as its.
  Before this, a three-thread rsyslogd came back as three processes, each with
  a private copy of what had been one address space.
- **Descriptor identity**: two processes sharing one struct fd get one back.
- **Pipes**, with the bytes still in them, and **named FIFOs**, reopened by
  path with the bytes still in them.
- **Descriptors with no file behind them** -- epoll sets with their
  registrations, inotify with its watch numbers and queued events, eventfd,
  signalfd, timerfd with its deadline, pidfd, memfd with its contents
  (kernel/anonfd_ckpt.h). Before these, every one came back as /dev/null, and a
  dbus-daemon whose epoll set was /dev/null spun at a full core and never
  served the bus that logins wait on.
- **Sockets**, described and built again (fs/sock_ckpt.h). A listening socket
  comes back listening; SOCK_SEQPACKET, which iOS's sandbox refuses outright,
  is rebuilt on a stream socket the way it is created in the first place. A
  connected local pair with both ends in the image -- a socketpair, or a
  connect/accept inside the guest -- comes back connected, with its peer
  credentials and what it had queued: a stream's bytes each way, datagrams one
  by one and in order. A connection whose far end is outside the image, and
  every TCP connection, comes back hung up: end-of-file and ENOTCONN, never a
  read that waits for ever. Every rebuilt socket keeps its options
  (SO_PASSCRED, timestamps, buffer sizes) and the guest's O_NONBLOCK, and a
  bound unix socket's node keeps its mode and owner -- rsyslogd's /dev/log
  came back 0755 instead of 0666, and every unprivileged program's log line
  vanished into AOK's fallback sink. udevd spun at most of a core after a restore until
  its SEQPACKET control socket and its worker pair came back as themselves.
- **tmpfs contents**: /run, /tmp and /dev/shm are RAM with a path on them, and
  nothing outside the image remembers them. The image carries each tmpfs mount
  and its tree -- files with their bytes, symlinks, fifos and device nodes,
  modes, ownership and times -- and the restore MOUNTS the tmpfs back before it
  fills it, because filling the directory underneath would write a session's
  pid files and lock files into the rootfs, where they would outlive the next
  boot. Sockets are the deliberate exception: sock_ckpt_rebuild binds those
  names itself, and a node already sitting there makes that bind EADDRINUSE.
- **The guest's clocks**, the way Linux carries them across hibernation:
  CLOCK_MONOTONIC continues from its value at the save, CLOCK_BOOTTIME and
  /proc/uptime also count the time the machine was stopped, btime stays put,
  and each process keeps its start time. Before this a restore started them
  all again near zero, and every absolute deadline in the image -- Python's
  time.sleep, glibc's CLOCK_MONOTONIC condition variables -- waited an extra
  "uptime at the save" (a minute, for a restored Python daemon on the iPad).
- **Timers, and the signals they deliver**: POSIX timers, the interval timers
  and alarm() come back armed (kernel/timer_ckpt.h), each deadline on the clock
  Linux counts it on -- MONOTONIC does not count the stop, BOOTTIME and a
  wall-clock deadline do -- and a sleep or poll-family timeout the freeze
  interrupted sleeps only what it had left, where it used to start over, even
  on a save with no restore. Signals queued and not yet taken come back with
  their siginfo; before, a pending signal came back as a bit nothing could
  deliver, and a program using SIGALRM as a timeout waited for ever.
- **The root it belongs to** (GH #607): an image names the root it was saved
  on -- its data directory's inode, which a rename keeps and a copy never has
  -- and a restore on any other root is refused before anything changes. The
  app offers only the current root's sessions, and deleting a root deletes its
  sessions. Before, a Devuan session resumed on Alpine ran against Alpine's
  files.
- **Native programs**, by the rule this section already named: zsh describes
  itself (its fork-by-relaunch already turns a live shell into a script that
  rebuilds it) and comes back with its parameters, functions and aliases. dash
  cannot -- it has no way to emit its shell functions as text -- and is refused
  by name.
- `tests/manual/checkpoint_restore.sh` is the proof, including every refusal,
  and `tests/manual/checkpoint_tmpfs.sh` is the tmpfs one -- it checks a held
  descriptor on a /run file too, and that nothing leaked into the rootfs.
  `checkpoint_anonfd.sh`, `checkpoint_fifo.sh`, `checkpoint_sockpair.sh`,
  `checkpoint_threads.sh`, `checkpoint_clock.sh`, `checkpoint_timers.sh` and
  `checkpoint_root_identity.sh` cover the rules above.

**What is still open**: descriptors in flight in an SCM_RIGHTS message when the
image is written (the bytes travel, the descriptors cannot, and the save says
so), a native program in a compute loop that makes no syscalls and so never
reaches a boundary, and dash's self-description. The measurement this section asked for on a real device
session has not been taken.

**Phase 0 was a gate, not a feature.** Two things, in
order. First, an inventory: walk a real booted guest and enumerate everything
held that cannot be trivially serialised, per `fd_ops` family, with counts --
because the interesting number is not whether a pty is hard, it is how many of
the fds in a normal session are the easy kind. Second, the narrowest possible
proof: checkpoint and restore a single-process guest, no native program, one
open file, on the CLI harness.

**Prove it.** The restored guest continues from the instruction after the
checkpoint, reads the same bytes from the same open file at the same offset, and
the four-arch suite passes on a guest that has been through a checkpoint/restore
cycle mid-run.

**The risk, stated in advance.** This is the shape of feature that is 80% done
quickly and then spends a long time on the last 20%, because the last 20% is
every program anyone actually runs. If phase 0's inventory says the common
session is mostly regular files and a pty, it is worth doing. If it says the
common session is full of things with no restore rule, that is a result, and the
right response is to publish the inventory and stop -- not to spend 556 and 557
discovering it slowly.

### Desktop groundwork

The Wayland work is shipped, both tiers. `DisplayRFBClient`, `DisplayRFBView`,
Metal shaders, `opt/AOK/tools/setup-wayland.sh` and `start-wayland.sh` are all in
the tree, and the applet runs a real wlroots compositor over VNC. **So the
desktop theme through 555--557 is polish and reach, not construction** -- which
is worth saying because the open issues read like the feature does not exist.

Two of them are the app's own chrome and both hurt a windowed session
disproportionately:
[#580](https://github.com/emkey1/ish-AOK/issues/580) puts the window controls
over the terminal content in windowed mode on iPadOS while fullscreen is correct
-- a layout-guide bug, not anything deep -- and
[#579](https://github.com/emkey1/ish-AOK/issues/579) loses keyboard focus after
a Magic Keyboard trackpad selection. A desktop that drops the keyboard when you
click is not a desktop.

And [#483](https://github.com/emkey1/ish-AOK/issues/483)'s second half: the
client still requests a fixed resolution pair rather than deriving it from the
window. The mechanism is already there -- `d60437caf` drives per-orientation
resize through RFB `SetDesktopSize`.
[#482](https://github.com/emkey1/ish-AOK/issues/482) is a screenshot with no
text and is very likely the same root cause; **confirm that before treating them
as two jobs**, because if they are one, this is a smaller item than the issue
count suggests.

---

## 556 -- the desktop is the product

555's persistence work continues here (suspend to disk phase 1: the fd
re-materialisation rules, and more than one process), but the headline moves to
making the graphical session something to recommend rather than something that
works.

**[#574](https://github.com/emkey1/ish-AOK/issues/574), desktop environments.**
The request is for something the tree can already install --
`setup-wayland.sh` puts the stack in place -- so the gap is packaging and
documentation, not capability. That makes it cheap and high-leverage: a
supported DE choice, a one-command setup, and a page of documentation that says
what works. Users do not know AOK does this. That sentence is true of several
things here and it is a product problem, not an engineering one.

**Pixman v2 coverage.** `pixman_accel_plan.md` has phases 0--2 done and verified
end-to-end against unmodified labwc and foot, with mask/`OVER_MASK_A8` named as
the biggest remaining gap and an app Settings toggle still missing. This is
directly the desktop's frame rate, and the measurement that justified it (~23.5%
of an interactive redraw window inside raw pixman) was taken on exactly the
workload 556 is about.

**Input, seriously.** Pointer, keyboard, modifiers, scroll, and what a trackpad
gesture means to a Wayland client. #579 is the first symptom rather than the
whole job.

---

## 557 -- reach

**3D acceleration, [#484](https://github.com/emkey1/ish-AOK/issues/484) -- as a
feasibility gate, not as a feature.** This is the largest open request and the
one most likely to consume a release without producing anything. virglrenderer
needs a host GL or GLES implementation to render against; iOS has Metal and
deprecated OpenGL ES years ago, so the only plausible path is ANGLE over Metal,
and that is a substantial dependency to carry into an app binary. Book ch42
notes it would need the Wayland work to land first -- it has landed, which is
why this is now a question worth asking rather than a deflection.

Treat it exactly like the Metal sgemm study and the JIT code cache study: a
scoped investigation with a **go/no-go gate and a number attached**, before any
estimate. The JIT code cache is the precedent worth remembering -- phase 0
measured translation at 3.5--6% of wall time against a 30% gate and the project
correctly did not build it. A no-go here is a good outcome, not a wasted
release.

**Suspend to disk ships**, behind a Settings switch and off by default, on the
same reasoning swap ships that way: a feature that spends the user's storage and
can lose their session is one they opt into.

**Keyboard toolbar customization, [#609](https://github.com/emkey1/ish-AOK/issues/609) -- a user request (2026-09-24).** In their
words: "reorganizing and adding/removing custom toolbar buttons."

Today the extra-keys bar above the on-screen keyboard is fixed:
- `Terminal.storyboard` lays out most of it: the `barButtons` outlet
  collection and the arrow key.
- `TerminalViewController` builds six punctuation keys in code (`dotKey` through
  `pipeKey`).
- The only setting hides the bar while a hardware keyboard is attached.

[#162](https://github.com/emkey1/ish-AOK/issues/162) delivered key *remapping*
(Caps Lock, Option, backtick-as-Escape), not the bar's layout.

The job:
- The bar's contents become an ordered list in `UserPreferences`.
- A Settings screen reorders, removes and adds keys. That includes custom
  buttons that send a string or key sequence the user defines.
- The bar is built from that list rather than from the storyboard.
- The default list reproduces today's bar exactly, so nobody who never opens the
  screen sees a change.

---

## Carried, not headlined

Work that fills the space between the items above. None of it is scheduled to a
release; all of it is ready to pick up, and the conformance items in particular
are what the release-run regression sweeps keep landing on.

**The debugging tools now work, and that was the highest-value item on this
list.** ~~`strace` and `gdb` kill the thread they attach to~~ -- **fixed
2026-09-08**, and the entry was wrong in an instructive way. It was not about
non-leader threads, and it was not the attach or the tracing: `PTRACE_INTERRUPT`
sent the tracee a real SIGTRAP, which the ptrace machinery intercepts while the
task is traced and which becomes an ordinary fatal signal the instant it is not.
`strace`'s detach interrupts a running tracee and then usually sees that
tracee's own syscall-stop first, so it detached leaving the trap queued. Two
smaller bugs went with it: `waitpid(<tid>, __WALL)` on a traced non-leader hung
(the gdb half), and `ptrace.seized` was never cleared on detach. The
measurements, the two wrong diagnoses on the way, and what is still open are in
[docs/historical/build_555_musts.md](historical/build_555_musts.md) §1;
[#503](https://github.com/emkey1/ish-AOK/issues/503) -- the amd64 cousin -- and
[#541](https://github.com/emkey1/ish-AOK/issues/541) are both closed.

That this sat for two releases as an unbounded "the tools do not work", and
turned out to be one line once anyone measured it, is the argument for measuring
a carried entry before carrying it again. The cost was never the bug: it was
that every diagnosis in between was made without the two tools that would have
answered it fastest -- the swap investigation settled a CPU-spin question from
`/proc/<pid>/io` counters for exactly this reason.

**The conformance long tail**, all in TODO.md with measurements: `PROT_EXEC` is
never enforced, so guest W^X is decorative -- a contained project with two
candidate designs, an identified obstacle in fault delivery across four dispatch
loops, and a note that it deserves its own before/after benchmark run rather
than being folded into a sweep. PI futexes are ENOSYS, and the lock half is
implementable while the priority-inheritance half is not -- so the honest shape
is documented up front. `pipe(2)` cannot honour PIPE_BUF atomicity while it
delegates to a host pipe. `tmpfs size=` is accepted and not enforced, which on a
device with a jetsam budget is host memory. `fcntl(F_GETFL)` reports an
`O_NONBLOCK` the guest never set. `/proc/locks` does not exist. FUSE has no
attribute cache, and three separate absences follow from that one gap.

**The reported-issue burn-down.** Eighteen open, and two of them are the same
network question from different angles:
[#568](https://github.com/emkey1/ish-AOK/issues/568) reports slow throughput and
[#523](https://github.com/emkey1/ish-AOK/issues/523)'s reproducible half is a
15.3 s TLS handshake tail against a sub-second median. That is a wait not being
woken rather than work being slow, which puts it in the poll/quiesce
neighbourhood -- and it reproduces with plain `curl`, so it needs neither Go nor
the AUR to chase.

**Closing issues is part of fixing bugs.**
[#541](https://github.com/emkey1/ish-AOK/issues/541) has been fixed since
2026-08-20 and is still open on GitHub. A fix the reporter never hears about did
not fully happen.

---

## Not on the roadmap, and why

Naming these is the point of the document. A "future directions" list that
contains everything commits to nothing, and an unmerged branch quietly becomes a
promise if nobody says otherwise.

**External display / AirPlay ([#540](https://github.com/emkey1/ish-AOK/issues/540)).**
Work exists on `worktree-external-display-540` and is deliberately unmerged --
the maintainer judged it flawed. It stays fenced. It is not "coming in a future
release", and it must not be swept into one by accident.

**DriverKit and raw USB.** M-series iPad only, an entitlement Apple grants per
app against a specific hardware justification and is unlikely to grant for a
terminal, and it vanishes in unsigned builds -- so it would split the user base
for a feature most users could not run. The survey is in TODO.md under *Host
capabilities worth exposing*. The useful half of that survey is that `iosfs`
already mounts USB storage through the document picker and **nobody knows**,
which is a documentation task, not a driver.

**Namespaces.** Architectural, not a gap -- and the Bedrock-AOK experience is
the evidence that it is the right call: the two capabilities that community
project actually needed were `bind_mount` and FUSE, both of which now exist,
and namespaces explicitly were not the ask.

**A guest address space for native programs.** Settled by measurement: it would
not produce `fork` anyway, and the memory lock costs 33--39x on tight access.

**A persistent JIT code cache.** Phase 0 ran and returned NO-GO with numbers:
translation is 3.5--6% of wall time against a 30% gate. Recorded so it is not
re-proposed.

**The WebKit/Wasm architecture.** A thought experiment, filed as one, and
valuable for what it revealed rather than as a plan: the moment syscalls are
answered by browser storage APIs, fakefs's uid/gid/mode/device-node model has
nowhere to live.

---

## Bluetooth LE -- unscheduled, and the best unclaimed item here

Not placed in a release because nothing above depends on it and it has no
deadline, but it is the highest ratio of *genuinely new capability* to *known
cost* on the list, and it should not get lost between the scheduled items.

CoreBluetooth's central role needs no MFi programme, no Apple-granted
entitlement and no vendor agreement: scan, connect, discover, read/write/notify
against any BLE peripheral, for the price of one Info.plist key and a user
prompt. `/dev/bluetooth` follows the `/dev/url` recipe that is already proven in
the tree, and app/LocationDevice.m is 190 lines for a read-only device -- this
one is read/write and stateful, so budget several hundred, plus the same five
registration points.

Two things decide whether it is done well, both in TODO.md: **do not fake
BlueZ** -- synthesising HCI-level events from a GATT-level API reports
controller states no real controller produces -- and App Review will ask why a
terminal wants Bluetooth, for which the precedent is already shipping in
`/dev/location`.

---

## Revising this document

Three rules, learned from the documents this one sits beside.

**Dates and numbers, not adjectives.** Every claim above is checkable. When one
of them turns out to be wrong, the correction is more valuable than the original
-- `docs/historical/build_553_musts.md` is kept precisely because the next cycle
found its diagnosis wrong in the optimistic direction.

**Status sections go stale faster than plans.** The swap plan's "still open"
lists were describing `/proc/swaps`, `swapon`/`swapoff` and `mincore` as
outstanding after all three had landed. If a status paragraph here is more than
a release old, distrust it and read the commits.

**A release that changes the roadmap is not a failure of the roadmap.** Swap's
phase 3 found a gap that defeated the feature's core promise, and finding it was
worth more than the schedule it broke.
