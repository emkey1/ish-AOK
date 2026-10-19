# 7. Five guests

Upstream iSH ran one guest architecture: i386. That was the right choice in
2017 and it aged badly, for reasons that have nothing to do with the emulator.
Distributions stopped caring about 32-bit x86. Alpine still ships it; Debian
does not ship a modern one; a growing amount of software simply is not built for
it any more. A guest architecture is only as useful as the userland available
for it, and i386's was quietly shrinking.

iSH-AOK runs five: `i386`, `amd64`, `arm64`, `riscv64` and, since 562,
`ppc64le`, all through the same gadget JIT. This chapter is about what it took
to get from one to five, what each addition actually taught, and the two traps
that catch everybody reading this code for the first time.

## 7.1 The ABI is a field on the task

The mechanism that makes five guests possible is small enough to quote. The
first member of `struct task` is:

```c
enum guest_abi abi;
```

and the syscall layer keys off it through a table of function pointers:

```c
struct syscall_abi_dispatch {
    enum guest_abi abi;
    const char *name;
    const syscall_t *table;
    size_t num_syscalls;
    qword_t (*syscall_number)(const struct cpu_state *cpu);
    void (*syscall_args)(const struct cpu_state *cpu, qword_t args[6]);
    void (*syscall_result)(struct cpu_state *cpu, dword_t result);
};
```

Five instances of that struct — one per guest — are all that stands between an
`int 0x80` and an `svc #0`. The number comes from `eax` or `x8` or `a7` or `r0`;
the arguments come from `ebx/ecx/edx/esi/edi/ebp` or `x0–x5` or `a0–a5` or
`r3–r8`; the result goes back to `eax` or `x0` or `a0` or `r3`. Everything past
that point is shared.

Two of those instances are not like the others. The first:

```c
// riscv64 shares arm64's syscall table: both are asm-generic ABIs with
// identical numbering, and the entries reuse the same sys_* implementations
// either way. Only the register plumbing (a7/a0-a5/a0 vs x8/x0-x5/x0) and
// the riscv-arch-specific numbers (258 riscv_hwprobe / 259
// riscv_flush_icache, intercepted in handle_asm_generic_native_syscall)
// differ. If the tables ever need to diverge for real, split them then.
```

riscv64 points at `arm64_syscall_table`. Linux's asm-generic syscall ABI is
shared by both architectures with identical numbering, so a second copy would
be a second thing to keep in sync and nothing else. The comment states the
condition under which that decision gets revisited, which is the difference
between a shortcut and a design.

The second borrows from the other side. powerpc is not an asm-generic ABI: its
numbers are its own, close to i386's, and it kept the legacy calls (`open`,
`stat`, `fork`, `poll`) that asm-generic dropped — which is amd64's set. So
`ppc64_syscall_dispatch` points at `amd64_syscall_table`, and
`ppc64_syscall_number` maps each powerpc number onto the amd64 handler of the
same name through a generated table (`kernel/ppc64_syscalls.h`, written by
`tools/gen-ppc64-syscalls.py`). Matching by name rather than by number is what
makes sharing a table possible between two ABIs that number nothing alike.

## 7.2 amd64, and the assumptions of nine years

The amd64 port began in April 2026, and the plan document opens not with
instruction encodings but with a list headed **Hard Blockers**. It is worth
reading as an artifact, because it is what "add a second guest architecture"
actually means in a codebase that has only ever had one:

- Guest-sized types are 32-bit. `addr_t` and friends are `uint32_t` in `misc.h`,
  and that assumption leaks through memory management, exec, syscall
  marshalling, signals, and ptrace.
- CPU state is i386-specific: eight general registers and `eip`, with no room
  for `rip`, `r8`–`r15`, `orig_rax`, `fs_base`, `gs_base`, or `xmm8`–`xmm15`.
- The decoder is 16/32-bit only — it still treats `0x40`–`0x4f` as INC/DEC,
  which on amd64 are the REX prefixes.
- Memory management is a fixed 4 GB design, with a hard-coded 32-bit hole scan.
- Syscall entry is hard-wired to i386 ABI rules.
- The ELF loader rejects anything that is not `ELFCLASS32` plus `EM_386`.
- TLS, signals, ptrace and the VDSO are all i386-specific.

Every one of those is an assumption that was true and harmless for nine years.
The amd64 port is mostly the work of removing them, and the reason the three
later ports were dramatically cheaper is that this one paid for the
infrastructure: a per-task ABI enum, a 64-bit-capable MM, ELF64 loading, and a
second syscall table mechanism.

Two things about amd64 are still true today and are worth stating plainly rather
than discovering.

The amd64 JIT's gadgets exist only for aarch64 hosts: the iOS device, and an
Apple-silicon Mac running the CLI, which exercises the same engine. On any other
host the gadget path is incomplete, so `main.c` defaults it *off* there and runs
the interpreter instead; `ISH_HOST_AMD64_JIT=1` forces it on for development. A
developer on an x86 Mac or a Linux box -- which includes the Linux CI build --
is therefore not exercising the engine the device runs.

GNU `as` used to be kept off the amd64 JIT. Commit `b71ce84d` sent any process
named `as` to the interpreter, as containment for crashes under the amd64 JIT
front end that were never root-caused, and the probe harness
(`tests/manual/x86/amd64_gas_probe.sh`) sat in the tree waiting for someone to
re-run the assembler under the JIT. Someone did: `23edd81e` (September 2026)
removed the bypass, and the assembler now runs on the JIT like everything else.
The interpreter still carries a few `as`-named diagnostics from that hunt, which
only fire when the interpreter is the engine running. Chapter 40 has something
to say about workarounds that are documented versus workarounds that are
absorbed; this one was documented, and so it could be retired.

## 7.3 arm64, and a disappointment worth understanding

The arm64 guest arrived on 1 July 2026, and its motivation was a competitor.

Open Minis ships [`OpenMinis/ish-arm64`](https://github.com/OpenMinis/ish-arm64),
a GPLv3 fork of the same upstream that added an AArch64 guest backend. Their
published numbers on compute-heavy workloads: `int_arith_2M` 12x, `fib(30)`
9.2x, `sum(1M)` 10.2x, `seq`+`awk` over 100K lines 7.2x. The plan document's
reading of that is precise and is the reason the port happened:

> That's the entire "Open Minis feels faster" effect users are reporting — not a
> better JIT, just skipping cross-architecture translation for aarch64 binaries.

Both projects are GPLv3 derivatives of `ish-app/ish`, so the plan explicitly
uses their public source as a design reference and, where practical, adapts code
directly with attribution. This is not a clean-room reimplementation and does
not pretend to be; [docs/CREDITS-aarch64.md](../../docs/CREDITS-aarch64.md)
carries the file-level attribution.

Two consequences of that port are already familiar from earlier chapters. The
NZCV representation was reversed mid-port — from four decoded booleans to a
packed word matching PSTATE bit for bit — once the goal became gadget throughput
rather than interpreter clarity (Chapter 5). And `emu/arm64_interp.c` still
exists, still passes its own tests, and is no longer called: `cpu_run_to_interrupt`
routes arm64 straight to the gadget engine. It survives as a bisection escape
hatch behind `ISH_ARM64_FORCE_INTERP=1`, with a comment that is admirably
unflattering about it — the interpreter does not implement everything the JIT
does, it has been seen to `SIGSEGV` on a bare `/bin/echo`, and it exists to run
a targeted reproduction far enough to compare behavior at one point of interest.

Now the disappointment. An arm64 guest on an arm64 host is **still one gadget
dispatch per guest instruction**, or per pair of them where fusion applies
(Chapter 6). There is no fast path where the host simply
executes the guest's own instructions, because that would require writing them
into memory and executing them, which is the thing Chapter 6 exists to work
around. What being the host's own ISA family buys is that each *gadget body* is
cheap — an arm64 `add` gadget is roughly one host `add`, where an x86 `add`
gadget must also deal with lazy flags and 8/16/32-bit sub-register semantics.
The dispatch, which is most of the cost, is identical.

The README puts it in one sentence, and it is worth repeating because it
surprises people who expect near-native speed from a matching architecture:
being the host's own ISA family makes each gadget's body cheaper, not free.

## 7.4 Which guest is fastest, and why

That said, "not free" is not "not worth it". Measured head to head on one Apple
silicon host, same engine underneath all three, musl userlands throughout:

| workload | i386 | amd64 | arm64 |
|---|--:|--:|--:|
| 10⁹-iteration integer loop | 30.89 s | 37.11 s | **3.39 s** |
| 10⁹-iteration double loop | 32.54 s | 15.00 s | **5.20 s** |
| 5,000 threads, create + join | 195.6 ms | 190.4 ms | **187.1 ms** |

arm64 is 9.1x faster than i386 and 10.9x faster than amd64 on the integer loop.
And the thread benchmark, which differs by 4.5% across all three, is the control
that makes the compute numbers mean something: thread creation spends its time
in shared kernel code — `task_create_`, locking, host `pthread_create` — that is
identical for every guest, so it *should* be flat, and it is. The compute loop
lives entirely in translated guest code, so it exposes exactly how much work
each guest's translation does.

Reconcile that with the previous section carefully, because the two facts look
contradictory and are not. Every guest pays one dispatch per *guest
instruction*. What differs is how many gadgets one guest instruction expands
into: an arm64 `add` is close to a single cheap gadget, while an i386 `add`
carries lazy-flag bookkeeping and sub-register semantics, and an x86 memory
operand can require address computation gadgets before the operation itself.
Nine times fewer gadgets, not nine times faster dispatch.

The amd64 anomaly in that table is worth noting too: its float loop runs 2.5x
faster than its own integer loop, because the float path goes through native
SSE handling rather than general translation. A number that looks like an
architecture being fast is sometimes one path being special-cased, and
Chapter 38 is careful about the difference.

Where the rest of the arm64 win comes from is Chapter 8 — high-level emulation,
which is gated to the arm64 and riscv64 guests precisely because those are the
guests whose remaining cost is dominated by dispatch rather than by translation.

## 7.5 riscv64, the third time

riscv64 landed nine days after arm64, and the plan says why it was cheap: this
was the third 64-bit asm-generic-ABI guest and the second gadget engine, so
nearly every kernel-layer decision was already made. The porting rule it states
is the most practical sentence in any of the port plans:

> grep `GUEST_ABI_ARM64` and mirror every site

There are more than thirty such decision points in `kernel/calls.c` alone. When
two architectures share an ABI, "find the other one's name and do what it does"
is not laziness, it is the correct algorithm — and it is checkable, which
guesswork is not.

The target is RV64GC, and the plan flags one thing as mandatory rather than
optional: **C, the compressed 16-bit instruction extension**. Real distribution
binaries are full of compressed instructions, and a decoder that assumes 4-byte
instructions will not execute the first function it meets. That is the kind of
detail that turns a two-week port into a two-day debugging session if it is
discovered late.

RV64GC stopped being enough in 2026: Ubuntu moved its riscv64 builds to the
RVA23 profile from 25.10, so such a root did not run at all. 558 implements it —
the scalar extensions (Zba/Zbb/Zbs, Zicond, Zcb, Zfa, Zfhmin, Zicbo*,
Zimop/Zcmop, Zawrs) and the vector unit, V with Zvbb, Zvfhmin and Zvkt, every
instruction a gadget — and reports it through `riscv_hwprobe` as Linux does.
Pointer masking (Supm) is the one piece deliberately left out.

riscv64 also brought something neither x86 guest could: a supported way to add
instructions. The ISA permanently reserves four major opcodes — `custom-0`
(`0x0B`), `custom-1` (`0x2B`), `custom-2` (`0x5B`), `custom-3` (`0x7B`) — and
guarantees no ratified extension will ever claim them. That guarantee is what
makes a vendor-extension hook safe: an instruction added in that space cannot
collide with a future standard one, so the ratified ISA the JIT already
implements is not put at risk. `jit/riscv64_vendor_ext.c` is the reference
implementation and `/AOK/docs/riscv64-vendor-extensions.md` is the worked
example, written so that someone implementing a T-Head or Andes extension — or
an experiment of their own — has a path that does not involve forking the
engine.

## 7.6 ppc64le, from a sibling's gadgets

The fifth guest, in 562, is 64-bit little-endian PowerPC: ELFv2, the userland
Alpine ships as `ppc64le` and Debian as `ppc64el`. Its occasion was a set of
gadgets that already existed. AOK VM, the project's QEMU-based sibling, had
grown PowerPC gadgets for a Mac OS guest on an emulated G4, and the plan
records that they were written so that this port could take them: operands
are byte offsets into a CPU state, the condition register is a word per
field, XER's SO, OV and CA a word each. They were 32-bit and big-endian, so the
port widened them to 64 bits, dropped the byte reversal — a little-endian
guest's memory is the host's — and added what a Linux userland needs that a G4
never did: the doubleword operations, the 64-bit rotates, VSX, and POWER8's
additions. What came across is this tree's own now (`jit/guest-ppc64/`, with
`jit/gen_ppc64.c` as its translator); as of 562 no gadget source here is kept in
step with AOK VM's.

The target is **POWER8**, Power ISA 2.07, because that is the baseline both
distributions compile for: Alpine 3.24's busybox and musl already use `lxvd2x`,
`mtfprd`, `lwa` and `ldbrx`. The riscv64 port was the template — the same patch
order, the same "grep the riscv64 site and mirror it", and gadgets only, with
no interpreter and no C core. The order of work came from what real binaries
executed rather than from the manual: an instruction with no gadget logs its
encoding and raises `SIGILL` at that address, which is the whole bring-up
tool, and musl plus busybox turned out to be 263 mnemonics, mostly integer.

What the gadgets cover is the integer unit, the rotates, CR and XER (with the
`OE` forms and ISA 2.06's extended divides), branches, every load and store
form, and `larx`/`stcx.`; scalar floating point with FPSCR, kept by the same
lazy model riscv64's `fflags` use (`emu/fpenv.c`); and VSX and VMX, where VMX's
float operations round to nearest and flush under `VSCR[NJ]`, and a NaN operand
is chosen in POWER's order, never negated.

The kernel side is where powerpc is not like anything already here. Section
7.1 showed the numbers going by name onto amd64's handlers; what the names do
not settle is converted on the way through. powerpc's open flags, `mmap` and
`mlockall` flags and socket option numbers are its own. Its ioctl encoding has
three direction bits where everyone else has two, so every `_IOC` number is
re-encoded. Its `struct termios` has its own layout, flag values, `c_cc`
indices and request numbers (`kernel/ppc64_termios.h`). And an error comes back
the way Linux's powerpc returns one: a *positive* errno in `r3` with CR0's
summary-overflow bit set, which musl tests with `bns+` before negating — and
a restart backs the program counter up over the `sc` and puts back `r3`, as
Linux does. Around that sits the rest of a Linux process: ppc64's signal frames
and `sigreturn`, `ptrace` with powerpc's register sets, seccomp as
`AUDIT_ARCH_PPC64LE`, `rseq`, a `/proc/cpuinfo` in POWER's format, `AT_HWCAP`
(Chapter 15), and a vDSO.

The oracle had bugs of its own. The differential tests in `tests/ppc64/` run
the same generated programs under `qemu-ppc64le` and the AOK guest, and qemu is
the one that is wrong in three places: it misses the signed overflow of a
`divde` quotient past 2⁶³, its `vexptefp` is a Taylor series about zero that
drifts for large operands, and its `fnmadd` and `fnmsub` round wrongly under
directed rounding. The test headers say where, and AOK follows the ISA.
Chapter 9's rule — check the oracle before claiming a defect — cuts both ways.
The suite needed teaching too: tests that had assumed x86's ioctl and socket
option numbers learned powerpc's, and the `vfork` tests learned that musl's
powerpc64 `vfork()` is a plain `fork()`, so they make the system call
themselves (`TEST_WANT_RAW_VFORK` in `test_common.h`).

Alpine 3.24.2 ppc64le runs `apk` over HTTPS, `gcc`, Python and `strace`, and the
manual suite passes on it: 362 on the Mac, and 366 with none failing on the M4
iPad and on the A10X. The catalogue offers it under Community, marked
Experimental (Chapter 30). What it does not have yet is any speed work — no
fused pairs, no HLE, no specialised gadgets, one guest instruction to one
gadget — nor anything of ISA 3.0, so a distribution built for POWER9 (RHEL 9,
Ubuntu 24.04 and later) dies on `SIGILL`. The vector crypto instructions are
missing too, and withheld from `AT_HWCAP2`, so OpenSSL takes its VMX paths. And
everything was checked against qemu; nothing has yet met a POWER machine.

## 7.7 The bring-up recipe

Three ports in four months produced a repeatable shape, and the fourth, in
October, followed it patch for patch. It is the same order every time:

1. **ABI scaffolding.** Add the enum value, the `syscall_abi_dispatch`
   instance, and the register plumbing. Nothing executes yet.
2. **The syscall table.** Either a new one, or — for an asm-generic guest —
   point at the existing one and mirror the decision points, or — for
   ppc64le — map the guest's numbers by name onto one that exists.
3. **ELF and process setup.** Loading, auxv, stack layout, TLS, signal frames.
4. **The gadget set.** One `.S` file per instruction family, brought up against
   hand-written assembly tests: `tests/arm64/*.s` and `tests/riscv64/*.s` are
   exactly that — `arm64_hello.s`, `arm64_atomics.s`, `arm64_fp.s`,
   `riscv64_fib.s`, `riscv64_signal.s`, and a `decode_check.c` driven by
   machine-generated decode vectors — and `tests/ppc64/` adds generated
   differential programs run against `qemu-ppc64le`.
5. **A real userland.** Boot a distribution rootfs and run the guest-side
   regression suite (Chapter 9) on it.

Steps 1–3 are kernel work and mostly mechanical once the first port has paid for
the infrastructure. Step 4 is where the time goes. Step 5 is where the
surprises are, because a real rootfs uses instructions and syscalls in
combinations no test thought to try.

## 7.8 Two traps

**A table entry is not a reachable syscall.** This catches everyone. Reading
`arm64_syscall_table` and finding an entry for syscall 229 does not mean a guest
calling 229 reaches it, because two things run first: native dispatch, and the
ENOSYS list. On the asm-generic guests, several syscalls are dispatched
*natively* — full-width, before the table is consulted — and the table entry
exists for a different reason entirely, spelled out at each one:

```c
[229] = (syscall_t) sys_munlock, // dispatched natively (full-width); entry needed to pass the NULL check
```

The entry is there so a NULL check passes. Editing it changes nothing. The only
reliable way to find out which code actually runs for a given syscall on a given
guest is the strace log (Chapter 36), which names the path taken rather than the
path you would expect from reading the table.

**The interpreters are legacy, and one of them is still load-bearing.** The
`engine` build option offers exactly one value, `jit`. But `emu/amd64_interp.c`
is still about 14,600 lines, second only to `jit/gen.c`, and it is still what
runs on non-aarch64 hosts. Until 558 it was also where AVX semantics got executed
for amd64; now the JIT runs every VEX and EVEX instruction as gadgets, and on
an aarch64 host the interpreter is reached only by an instruction whose bytes
cannot be read. "Legacy" here means "not
where new work goes", not "dead". A reader who assumes the interpreters are
vestigial will misread both the amd64 story and the AVX one.

## 7.9 What five guests cost, and what they buy

The cost is a multiplier on every decision point in the kernel. Chapter 11's
rule — *a syscall often has a second copy* — is a direct consequence: per-ABI
duplicate bodies exist, and a fix applied to one is a fix missing from four.
The test matrix multiplies too, and not evenly: some tests only exist for one
architecture, and some bugs only exist in one engine.

What it buys is more interesting than "more architectures work".

It buys **userlands**. Alpine, Devuan and Arch for the architectures each
supports, and the ability to chroot between installed roots of different
architectures from one booted guest (Chapter 30).

It buys **differential coverage inside the project**. i386 and amd64 are
separate engines, so a bug in `jit/gen.c`'s i386 paths can live there and
nowhere else — which is why the old i386 e2e rootfs is the one to reach for
whenever i386 codegen changes. Alpine roots are musl and Devuan roots are
glibc, and the two libcs generate syscall sequences differently: glibc's
hand-written stubs pass registers in ways musl's do not, which is precisely how
the marshaller bug that killed `uv` with `SIGSYS` was found. Five guests and
two libcs are a lot of ways to be wrong in only one of them.

And it buys the thing the chapter opened with: a hedge against a distribution
dropping your architecture. i386's userland is shrinking. arm64's is not.

---

*Anchors:* [kernel/calls.c](../../kernel/calls.c) (`syscall_abi_dispatch`, the
five dispatches), [kernel/task.h](../../kernel/task.h), [kernel/abi/](../../kernel/abi),
[jit/guest-arm64/](../../jit/guest-arm64), [jit/guest-riscv64/](../../jit/guest-riscv64),
[jit/guest-ppc64/](../../jit/guest-ppc64), [jit/gen_ppc64.c](../../jit/gen_ppc64.c),
[kernel/ppc64_syscalls.h](../../kernel/ppc64_syscalls.h),
[kernel/ppc64_termios.h](../../kernel/ppc64_termios.h),
[jit/riscv64_vendor_ext.c](../../jit/riscv64_vendor_ext.c),
[main.c](../../main.c) (`configure_standalone_amd64_jit`),
[docs/historical/amd64_port_plan.md](../../docs/historical/amd64_port_plan.md),
[docs/aarch64_guest_plan.md](../../docs/aarch64_guest_plan.md),
[docs/historical/riscv64_guest_plan.md](../../docs/historical/riscv64_guest_plan.md),
[docs/ppc64le_guest_plan.md](../../docs/ppc64le_guest_plan.md),
[docs/CREDITS-aarch64.md](../../docs/CREDITS-aarch64.md),
[docs/guest_architecture_benchmarks.md](../../docs/guest_architecture_benchmarks.md),
`tests/arm64/`, `tests/riscv64/`, `tests/ppc64/`.
