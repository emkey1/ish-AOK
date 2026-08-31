# 9. Proving an emulator right

A compiler will not tell you that your `sub` sets the carry flag backwards. A
type system will not catch a page-table entry that stays writable one
instruction too long. The properties an emulator has to get right are almost
entirely semantic, and semantics have no compile-time representation — the code
that is subtly wrong compiles exactly as cleanly as the code that is right, and
runs almost exactly as well, until the day something depends on the difference.

Worse, the failures are rare in a specific and unhelpful way. An instruction
executed a hundred million times a second can be wrong in a case that arises
once a week. A flag computed incorrectly is invisible until a branch reads it.
A block invalidated one page too late corrupts a program that had been running
for an hour.

So the question this chapter answers is: how do you find out, deliberately and
on purpose, that the emulator is wrong — before a user does?

The answer, in every form it takes here, is the same word: **oracles**.

## 9.1 The oracle principle

An oracle is an independent implementation of the same contract, whose answers
you are willing to trust more than your own. It does not have to be perfect. It
has to be *independent*: produced by different people, from a different
understanding, so that its mistakes are unlikely to be your mistakes. When you
and the oracle agree, you have learned something weak. When you disagree, you
have learned exactly where to look.

iSH-AOK has four kinds available, and uses all of them:

1. **Real silicon.** An x86 processor, single-stepped under `ptrace`, executing
   the same program.
2. **Another emulator.** The Unicorn engine, for hosts where the first is not
   available.
3. **Real Linux.** A physical machine, a VM, or Rosetta, running the same test
   program compiled natively.
4. **Itself, differently configured.** The interpreter versus the JIT; the JIT
   with the block cache disabled; the JIT single-stepping. Not an independent
   implementation of the contract, but an independent implementation of the
   *path*, which is enough to answer "is this the translator or the kernel?"

There is a discipline that goes with them, and it is stated here because it is
the most frequently violated rule in the project's history: **check the oracle
before claiming a defect.** A surprising number of "emulator bugs" turn out to
be behaviours real Linux shares. Empty output is not death. A program that
prints nothing under AOK and nothing on the oracle is a program that prints
nothing.

## 9.2 Ptraceomatic

The oldest of these tools dates to May 2017, three weeks into upstream iSH's
existence, and its header has not been rewritten since:

> Fun little utility that single-steps a program using ptrace and simultaneously
> runs the program in ish, and asserts that everything's working the same.
> Many apologies for the messy code.

The design is exactly that. Start the same static binary twice: once as a real
child process under `ptrace`, once inside the emulator. Then loop —
`PTRACE_SINGLESTEP` the real one, step the emulated one, and compare their
architectural state after every single instruction. The first divergence is
reported with the instruction that produced it.

The comparison is a wall of macros, and the shape is worth seeing because it
says what "the same" means:

```c
#define CHECK(real, fake, fmt, ...) do { \
    if ((real) != (fake)) { \
        reportf(fmt ": real 0x%llx, fake 0x%llx\n", ##__VA_ARGS__, ...); \
        if (trap_wanted()) debugger; \
        return -1; \
    } \
} while (0)
```

`real` and `fake` — the vocabulary is honest about which one is being doubted.
Every general register, the instruction pointer, the flags, and the FPU state
are checked, for both the i386 and amd64 register files.

Three complications make this harder than "step both, compare".

**Undefined flags.** x86 leaves certain flag bits architecturally undefined
after certain operations, which means the real processor is entitled to produce
anything at all and a different processor may produce something else. Comparing
them would report divergences that are not bugs. So the tool computes a mask —
`undefined_flags_mask()`, per instruction — and before comparing, copies the
emulator's value into the real one for exactly those bits. The trap flag is
added to the mask unconditionally, because single-stepping sets it in the real
process and not in the emulated one. This is a small piece of code that encodes
a real subtlety: fidelity means matching the *specified* behaviour, not the
observed behaviour of one particular chip.

**Syscalls cannot be executed twice.** If both processes ran `int $0x80`, they
would open two files, write two lines, and reserve two memory maps. So the
tracee is not allowed to execute syscalls at all. The tool intercepts the
instruction, copies the emulated process's output buffers into the tracee with
`pt_copy`, injects the emulated return value into `eax`, and steps the tracee's
`eip` past the instruction. The real process gets the emulator's answers, which
is the only way the two can stay in step.

**The starting state has to match, bit for bit.** `prepare_tracee` transplants
the VDSO, copies the emulator's entire initial stack page range into the tracee,
and sets the tracee's `esp` to the emulator's, so argv, envp and auxv in the
real process are literally the emulator's own bytes. `start_tracee` sets
`ADDR_NO_RANDOMIZE`. Anything not equalized at the start is a divergence at
instruction one.

## 9.3 The divergence at 4175

The best way to explain what this tool is like to use is a single episode, and
this one is documented in `docs/TODO.md` in enough detail to reconstruct.

**The symptom.** A static i386 glibc binary diverged at instruction 4175, every
time: `edx: real 0x1, fake 0x800000`.

**The instinct, and why it was wrong.** An emulator bug. The reading instruction
was `8b 54 24 14` — a load of `edx` from the stack — so presumably the emulator
had computed the wrong address, or the wrong value was on the stack.

Except 0x800000 is 8,388,608, which is exactly the oracle machine's `ulimit -s`.
The emulator's value was the *correct* one. The real process had garbage.

**Ruling out the alternatives, by construction rather than by argument.** This is
the part worth imitating:

- *auxv, the initial stack, the environment.* Ruled out because `prepare_tracee`
  copies the emulator's stack into the tracee wholesale; they cannot differ.
  This was the first suspect and it was wrong.
- *ASLR.* Ruled out because `ADDR_NO_RANDOMIZE` is set, and because the
  divergence reproduced at exactly instruction 4175 across two different
  binaries built minutes apart, at different addresses, with the same four bytes
  and the same values. Nothing random survives that.
- *A syscall returning host-specific data the tool does not synchronize.* The
  right family — but the direction is reversed from the guess. The problem was
  never that the real process saw a host value the emulator could not know. It
  was that **the emulator's answer never reached the real process.**

**The cause.** `step_tracing`'s interception switch had no case for syscall 191,
`ugetrlimit`. A syscall with no case still gets its *return value* synced — the
code falls out of the switch into `regs.rax = cpu->eax` — but nothing writes the
*memory* the syscall was supposed to fill. glibc's `__libc_early_init` calls
`getrlimit(RLIMIT_STACK, &rlim)` and reads `rlim.rlim_cur` off the stack four
instructions later. The emulated process had 8 MB written into that slot. The
real one still had uninitialized stack, which happened to be 1.

The instruction the tool blamed was innocent. The write it was reading had never
been performed at all.

**Five independent lines of evidence**, which is what "confirmed" should mean:
the reading instruction is four instructions after the `call __new_getrlimit`
that fills the slot; `__new_getrlimit` issues `mov $0xbf,%eax`, and 0xbf is 191;
191 is absent from the switch; the fake value equals the host's real
`RLIMIT_STACK` and the real value does not; and removing the case reproduces the
divergence exactly while adding it back removes it.

**The diagnostic that now exists.** `PTRACEOMATIC_TRACE_SYSCALLS=1` prints
`syscall 191 -> 0   [no memory sync]`, which states the conclusion directly and
would have found this in minutes. Building that after the fact, rather than
moving on, is the difference between a fixed bug and a faster next bug.

**The second bug hiding behind the first.** With 4175 fixed, the run got 416
instructions further and stopped again, at `call *%gs:0x10`, with `esp` off by
exactly four. `step_tracing` runs the emulated CPU, hands any non-`INT_DEBUG`
interrupt to `handle_interrupt`, and then steps the real CPU regardless. But a
fault the emulator resolves *itself* — here an `INT_GPF` growing the stack after
`sub $0x101c,%esp` — leaves the faulting instruction un-retired, to be retried
on the next step. The real CPU had executed the `call` and pushed a return
address; the emulated one had not moved. One instruction out of phase, and every
subsequent comparison blames whichever instruction happens to be next. The fix
holds the real CPU back whenever the emulated CPU takes a non-syscall interrupt
without advancing `eip`, bounded at sixteen consecutive holds so a genuinely
stuck emulator reports rather than hangs.

**Where it left the tool.** A static i386 glibc binary now runs start to finish
with no divergence, verified on four binaries twice each with exit codes
matching a native run. amd64 guests still die in setup with `SIGSEGV`, which is
unchanged by any of this and consistent with the note in `prepare_tracee` that
VDSO and stack sync for amd64 is phase-2 work.

**And the general lesson, in the tool's own words.** Exactly four syscalls in the
whole start-up sequence were intercepted rather than executed: `set_tid_address`,
`set_robust_list`, `rseq`, and `ugetrlimit`. The first three write no guest
memory. Exactly one of the four needed a case, and exactly that one was missing.
So: when ptraceomatic next reports a memory divergence, read the
`[no memory sync]` lines before reading the emulator.

> **The bug that taught us this**
>
> There is a smaller story inside that one. Divergence reports originally went
> through `printk`, which writes to file descriptor 555 (Chapter 36's
> convention). Nobody redirects 555 when running this tool by hand, so the
> `writev` failed with `EBADF` and the report vanished. Then the tool fired an
> `int3` — intended as a breakpoint for someone single-stepping in gdb — and
> with no debugger attached that is simply a crash. The entire session was a
> shell printing "Trace/breakpoint trap" and not one line explaining what had
> differed.
>
> Reports go to stderr now, and the trap is opt-in behind
> `PTRACEOMATIC_TRAP=1`. The rule: the one output anyone actually wants must
> not go to a channel nobody opened.

## 9.4 Unicornomatic, and why a second oracle exists

Ptraceomatic needs an x86 host running Linux, because it needs real x86 silicon
and real `ptrace`. That rules out the Mac the project is mostly developed on, and
it ruled out something else, according to the header of the tool that replaced
it:

> Basically the same deal as ptraceomatic, except ptraceomatic doesn't run on my
> raspberry pi and I need to verify the damn thing still works on a raspberry
> pi.

`unicornomatic` runs the same lockstep comparison against the Unicorn CPU
emulator instead of against hardware. It is a weaker oracle — an emulator
checking an emulator — but it is a *differently wrong* one, written by different
people from the same manuals, and it runs where the strong oracle cannot.

That is the practical shape of oracle diversity: you do not get one perfect
reference, you get a set of imperfect ones with non-overlapping blind spots, and
you arrange to be able to run whichever is available.

## 9.5 The conductor: differential testing above the instruction level

Instruction-level lockstep answers "does this instruction behave correctly". It
does not answer "does this program behave correctly", which is a different
question involving syscalls, the filesystem, signals, and the JIT's own caches.

`tests/remote/conductor.py` is the harness for that. It builds each corpus test
three ways — i386 ELF, x86_64 ELF, and an x86_64 macOS Mach-O — and runs every
cell of a matrix:

| cell | what it is |
|---|---|
| `oracle` | native x86_64 via Rosetta — x86_64 truth |
| `mint:x86_64` | x86_64 ELF in a Linux VM — real-Linux truth |
| `mint:i386` | i386 ELF in the same VM — the i386 truth an Apple silicon Mac cannot provide |
| `amd64:interp` | ish with `/proc/ish/amd64_jit=0` |
| `amd64:jit` | ish with the amd64 JIT on |
| `i386:jit` | ish, default i386 JIT |
| `i386:no_cache` | ish with the block cache disabled for this program |
| `i386:single_step` | ish single-stepping this program |

Four of those cells are the emulator disagreeing with itself, and that is the
point. If `i386:jit` diverges and `i386:single_step` does not, the bug is in
block translation or block caching, not in the semantics of the instruction. If
both diverge and `mint:i386` agrees with neither, the bug is in the kernel. The
matrix localizes before anyone reads code.

Two design details are worth borrowing. Comparison is **key-based rather than
whole-file diff**, because an i386 cell may legitimately omit lines a 64-bit cell
emits, and a diff would flag every one of them. And unreachable hosts are
**auto-skipped**, so the harness degrades to the cells available on the machine
in front of you rather than refusing to run.

The conductor also minimizes: given a divergence in a cell, it reduces the test
until the smallest reproduction remains. A one-line repro is worth an hour of
reading.

## 9.6 The guest-side suite, and where it is registered

The primary regression gate is not any of the above. It is roughly 170 small C
programs in `tests/manual/`, which are published read-only inside the guest at
`/AOK/tests`, compiled *on the device* by the guest's own toolchain, and run
there:

```sh
sh /AOK/tests/setup-regressions.sh --install-deps --run
sh /AOK/tests/setup-regressions.sh --only fs_conformance,futex_core --run
```

Each program exits non-zero on failure and accepts `-v`. They cover signals,
futexes, process lifecycle, the filesystem layer, the JIT, and per-architecture
instruction behaviour — and each one exists because something behaved
differently from Linux and somebody had to find out why. That makes the
directory a specification by accretion: the tests say what "correct" means more
precisely than any document does.

Two things about it will catch a new contributor.

**There are two registration points.** A test must be listed in
`fs/aok-tests.manifest` to be published to `/AOK/tests` at build time, and in
`setup-regressions.sh` to be built and run. A test missing from the first is
*silently absent on the device* — it does not fail, it does not appear, and the
run reports success without it.

**tier0 registration is automatic, and cannot be opted out of.** The conductor's
discovery picks up every `tests/manual/*.c` that includes `test_common.h`, so a
new test joins that sweep with no registration at all. Two mechanisms, opposite
defaults, in the same directory.

> **What the guest believes**
>
> `test_common.h` contains a small function that is a lesson in how to write a
> skip condition. Some tests are run inside a chroot set up by
> `/AOK/tools/mount-root.sh`, which bind-mounts `/proc`, `/sys`, `/dev` and
> `/AOK/tests` from the *booted* root into the target root. So `/proc` describes
> a different filesystem from the one the test is standing in, and any test that
> cross-checks procfs against the live filesystem is comparing two unrelated
> roots. It cannot pass, however correct the kernel is.
>
> The detection is the interesting part. It would be natural to detect the
> situation by comparing procfs against `stat()` and skipping when they
> disagree. The header says explicitly why that is wrong: that comparison *is
> what these tests assert*, and a skip condition which overlaps the assertion
> hides the very bug it exists to catch.
>
> So it is detected by what the bind list leaves out instead — `/AOK/tests`
> exists because it was bound in, while `/AOK/VERSION` exists only in the booted
> root. Two paths, an `access()` each, and no overlap with anything under test.

## 9.7 The other two suites, and why both are needed

**Host tests.** `meson test -C build` runs the unit-level tests that do not need
a guest. Some of them skip: `float80` compares against the host's own 80-bit
`long double`, which Apple silicon does not have, so on the primary development
machine that test reports success while testing nothing. It runs in full on the
x86_64 Linux CI (Chapter 35), which is one of the reasons that CI exists.

**End-to-end.** `tests/e2e/` boots an i686 Alpine 3.11 rootfs, compiles C inside
it, and runs the result — a fork- and exec-heavy workload on the one architecture
the day-to-day arm64 testing never touches.

The case for keeping all three is a single incident, and it is recorded in the
project's own notes: a change passed the guest regression suite 125 to 0 on
arm64, and broke the e2e suite three runs out of three. It was the lazy-mmap
fork bug of Chapter 5. The arm64 suite could not see it; CI could. Suites are
not redundant when their architectures, libcs and workloads differ — they are
complementary, and the overlap is the part you can afford to lose.

## 9.8 Using a real Linux box properly

The strongest oracle is an ordinary Linux machine, and the project's practice
with it has two rules that are easy to get wrong.

**Build the test both ways.** Copy the `tests/manual/*.c` file and
`test_common.h` to the Linux host and compile it 64-bit *and* 32-bit. The
`-m32` glibc build is the high-value one, and the reason is a trap worth
remembering: every i386 root under AOK is musl, and musl always has a 64-bit
`off_t`, whereas 32-bit glibc still defaults to 32. A test using offsets past
4 GiB silently truncates them under glibc and passes while testing nothing. The
fix is mechanical — `#define _FILE_OFFSET_BITS 64` before the first include,
backed by a `_Static_assert(sizeof(off_t) == 8, ...)` so the next person cannot
lose it.

**Run it unprivileged.** AOK's CLI runs as uid 0, and root hides an entire class
of ordering and permission bugs (Chapter 15). A test that needs root on the
oracle should accept `EPERM` as the correct answer rather than being run with
`sudo`, because `EPERM` is what the interesting case looks like.

## 9.9 Rules that came out of all this

Each of these was paid for once.

**Not faulting is not executing.** A probe that runs an instruction group and
checks only that nothing crashed will pass when the instruction is implemented
as a no-op. Emulator tests need an observable *witness* — a register value, a
memory effect, a signal — not the absence of a fault.

**Check the oracle before claiming a defect.** More than once, behaviour
reported as an AOK bug turned out to be behaviour real Linux shares.

**A divergence names an instruction, not a cause.** Section 9.3 is the long
version: the reported instruction is where the difference became *visible*,
which can be hundreds of instructions after where it was *created*.

**"Passes alone, fails in the suite" is not proof of a flake.** The remedy that
looks obvious — re-run it by hand on a quiet machine — pointed the wrong way on
a real bug. What settles it is an A/B of the suspected cause in one binary,
flipped both directions, before any further re-runs; then bisecting the context
to find how much preceding work is required to reproduce; then attaching a
debugger and *looking* rather than theorizing.

**A refuted finding can still contain a real bug.** A verifier that correctly
rejects your classification may be reporting a genuine defect underneath it.
Read the refutations, not just the confirmations.

## 9.10 The honest gap

Everything in Sections 9.2 through 9.5 is an x86 story. Ptraceomatic needs real
x86 silicon; unicornomatic needs Unicorn's x86 support; the conductor's oracle
cells are Rosetta and an x86 Linux VM.

There is **no instruction-level oracle for the arm64 and riscv64 guests**. What
those guests have instead is a set of hand-written assembly tests
(`tests/arm64/*.s`, `tests/riscv64/*.s`), machine-generated decode vectors, the
full guest-side suite running on a real distribution userland, and — for arm64
specifically — the option of running a program under the retired interpreter
(`ISH_ARM64_FORCE_INTERP=1`) to A/B a suspected JIT semantics bug against a
second implementation in the same binary.

That is a genuinely weaker position than i386 enjoys, and it is worth stating
plainly rather than leaving a reader to infer that every guest is verified to
the same depth. The strongest available oracle for an arm64 guest instruction is
the host processor that would execute it directly — which is exactly the
comparison a lockstep harness on an Apple silicon host could make, and which
nobody has built.

---

*Anchors:* [tools/ptraceomatic.c](../../tools/ptraceomatic.c),
[tools/unicornomatic.c](../../tools/unicornomatic.c),
[tools/undefined-flags.h](../../tools/undefined-flags.h),
[tools/transplant.h](../../tools/transplant.h),
`tests/remote/conductor.py`, `tests/manual/setup-regressions.sh`,
`tests/manual/test_common.h`, [fs/aok-tests.manifest](../../fs/aok-tests.manifest),
`tests/e2e/e2e.bash`, `tests/arm64/`, `tests/riscv64/`,
[docs/TODO.md](../../docs/TODO.md) ("ptraceomatic's divergence at 4175").

*Story:* instruction 4175 — `edx: real 0x1, fake 0x800000`, where the emulator
was right, the tool was wrong, the blamed instruction was innocent, and a second
bug was hiding one instruction behind the first.
