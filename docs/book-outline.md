# iSH-AOK: The Book — detailed outline

**Working title:** *A Kernel Without a Kernel: Inside iSH-AOK*
**Subtitle:** How a Linux userland runs on an iPhone, from one interpreted x86
instruction in 2017 to four JIT'd guest architectures and native host programs in 2026.

## Premise

iSH-AOK is a Linux kernel reimplemented as an ordinary iOS application. There is
no hypervisor, no container runtime, no JIT entitlement, and no privileged code
anywhere in it. Everything a Linux program expects — processes, signals, mmap,
futexes, ptrace, epoll, sockets, /proc, ttys — is a data structure in a userspace
app that Apple would let anyone ship. The book's spine is a single question asked
over and over at descending levels of the stack: *what does the guest believe, and
what is actually true?* Every chapter is a place where those two answers diverged
and something had to reconcile them.

The book is not an API reference. It is an architecture book with an argument,
and the argument is that emulation quality is a fidelity problem, not a speed
problem — speed is the part that turned out to be tractable.

## Audience and prerequisites

Three readers, addressed in this order of priority:

1. **The systems-curious programmer** who knows C and roughly what a syscall is,
   and wants to understand how an operating system's interface can be
   reconstructed from the outside.
2. **The contributor** who needs a map of the tree before touching it.
3. **The emulator/VM implementer** who wants the specific techniques: threaded-code
   dispatch, differential testing against real silicon, high-level emulation of
   libc, host-function-as-guest-process.

Prerequisites: C, basic POSIX. Not assumed: Objective-C, ARM64 assembly, iOS,
x86 encoding. Each is introduced where first needed.

## Conventions used throughout

- **"iSH-AOK" / "AOK"** is this project. **"upstream iSH"** is `ish-app/ish`, the
  project it forked from. Both names are load-bearing; the book never says bare "iSH".
- **"Guest"** is the emulated Linux side. **"Host"** is iOS/macOS and its libc.
- Every claim about behavior is anchored to a file and, where the history matters,
  a commit. Anchors appear in an `Anchors:` line at the end of each chapter.
- Chapters that exist because of a specific bug carry a `Story:` line naming it.
  These are the narrative engine of the book; the technique is explained through
  the failure that forced it.
- Measurements are quoted with method and date. An unreproducible number is
  omitted rather than rounded.

## Source material available in-tree

The book can be written almost entirely from primary sources already in the repo,
which is why this outline is worth trusting:

- 6,119 commits (2017-05-04 → present), with commit messages that from 2026 onward
  read as design notes rather than changelogs.
- `docs/` — 50+ design and postmortem documents: per-guest port plans, perf
  benchmark methodology, release notes for builds 521–551, and `docs/TODO.md`
  (2,163 lines), which is effectively a running lab notebook of diagnosed bugs.
- `opt/AOK/docs/` — the 21 user-facing documents compiled into the app.
- `tests/manual/` — ~196 focused regression programs, each one a specification of
  a behavior somebody got wrong once.
- The upstream `docs/CHANGELOG.md`, which preserves the 2017–2018 TestFlight-era
  build notes.

---

# Part I — Origins

*Why this exists, who built it, and what changed when it forked.*

## Chapter 1. The impossible app

- The constraint stack on iOS: no `fork` of a real process available to apps, no
  W^X-violating code generation without an entitlement, no root, a sandbox with a
  per-app container, and a lifecycle that suspends you when the user looks away.
- What people wanted anyway: a real shell, a package manager, a compiler, ssh.
- The three ways to attempt it — a remote shell (not local), a UNIX-like
  reimplementation (not Linux), and full emulation (slow) — and why upstream iSH
  chose the third and then attacked the "slow".
- The thesis of the whole system: **the kernel is a library, the CPU is an
  interpreter loop, and the filesystem is a database.**
- Reader's map of the book.

*Anchors:* `README.md`, `main.c`, `platform/`.

## Chapter 2. Upstream iSH, 2017–2023

- May 2017: Theodore Dubois's first commits — an x86 decoder and enough
  instructions to reach a syscall; "Everything to get Hello World working" four
  days later.
- **Ptrace-O-Matic** (May 2017), the single most important early decision: run the
  same program under a real x86 kernel via `ptrace` and under the emulator in
  lockstep, and compare register state at every instruction. Correctness by
  differential testing, not by reading the manual harder. It shapes everything
  later, including how the 2026 guests were brought up.
- The kernel takes shape: `brk`, TLS, VDSO, then the parts that make it a kernel
  rather than an interpreter — fds with thread-safe refcounts (Dec 2017), thread
  groups and `CLONE_THREAD` (Jan–Feb 2018), fakefs with a real metadata database
  and hardlink support (Jan 2018).
- The TestFlight era: builds 22–48 in `docs/CHANGELOG.md`, read as an archaeology
  of what users hit first — pty semantics for zsh, `RLIM_INFINITY`, `#!` argument
  passing, "very serious problems caused by renaming directories".
- May 2018: "Foundations of jit, no actual compiling yet" — the turn from
  interpretation to threaded code.
- The community and its other contributors (Saagar Jha, Jason Conway, Matthew
  Merrill, and others); the App Store / GPL question that has followed the project
  from the start.
- 2019–2023: consolidation, then quiet.

*Anchors:* `tools/ptraceomatic.c`, `docs/CHANGELOG.md`, `git log --reverse`.

## Chapter 3. The fork

- December 2020: the first AOK commits are unglamorous and diagnostic —
  `/proc` entry ownership, then printing the *process name* alongside the pid when
  reporting a stub syscall. The fork's temperament is visible in that second one:
  make the failure say who caused it.
- What "AOK" came to mean: a fork with its own product identity
  (`app.ish.iSH-AOK`), its own TestFlight, its own release cadence, and its own
  README because upstream's instructions stopped being true here.
- The divergence inventory, as of build 551: four guest architectures, native
  programs, `/AOK`, bundled and downloadable roots, File Provider, FUSE,
  Shortcuts, the optional accelerators, and a guest-side regression suite.
- Fork etiquette: what is still shared with upstream, what is credited (the
  aarch64 work motivated by `OpenMinis/ish-arm64`, `docs/CREDITS-aarch64.md`),
  and the practical traps — `gh` resolving to the wrong repository, upstream docs
  that no longer apply.

*Anchors:* `README.md` ("Upstream Relationship"), `docs/CREDITS-aarch64.md`.

## Chapter 4. 2026: the year the tree tripled

- The shape of the year, month by month: 28 commits in February, 400 in April
  (amd64), 660 in July (arm64 on the 1st, riscv64 on the 10th), 589 in August
  (native programs). 2,216 commits in one year against 3,900 in the previous six.
- What made that possible: an AI-assisted development loop with hard gates —
  every change validated by a guest-side test, a differential oracle, or a
  measurement, and postmortems written into `docs/TODO.md` as they happened.
- The discipline that keeps it honest, stated once here and demonstrated
  throughout: **check the oracle before claiming a defect.** Half the "bugs" in
  the emulator turned out to be behavior real Linux shares.
- How this book uses that record.

*Anchors:* `docs/TODO.md`, `docs/release-notes-since-iSH-AOK_*.md`.

---

# Part II — The Execution Engine

*How guest instructions become host work.*

## Chapter 5. The guest machine

- What a CPU is here: `struct cpu_state` — registers, flags, and the lazy-flag
  scheme that defers computing them until something reads them.
- Memory: the guest address space as page tables in `emu/memory.c`, the software
  TLB in `emu/tlb.c`, and why a TLB matters more here than a page table walk
  costs on real hardware.
- **Lazy anonymous mmap**: reservations versus page tables — a mapping is either
  reserved or materialized, never both, and never split. The invariant and why
  violating it produced the bugs it did.
- Copy-on-write, `MAP_SHARED`, and the three separate ways a shared mapping quietly
  stopped being shared.
- Floating point: `float80.c` and the x87 80-bit problem on hosts that do not have
  it (Apple silicon has no `long double` reference to test against — the test skips,
  and what that costs).
- Vectors: MMX, SSE, and AVX/VEX support for the x86 guests.

*Anchors:* `emu/cpu.h`, `emu/memory.c`, `emu/tlb.c`, `emu/float80.c`, `emu/avx.c`, `emu/vec.c`.

## Chapter 6. Threaded code: the JIT that is not a JIT

- **The central trick**, and the chapter the book exists to deliver: a compiled
  block is an array of `unsigned long` holding *addresses of pre-assembled
  assembly gadgets* and their immediates (`gen()` in `jit/gen.c`). No instruction
  bytes are generated at runtime. There is nothing to make executable, so there is
  no W^X problem and no JIT entitlement — the "JIT" is legal on a stock iOS app
  because it emits data, not code.
- The gadget library: hand-written assembly, one file per family (`math.S`,
  `memory.S`, `control.S`, `string.S`, `bits.S`), in two host flavors —
  `gadgets-aarch64` and `gadgets-x86_64`.
- Dispatch: how a gadget tail-calls the next one, the register conventions that
  make that cheap, and the measured cost — roughly 6.8 ns per dispatch, which is
  why total time tracks *guest instruction count* rather than guest instruction
  complexity.
- The block cache: hashing by guest IP, per-address-space `struct jit`, the page
  hash for invalidation, and **jetsam** — the deferred-free list that stands in for
  RCU, plus the lock that made a lost wakeup wedge the emulator.
- Return caches and instruction fusion, both runtime-togglable per guest through
  `/proc/ish/<arch>_jit_fuse` specifically so an A/B measurement is possible.
- Self-modifying code, page invalidation, and the cross-page hazard class.

*Anchors:* `jit/gen.c` (11,372 lines), `jit/jit.c`, `jit/jit.h`, `jit/gadgets-*/`,
`tests/manual/jit_fuse_ab.sh`.
*Story:* the cross-page `write_prep` ordering corruption — a gadget that was
correct in isolation and wrong at a page boundary.

## Chapter 7. Four guests

- Why more than one guest architecture at all: 64-bit userlands, distro
  availability, and the fact that most Linux software stopped being tested on i386.
- **i386** — the original guest and the one all the tooling was built for.
- **amd64** (April 2026) — 64-bit registers, a new syscall ABI, a different
  calling convention, and the largest interpreter file in the tree as its legacy.
- **arm64** (July 2026) — an ARM guest on an ARM host, and the disappointment at
  the center of it: *it is still one gadget dispatch per instruction.* Being the
  host's own ISA family makes a gadget body cheaper, not free. Where the win
  actually comes from instead (HLE, Chapter 8).
- **riscv64** (July 2026) — the cleanest bring-up, plus vendor/user extension hooks.
- The bring-up recipe, generalized from three repetitions: ABI scaffolding →
  per-ABI syscall table → gadget set → the architecture's own regression suite →
  differential comparison → boot a real rootfs.
- Per-ABI syscall tables and the trap that hides behind them: a table entry is not
  a reachable syscall, because native dispatch and the ENOSYS list run first.
- The interpreters (`emu/*_interp.c`) are legacy; the `engine` build option now
  offers only `jit`. What that means for anyone reading old code.

*Anchors:* `docs/amd64_port_plan.md`, `docs/aarch64_guest_plan.md`,
`docs/riscv64_guest_plan.md`, `jit/guest-arm64/`, `jit/guest-riscv64/`,
`kernel/calls.c`, `tests/arm64/`, `tests/riscv64/`.

## Chapter 8. High-level emulation: skipping the instructions entirely

- The observation: a guest spends much of its life inside `memcpy`, `strlen`,
  `memcmp` — functions whose *semantics* the host already implements in optimized
  form.
- Fingerprinting: recognizing a known libc routine at translation time and
  emitting a single HLE gadget for the whole block, with an unrecognized libc
  simply never matching and falling through. A pure fast path, never a correctness
  dependency.
- Why it is gated to the arm64 and riscv64 guests only, and why `ISH_HLE=1`
  silently does nothing on i386/amd64.
- The measured curve: 1.23x at 256 B, 3.16x at 4 KB, 7.17x at 64 KB, 6.68x at 1 MB —
  and the honest reading, that it helps data movement and is neutral where a
  program's own arithmetic dominates.
- `ISH_HLE_STATS=1`, and using call counts to find what to fingerprint next.
- The trap: a fingerprint that matches a function it does not actually implement.

*Anchors:* `jit/hle.c`, `jit/hle-table.inc`, `tools/hle_fingerprint_guest.c`,
`docs/performance-optimizations-2026-07.md`.

## Chapter 9. Proving an emulator right

- **Ptraceomatic**, in detail: the child runs under a real kernel, the emulator
  runs in lockstep, registers and memory are compared each step. What it can prove
  and what it cannot (it needs an x86 host, so it does not cover the ARM guests).
- **Unicornomatic**: the same idea against the Unicorn engine as oracle.
- The guest-side suite: ~196 programs in `tests/manual/`, each exiting non-zero on
  failure, published into the guest at `/AOK/tests` and run on-device. Two
  registration points — the manifest and the runner — and a test missing from
  either is silently absent.
- Host-side `meson test`, and the tests that skip rather than lie.
- **Not faulting is not executing**: an emulator probe needs an observable
  witness, or a nop-equivalent passes the test.
- Divergence triage: how to tell an emulator bug from a test bug from real-Linux
  behavior you did not know about. Worked example — the ptraceomatic divergence at
  instruction 4175 that was not an emulator bug.

*Anchors:* `tools/ptraceomatic.c`, `tools/unicornomatic.c`, `tests/manual/`,
`fs/aok-tests.manifest`, `tests/manual/setup-regressions.sh`, `docs/TODO.md`.

---

# Part III — The Kernel

*The parts of Linux that are data structures here.*

## Chapter 10. Processes, threads, and the shape of the task table

- `struct task` as the whole of process state; the pid table; how a guest
  "process" is one host thread.
- `fork`, `vfork`, `clone`, and what copying an address space costs when the
  address space is your own data structure.
- **Threads are children of their creator here**, not of the thread-group leader's
  parent as in Linux — a genuine divergence, what it changes in practice, and how
  `exit_finished` interacts with it.
- Exit, zombies, reaping, `wait4`, and process groups.
- The App Store version of the process model: no PID namespaces, no mount
  namespaces, one true system state. Chapter 21 shows why that turned out to be a
  feature for tooling and a wall for anything container-shaped.

*Anchors:* `kernel/task.c`, `kernel/task.h`, `kernel/fork.c`, `kernel/exit.c`, `kernel/group.c`.

## Chapter 11. Syscalls

- The dispatch path end to end: guest `int 0x80`/`svc`/`ecall` → interrupt handler
  → per-ABI table → handler → errno translation → result register.
- Four ABIs, one set of handlers, and the argument-marshalling layer between them.
- **A syscall often has a second copy**: per-ABI duplicate bodies, and the exits
  that return through `errno_map()` rather than a literal error constant — the
  fix-it-once-fix-it-twice rule.
- The legacy marshaller and how a dword-fit check killed legal 32-bit arguments
  (the bug that made `uv` and `fchmod` die with SIGSYS).
- Stub syscalls, the ENOSYS list, and the diagnostic that prints who asked.
- **Capability lies are load-bearing**: reporting a feature absent to avoid
  implementing it produces a system state real Linux never produces, and callers
  fall into paths nobody tests.

*Anchors:* `kernel/calls.c` (6,417 lines), `kernel/abi/`, `kernel/errno.c`.

## Chapter 12. Signals, job control, and ptrace

- Signal delivery when a "process" is a host thread: pending sets, masks, the
  self-pipe/wake problem, and why a host signal can be swallowed.
- `sigaltstack`, restart semantics, `SA_RESTART`, and EINTR discipline.
- Job control: controlling terminals, foreground process groups, `^C` and `^Z`,
  SIGTTIN/SIGTTOU, and the flow-control path.
- **ptrace**: `PTRACE_SEIZE`, group-stops, and why a debugger inside the guest is
  a stress test of the entire task model. Two of the hardest bugs in the tree live
  here — a `PTRACE_SEIZE` of an already-stopped tracee that hung forever, and a
  traced *native* program whose group-stop never reached its tracer.
- The wake-signal mask anomaly: measured behavior that contradicted the code, and
  the rule that came out of it — never argue safety from the mask.

*Anchors:* `kernel/signal.c`, `kernel/ptrace.c`, `fs/tty.c`, `docs/TODO.md`.

## Chapter 13. Memory management from the kernel side

- `mmap`/`munmap`/`mprotect`/`mremap` against the page-table model, and the
  validation Linux performs that the guest depends on.
- `brk`, stack growth, `MAP_GROWSDOWN`, and guard pages.
- `memfd`, shared mappings, and `MADV_*` — including `WIPEONFORK` and
  `MADV_REMOVE`.
- **PROT_EXEC is never enforced**: there is no NX for guest pages, why that is
  its own project rather than a patch, and what it means for anyone reasoning
  about guest security.

*Anchors:* `kernel/mmap.c`, `kernel/memfd.c`, `emu/memory.c`, `docs/TODO.md`.

## Chapter 14. Synchronization primitives the guest can see

- **Futexes**: the wait/wake table, `FUTEX_CMP_REQUEUE`, robust lists, and why
  every threaded guest program is a futex correctness test.
- `epoll`, `poll`, `select`, `eventfd`, `signalfd`, `timerfd`, `pidfd`, `inotify` —
  the readiness layer, and what "a file with no poll operation" should report.
- SysV IPC: semaphores and message queues.
- POSIX/Linux AIO.
- Timers, clocks, `times()`, `nanosleep` validation, CPU clocks, and alarm clocks.

*Anchors:* `kernel/futex.c`, `kernel/epoll.c`, `kernel/poll.c`, `kernel/eventfd.c`,
`kernel/pidfd.c`, `kernel/inotify.c`, `kernel/aio.c`, `kernel/sysvsem.c`,
`kernel/sysvmsg.c`, `kernel/time.c`.

## Chapter 15. Loading a program

- ELF loading: segments, interpreters, `AT_*` auxv, stack layout, and the VDSO
  the guest is given.
- `#!` scripts, argument limits, and the historical bugs at each boundary.
- Permission checks in the right order — **ask the execute question before opening
  the file**, and why running as uid 0 hides an entire class of permission bugs
  from anyone testing with the CLI.
- `personality`, `AT_SECURE`, and the setuid-binary crash that came out of it.
- Where `execve` forks in two directions: a guest image, or (Part V) a host
  function.

*Anchors:* `kernel/exec.c`, `kernel/elf.h`, `kernel/vdso.c`, `vdso/`.

---

# Part IV — Filesystems

*The layer with the most surface area and the most user-visible failure modes.*

## Chapter 16. The VFS

- `struct fd`, `struct inode`, mount table, dentry-equivalent path resolution.
- Path resolution rules the guest depends on: symlink loops, `..` above the root,
  `O_NOFOLLOW`, trailing slashes.
- **The `*at()` rule AOK got wrong**: an absolute path ignores `dirfd`. The bug's
  fingerprint was a MariaDB crash three frames from the cause, and there was a
  second copy of the rule in another file.
- Locking: `inodes_lock` as the measured bottleneck, and what "blocked" versus
  "contended" means when reading a lock profile.

*Anchors:* `fs/fd.c`, `fs/inode.c`, `fs/path.c`, `fs/mount.c`, `fs/generic.c`, `fs/lock.c`.

## Chapter 17. fakefs: a filesystem in a database

- The problem: iOS gives you a sandboxed container with no uid/gid, no modes, no
  device nodes, no hardlinks that mean anything, and a filename charset that is
  not Linux's.
- The answer: store bytes as host files, store *metadata* in SQLite. The schema,
  the db-inode key, and hardlink support (upstream, January 2018).
- Path mangling, `fix_path.h`, and what a guest filename becomes on disk.
- Rebuild and migration: `fake-rebuild.c`, `fake-migrate.c`, and why a flattened
  root sometimes needs reinstall rather than migration.
- **fakefs lock saturation**: one thread saturates the metadata mutex at 78%, so
  parallel metadata work is *slower*, not faster. `ISH_FAKEFS_LOCKSTATS` and how
  the measurement was taken.
- `0xdead10cc`: the iOS watchdog that kills an app holding a file lock across
  suspension, the bounded drain that fixed it, and the escape hatch that remains.

*Anchors:* `fs/fake.c`, `fs/fake-db.c`, `fs/fake-rebuild.c`, `fs/fake-migrate.c`,
`fs/fake-lockstats.h`, `tools/fakefsify.c`.

## Chapter 18. The synthetic filesystems

- `/proc`: per-pid entries, `meminfo`, `uptime`, `stat`, the fork counter, and the
  rule that a synthetic file is read by a *tool* with expectations —
  **read what the consumer reads** (btop reads `/sys/class/net`, not `/proc/net/dev`).
- `/proc/ish`: the fork's own control surface — tuning knobs, JIT fusion masks,
  defaults, diagnostics. The undocumented guest-side preference surface.
- `/sys`, devtmpfs, `/dev` nodes, dynamic devices, and what eudev needed before it
  would start.
- tmpfs, fifos, and the assert that could abort the whole app.
- ttys and ptys: the terminal seam from the kernel side — line discipline, canonical
  mode, VMIN/VTIME, word erase, literal-next, hangup and recovery.

*Anchors:* `fs/proc/`, `fs/proc/ish.c`, `fs/dev.c`, `fs/dyndev.c`, `fs/tmp.c`,
`fs/fifo.c`, `fs/tty.c`, `fs/pty.c`, `opt/AOK/docs/proc-ish.md`,
`opt/AOK/docs/tuning-knobs.md`.

## Chapter 19. Sockets and networking

- Mapping guest sockets onto host BSD sockets; where the two disagree.
- `SO_*` options that had to be implemented rather than accepted-and-ignored,
  `SO_BINDTODEVICE`, `SO_PROTOCOL`, and the Darwin-only options that needed guarding.
- Linux behaviors the guest relies on: a bound-but-not-listening TCP port refuses;
  connected UDP sockets report errors.
- Unix sockets and `SCM_RIGHTS` fd passing — the prerequisite for Wayland.
- `sockrestart`: iOS drops sockets when the app is suspended, and what has to be
  faked to keep the guest's belief intact.
- Name resolution, `/etc/resolv.conf`, routes, and `net_route.c`.

*Anchors:* `fs/sock.c`, `fs/sockrestart.c`, `fs/net_route.c`, `opt/AOK/docs/networking.md`.

## Chapter 20. FUSE

- Why FUSE at all: it lets unmodified guest filesystem daemons work.
- Protocol 7.31 implemented in-kernel; `/dev/fuse` and a `fuse` filesystem type.
- The privilege inversion that makes it simple here: no setuid `fusermount` is
  needed because the guest is already fake-root, so libfuse calls `mount(2)` directly.
- The `may_block` locking rule and why it is the constraint that shapes the code.
- Known gaps: no mmap, no FORGET.

*Anchors:* `fs/fuse.c`, `opt/AOK/docs/fuse.md`.

## Chapter 21. /AOK: a filesystem compiled into the binary

- What it is: a synthetic read-only filesystem mounted at `/AOK` on every boot,
  independent of which root you booted, un-deletable from inside a guest.
- How it is built: `fs/aok-*.manifest` lists files, `tools/gen-aokfs.py` embeds
  them as C string tables at build time, `fs/aok.c` serves them. The bytes are in
  the app binary — there is no on-device copy step.
- The three entries that break the read-only rule: `/AOK/persist` (host-backed,
  survives everything, flattens Linux ownership), `/AOK/fakefs` (survives
  everything, keeps full metadata), `/AOK/roots` (read-write views of your other
  installed roots, for cross-architecture chroot).
- `/AOK/native` is different again: no manifest, one entry per compiled-in program.
  A program this build does not carry has no entry at all rather than an entry that
  fails.
- Why "no namespaces" makes `/proc`, `/sys` and `/dev` the same from inside any
  chroot — and why that is what makes tools like `ktop` useful.
- The build trap: a doc or tool not listed in a manifest is silently absent on device.

*Anchors:* `fs/aok.c`, `fs/aok-*.manifest`, `tools/gen-aokfs.py`,
`opt/AOK/docs/00-overview.md`, `opt/AOK/docs/persist.md`, `opt/AOK/docs/roots.md`.

---

# Part V — Native Programs

*The fork's biggest idea: `execve` that runs host code.*

## Chapter 22. A native program is a function call

- The mechanism: `execve` of a path under `/AOK/native` dispatches to a function
  compiled into the app instead of loading a guest image, and the caller cannot
  tell. The registry in `kernel/native.c`; the pending-record dance that makes the
  handoff atomic.
- Why: not "faster I/O" but *interpretation*. A bash arithmetic loop runs roughly
  16x faster than under the emulated shell because the interpreter's own work stops
  being translated instruction by instruction.
- **THE RULE**, stated as a law and enforced by the rest of Part V: a native
  program is a function call on a guest task's thread, so every global it touches
  must be cleaned up or keyed by task. A file descriptor in a global is a landmine.
- The multicall pattern: `argv[0]` selects the applet, exactly as busybox does,
  and `/AOK/tools/native-links.sh` builds the symlink farm that puts them on PATH.
- The PATH-shadowing trap, and why a native program must be *compiled in* rather
  than dropped in.

*Anchors:* `kernel/native.c`, `kernel/native.h`, `opt/AOK/docs/native-programs.md`,
`opt/AOK/docs/native-setup.md`.

## Chapter 23. The shim: answering guest questions with host code

- The governing question, which is the sharpest sentence in the project's docs:
  not "is this function pure?" but **"can this function's answer differ between
  the host and the guest?"**
- What has to be routed: environment, identity (`passwd`/`group`), filesystem
  paths, `/etc/hosts` and `/etc/resolv.conf`, terminfo, locale, rc-file locations,
  time, load average, `klogctl`.
- How: `kernel/native_libc.c` (9,152 lines) force-included ahead of the system
  headers, redirecting libc calls onto the syscall dispatcher.
- The limits of `#define` redirection: it stops at AOK's own translation units, so
  a prebuilt library is a different problem. How to tell whether a library is safe
  to link.
- Locale as a guest question: Darwin spells the UTF-8 locale "UTF-8", which is a
  charset and belongs in `LC_CTYPE` only.
- **The gate**: `tools/check-native-libc.py` reports every host-libc symbol a
  native program references that is not on an explicit allowlist. Run deliberately,
  not wired into the build — and it grades the *archives*, which `ninja ish` does
  not refresh, so a stale archive means yesterday's verdict.
- Signals under the shim, and the day the shim's own signal blocking leaked into
  children so `^C` and `^Z` reached nothing.

*Anchors:* `kernel/native_libc.c`, `tools/check-native-libc.py`,
`tools/gen-nlibc-renames.py`, `kernel/native_syscall.c`, `kernel/native_io.c`.

## Chapter 24. The thing that cannot fork

- Why `fork` is impossible for a native program: a C function on a guest thread
  has no address space to copy, and the host stack cannot be duplicated.
- The answer, proven on bash and then generalized to zsh: **serialize your own
  state into a script and re-launch yourself.** The ordering constraints that make
  it work (extglob before functions, shopt before `set -o`), and what must not cross.
- What goes through that path in practice: command substitution, pipelines,
  subshells, background jobs.
- The cost, and how it was paid down: a per-line `2>/dev/null` that cost 6.5 ms per
  subshell, the `exec` fix that reached parity, and the `$$` fix.
- **The exec stand-in is a wait**: `exec` becomes spawn-then-wait, so signals aimed
  at the job hit that wait. The 127 bug and the checkpoint trap.
- Concurrency: one program, many simultaneous shells, via thread-local rewriting of
  the interpreter's statics (`tools/bash-tls-*.py`, `tools/check-bash-tls.py`) —
  and why a mismatch gate misses whole classes of state.
- Scoring the result honestly: 119 differential zsh cases with expectations taken
  from real zsh, 116 passing, and the two failures that belong to the rootfs
  (`/dev/fd`) rather than to the shell.

*Anchors:* `deps/bash/aok_fork.c`, `deps/zsh/Src/aok_fork.c`, `kernel/bash_glue.c`,
`kernel/zsh_glue.c`, `docs/bash_native_plan.md`, `docs/bash_native_reentry.md`,
`tests/manual/native_zsh_fork_state.sh`.

## Chapter 25. The catalogue

- **SmallCLUE** — a busybox-style MIT toolbox, and the vehicle for everything else.
- **OpenSSH** as applets — `ssh`, `scp`, `sftp`, `ssh-keygen`, `ssh-copy-id`, built
  without OpenSSL (ed25519-only, and what that costs). Reviving it was
  re-enabling, not porting.
- **Nextvi** (`vi`) and **motepad**, the modeless terminal editor with an app-side
  counterpart.
- **helix** (`hx`) — a modal editor with syntax highlighting from grammars linked
  into the binary; MPL-2.0, so it is a build switch and default-off, and it adds
  tens of megabytes to a 14 MB binary.
- **Rust on the shim**: real Rust runs natively, async Rust included, by rewriting
  its libc imports onto the shim; the kqueue front end behind Tokio, and the four
  silent traps of foreign-toolchain interposition.
- **The benchmarks** (`bmm`, `bmt`) as native programs.
- **Applets do not own argv**: in-place argv compaction double-frees through the
  native-exec record — one fixed, several still live.
- The candidate list and what makes a good candidate.

*Anchors:* `deps/smallclue`, `deps/helix`, `deps/tokio`, `kernel/openssh_glue.c`,
`kernel/nextvi_glue.c`, `kernel/native_motepad.c`, `kernel/native_bench.c`,
`kernel/native_kqueue.c`, `tools/native-applet-audit.py`.

## Chapter 26. Licensing, honestly

- Why this chapter is in an architecture book: the license determines what is in
  the binary, and `link_whole` means the objects ship whether or not anything calls
  them. Removing a registry entry removes nothing — measured, 144 bash and 35
  readline objects stayed.
- The App Store / GPL conflict: `LICENSE.IOS` is a promise from *this project's*
  copyright holders and cannot bind the FSF, which holds bash's copyright and has
  had GPL software removed from the store twice (GNU Go 2010, VLC 2011) under
  GPL section 6.
- How that becomes engineering: `-Dnative_bash=auto|enabled|disabled`, a
  `Licensing` heading printed at configure time so nobody has to assume, and
  `ar t` as the verification.
- The rest of the inventory: SmallCLUE MIT, OpenSSH and libarchive BSD, liblzma
  public domain, zsh permissive, helix MPL-2.0.
- The mere-aggregation position: users still get bash — the emulated one from the
  rootfs, exactly like every other GPL tool in the distro.

*Anchors:* `meson_options.txt`, `meson.build`, `LICENSE.md`, `LICENSE.IOS`, `README.md`.

## Chapter 27. What native programs cost

- The settled question: **no guest address space for native programs.** Measured —
  the C stack kills it, the memory lock is 33–39x, and the structs that would need
  translating are already byte-copyable.
- Failure modes unique to this design: process-global state, an fd in a global,
  recursion off the end of a thread stack, a fatal signal firing while host stdio
  holds a FILE lock.
- Darwin stdio lock forensics: naming the `FILE` a wedged process is blocked on.
- When *not* to make something native.

*Anchors:* `docs/TODO.md`, `kernel/native_libc.c`, `docs/native_workspace_design.md`.

---

# Part VI — The Application

*Everything above the kernel: the part users actually touch.*

## Chapter 28. The iOS app around the kernel

- App structure: `main.m` → `AppDelegate` → `SceneDelegate` → `TerminalViewController`,
  and where the kernel gets started.
- The lifecycle problem: iOS suspends you, and a suspended kernel is a stopped
  world. Background execution, jetsam, and what has to be drained before suspension.
- App groups, containers, and the crash class that comes from assuming one exists.
- `UserPreferences` and the settings surface, including its guest-side twin at
  `/proc/ish/defaults`.
- Diagnostics: the in-app log, crash reporting, and reading an `.ips` for a host
  SIGKILL that looks like a guest exit 137.

*Anchors:* `app/AppDelegate.m`, `app/SceneDelegate.m`, `app/UserPreferences.m`,
`app/Diagnostics.h`, `kernel/log.c`.

## Chapter 29. The terminal

- The unusual choice: the terminal emulator is **hterm**, JavaScript from Chromium's
  libapps, running in a `WKWebView` — and `app/terminal/term.js` is the seam.
- The bundle is a build artifact: editing `hterm/js` does nothing until
  `hterm_all.js` is regenerated. (A trap that has cost real time.)
- Input: the keyboard accessory bar, external keyboards, modifier handling,
  IME/dead keys, paste, and selection.
- Output: scrollback, resize, cell metrics, and the height bug that could only be
  seen and not reasoned about.
- Themes and color: how a scheme is applied, and how to verify rendered color by
  sampling pixels from a screenshot rather than trusting the setting was written.
- The terminal seam from the guest's side: terminfo, capability negotiation, and
  the "stall" that turned out to be upstream zsh waiting for a Device Attributes
  reply that hterm does answer.

*Anchors:* `app/Terminal.m`, `app/TerminalView.m`, `app/TerminalViewController.m`,
`app/terminal/`, `deps/libapps`, `app/Theme.m`, `kernel/native_termcap.c`.

## Chapter 30. Roots

- What a root is here, and the four architectures it can be.
- Bundled versus downloadable: Alpine 3.23.3 and Devuan 6 (aarch64) in the bundle;
  the same two plus Arch for i386, x86_64 and riscv64 from the catalogue in
  `deps/rootfs-manifest`.
- Import, export, upgrade, and deletion; recording the guest ABI per root.
- `fakefsify` and how a tarball becomes a fakefs.
- Provisioning scripts (`provision-ultimate-*.sh`) and the "fixes" directory for
  known upstream-distro bugs.
- Cross-root work: `/AOK/roots`, `mount-root.sh`, chrooting into another
  architecture's userland.

*Anchors:* `app/Roots.m`, `app/RootsTableViewController.m`, `tools/fakefsify.c`,
`deps/rootfs-manifest`, `opt/AOK/tools/manage-roots.sh`, `opt/AOK/docs/roots.md`.

## Chapter 31. Files, Workspace, and the app-side tools

- **File Provider**: exposing guest files to the iOS Files app — enumeration,
  items, errno mapping, and per-root domains.
- **GuestFileBridge**: the app talking to the guest filesystem, and why one serial
  queue was wrong. Latency lanes, ordering guarantees per caller, and what happens
  to work whose caller has gone away.
- **Workspace** (13,443 lines in one view controller, and why): a file manager,
  MotePad, an image viewer, a video player, and a markdown renderer.
- The `md` renderer: the scrape heuristics, the marker design behind colour, and
  the day it rewrote the documents it was rendering.
- `ktop`: a process viewer that sees every process across every mounted root,
  labeled by guest architecture — possible precisely because there are no
  namespaces. And the CI job that froze its prebuilt binary while its source moved.

*Anchors:* `app/FileProvider/`, `app/GuestFileBridge.m`, `app/WorkspaceViewController.m`,
`app/MarkdownRenderer.m`, `opt/AOK/tools/ktop/`, `docs/guest_file_bridge_lanes.md`,
`docs/workspace_file_manager_plan.md`.

## Chapter 32. Devices and system integration

- Audio: output engine, PCM decode (Opus, Vorbis, AVF), and the guest-side interface.
- Location, RTC, battery, pasteboard — the pattern for "an iOS capability as a
  guest device".
- **Apple Shortcuts / App Intents**: a headless "Run Command" action that runs a
  command under the native zsh and returns output without the app coming to the
  foreground, plus Siri phrases. The shipping story: the unsigned-simulator wall,
  the metadata clobber, the scene-activation drop.
- **Foundation Models**: an on-device LLM bridge with a `run_shell` tool that
  reaches back into the guest through a confirmation dialog — availability
  flattening, and where the confirmation boundary sits.
- The RFB/Metal display path and the Wayland plan it serves.

*Anchors:* `app/AudioPlayerEngine.m`, `app/LocationDevice.m`, `app/RTCDevice.m`,
`app/ISHAppShortcuts.swift`, `app/ISHRunCommandIntent.swift`,
`app/AOKFoundationModelsBridge.swift`, `app/DisplayRFBClient.m`,
`opt/AOK/docs/shortcuts.md`, `opt/AOK/docs/llm-chat.md`.

## Chapter 33. The optional accelerators

- The design rule they share: **off by default, opt-in, and a pure fast path** —
  declining is not failing, and there is always a CPU fallback.
- Crypto (`ISH_CRYPTO_ACCEL=1`): AES-GCM and ChaCha20-Poly1305 offload, the guest
  OpenSSL provider that reaches it, and the install script.
- Pixman (`ISH_PIX_ACCEL=1`): composite offload and the guest-side shim.
- The accelerator syscall convention (`0xacc0`, `0xacc1`, …) and the two silent
  failure modes hiding in its dispatch — a binary ternary that misroutes a third
  number, and a discarded return that leaves the result register unwritten.
- Metal sgemm: the milestone-1 study, the no-copy property and the *blessed
  allocation* it depends on, and the honest scoping — a naive kernel proves a win,
  never a loss.

*Anchors:* `kernel/ish_accel*.c`, `opt/AOK/tools/crypto/`, `opt/AOK/tools/pixman/`,
`docs/metal_sgemm_milestone1.md`, `opt/AOK/docs/crypto-accel.md`.

---

# Part VII — Building, Testing, and Shipping

## Chapter 34. Two builds

- **Xcode is the only build that ships.** The meson/ninja CLI build is a
  development convenience, and a feature that is not in the Xcode build is not done.
- The Xcode side: schemes, xcconfigs, the fork's bundle identity, the
  `Debug-ApplePleaseFixFB19282108` configuration and the bug it is named after, the
  Download Root phase, and the extensions.
- The meson side: `meson_options.txt` as the feature matrix (guest architectures,
  engine, log channels, native program switches, gadget dispatch variant), and why
  `--buildtype=debugoptimized` is not optional — an `-O0` emulator invalidates
  measurements, and the guest says so in `uname -v`.
- Submodules, and the worktree trap: worktrees share a git dir, so never
  `submodule update --init` from one.
- Generated code: `tools/gen-native-syscalls.py`, `tools/gen-aokfs.py`,
  `tools/gen-nlibc-renames.py`, `jit/offsets.c`, the VDSO build.
- The simulator: cargo triples are not clang triples, the two-arch fan-out, and the
  empty-entitlements trap that leaves the guest unbootable.

*Anchors:* `meson.build`, `meson_options.txt`, `app/*.xcconfig`, `iSH-AOK.xcodeproj`,
`tools/`, `xcode-meson.sh`.

## Chapter 35. Testing strategy

- The four tiers and what each is for: host unit tests, the guest regression suite,
  end-to-end boots, and differential oracles.
- Running the guest suite on device: `setup-regressions.sh --install-deps --run`,
  `--only`, and adding a test (source + manifest + runner, or it is silently absent).
- The three shell-script suites that are exceptions to the runner.
- CI: `ci.yml`, the dev and release IPA workflows, the ktop build, the site deploy,
  the Alpine repo update. **Linux CI is a second compiler** — GCC catches shipping
  bugs that clang's `-w` hides, including the Darwin-generated `config.h` class.
- Flakes: `time_conformance` failing only in a full-suite run, the i386 stack-thread
  race, and the discipline of not calling a flake a flake.

*Anchors:* `.github/workflows/`, `tests/`, `tests/e2e/`, `docs/TODO.md`.

## Chapter 36. Debugging a system with no debugger

- The log channels (`ISH_LOG=verbose strace instr`) and the fd-555 trace, which is
  the tool that answers "which code path actually ran".
- lldb against the CLI build; `ish-lldb.lldbinit` and `ish-gdb.gdb`.
- **In-process hardware watchpoints** on arm64 (`ARM_DEBUG_STATE64`) without a
  debugger attached — the self-test and the stale-address trap.
- Lock profiling: `ISH_FAKEFS_LOCKSTATS`, `util/lockstats.c`, and how to read a
  duty cycle that exceeds 100% (the tool is wrong).
- Stripped-binary crash forensics: an assertion report names the bad value; frame
  offsets plus `objdump` beat guessing.
- The rules that keep an investigation honest, each earned: check the oracle first;
  a knob that "fixes" a crash may only be short-circuiting the read; verify what the
  *user* runs, not what is convenient to run; a refuted finding can still contain a
  real bug.

*Anchors:* `kernel/log.c`, `util/lockstats.c`, `ish-lldb.lldbinit`, `docs/TODO.md`.

## Chapter 37. Releasing

- What a release is: bump `CURRENT_PROJECT_VERSION`, write
  `docs/release-notes-since-iSH-AOK_<N>.md` and a summary, tag
  `builds/iSH-AOK_<N>`. **The tag name is load-bearing** — the workflow triggers on
  it, so a differently named tag produces no build.
- `tools/release-aok.sh`: preflight, archive, export, upload; and where fastlane's
  Ruby requirements bite.
- TestFlight, App Store review, and the entitlements story — including the
  unsigned-IPA bug where sideloaders had nothing to read because entitlements were
  dropped from the bundle.
- The two checklists that do not live in code: audit README and translations plus
  `opt/AOK/docs` (they are compiled into the app), and triage open PRs, issues, and
  Xcode Organizer crashes.
- Reading the release notes as a design record — build 521 to 551 as a compressed
  history of the fork.

*Anchors:* `tools/release-aok.sh`, `.github/workflows/build-release-ipa.yml`,
`fastlane/`, `docs/release-notes-since-iSH-AOK_*.md`.

---

# Part VIII — Performance

## Chapter 38. Where the time actually goes

- The headline: the engine is **dispatch-bound**, ~6.8 ns per gadget dispatch, so
  cost tracks guest instruction count. Everything else follows from that sentence.
- Method before numbers: how a benchmark is run so the number means something —
  one process per measurement, interleaved A/B, `debugoptimized`, a quiet device,
  and the knobs consumed at translation time.
- The measured landscape: guest architecture comparisons, JIT fusion and return
  caches, HLE's curve, native versus emulated shells.
- The lock contention map: `inodes_lock` is the bottleneck; memory and pid locks are
  not. Parallel metadata work is slower than serial.
- What has *not* worked, and why that is worth publishing: HLE hurts some
  workloads, a guest address space for native programs is 33–39x, and mint's Intel
  iGPU cannot answer an A-series question.

*Anchors:* `docs/perf_benchmarks_2026_08.md`,
`docs/performance-optimizations-2026-07.md`, `docs/guest_architecture_benchmarks.md`,
`docs/jit_gadget_perf_plan.md`, `opt/AOK/docs/benchmarks.md`.

## Chapter 39. Six optimizations, in full

Each case study follows the same arc — symptom, measurement, hypothesis, the thing
that was actually wrong, the fix, the number afterward:

1. Instruction fusion and the return cache (translation-time wins).
2. HLE fingerprinting (skipping the interpreter entirely).
3. The native-shell interpretation win, and the subshell cost it created.
4. `fd_copy_range` and the short-write data loss behind `sendfile` — a correctness
   bug found while chasing throughput.
5. The gadget dispatch variant (`arm64_gret`: `dmb` versus `ldar`, 1.7x on ARMv8.0
   against ~6% on Apple silicon) — an option that exists because the right answer
   differs per device.
6. The file browsers freezing behind bulk transfers: a scheduling problem, not a
   throughput problem.

---

# Part IX — Doctrine and Direction

## Chapter 40. What this project believes

The rules, each stated with the failure that produced it. This is the chapter a
reader can take to a different project:

- Fidelity over convenience: **capability lies are load-bearing.**
- **Check the oracle before claiming a defect.** Empty output is not death.
- **A table entry is not a reachable syscall**; **not faulting is not executing.**
- Test what the consumer reads, and what the user actually runs.
- Root hides an entire class of permission bugs.
- Blocked is not contended.
- A syscall usually has a second copy.
- Comments cite files that do not exist; verify the guard before trusting the comment.
- Finish work completely, and publish the failures alongside the wins.

## Chapter 41. The honest gaps

- No `PROT_EXEC` enforcement — no NX for guest pages.
- No PID or mount namespaces, so nothing container-shaped works; this is
  architectural, not a missing feature.
- The interpreters are legacy and being retired.
- Known native-program gaps: the applet argv ownership class, unrouted host
  symbols, process substitution where the rootfs lacks `/dev/fd`.
- FUSE without mmap or FORGET.
- Performance ceilings that are structural rather than unfinished.

*Anchors:* `docs/TODO.md` ("Diagnosed, not fixed", "Deferred on purpose").

## Chapter 42. Where it could go

- Wayland in Workspace: headless wlroots plus wayvnc in the guest, a VNC client in
  the app, and why it became feasible (memfd + `MAP_SHARED`, `SCM_RIGHTS`, epoll
  family, native pixman).
- External display and AirPlay.
- GPU offload beyond the sgemm milestone.
- The WebKit/Wasm thought experiment: what a browser-hosted iSH would and would
  not be.
- Nested AOK under AOK — proven, ~50x per level, and what it is good for.
- The Bedrock companion, and the two gaps that mattered (bind mounts, FUSE).
- What a second maintainer would need to know first.

*Anchors:* `docs/wayland_workspace_plan.md`, `docs/wayland_rotation_resize_plan.md`,
`docs/external_display_plan.md`, `docs/wasm_browser_architecture.md`,
`docs/metal_sgemm_milestone1.md`.

---

# Appendices

- **A. Timeline.** 2017–2026, upstream and fork on one axis, with the commits that
  mark each turn.
- **B. Repository map.** Every top-level directory, what lives there, and where to
  start reading.
- **C. Syscall coverage.** Implemented, stubbed, and refused, per ABI — generated
  from `kernel/calls.c` rather than hand-maintained.
- **D. `/proc/ish` reference.** Every node, what it reports, and what writing to it does.
- **E. Environment and build knobs.** `ISH_*` variables and `meson_options.txt`, with
  defaults and measured effects.
- **F. The regression suite, annotated.** Each test in `tests/manual/` and the bug
  it exists because of.
- **G. Glossary.** Gadget, block, jetsam, fakefs, aokfs, HLE, applet, root, shim,
  native program, oracle.
- **H. Further reading.** Upstream iSH, `OpenMinis/ish-arm64`, Alpine and Devuan,
  the FSF's App Store enforcement writeups.

---

## Production notes

**Length.** 42 chapters at 4,000–6,000 words averages ~200,000 words, which is too
long for one volume. Two workable cuts:

- *Volume I: The Machine* (Parts I–IV, Ch. 1–21) — history, engine, kernel,
  filesystems. Stands alone as an emulation/OS book.
- *Volume II: The Product* (Parts V–IX, Ch. 22–42) — native programs, the app,
  shipping, performance, doctrine. Stands alone as a book about turning an emulator
  into something people use.

**Diagrams needed** (roughly 25): the layer cake; the guest→host syscall path; a
threaded-code block laid out as an array of gadget addresses; the block cache and
jetsam lifecycle; fakefs bytes-vs-metadata split; `/AOK` composition and where each
piece comes from; the native-exec handoff; the re-launch/serialize fork stand-in;
the terminal seam from hterm to line discipline; the app lifecycle against kernel
suspension.

**Recurring feature boxes:**

- *"What the guest believes"* — a divergence and its reconciliation.
- *"The bug that taught us this"* — one postmortem, told properly.
- *"Measure it yourself"* — a command the reader can run in the guest.

**Writing order.** Part II first (it is the most self-contained and hardest to get
right), then Part V (the most novel and least documented elsewhere), then Parts
III–IV, then I, VI–IX. Chapter 1 gets written last, as always.

**Fact-checking protocol.** Every technical claim gets an anchor at the time of
writing, and every measurement is re-run on current `working` before the chapter is
declared done. Claims sourced only from memory or from a commit message are marked
and verified against the tree — the project's own rule, applied to the book about it.
