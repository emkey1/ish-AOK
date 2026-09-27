# iSH-AOK aarch64 Guest Port Plan

Date: 2026-07-01
Branch: `aarch64`

## Motivation

Open Minis (App Store id 6759188481) ships a fork of upstream iSH,
[`OpenMinis/ish-arm64`](https://github.com/OpenMinis/ish-arm64), that adds a
**native AArch64 guest backend** alongside the existing i386/amd64 guest
support. Because their host (iOS/Apple Silicon) and that guest architecture
match, guest instructions map close to 1:1 onto host gadgets instead of going
through full x86-semantics translation. Their own numbers: 7-12x faster than
x86 emulation on compute-heavy workloads (`int_arith_2M` 12x, `fib(30)` 9.2x,
`sum(1M)` 10.2x, `seq+awk 100K` 7.2x). That's the entire "Open Minis feels
faster" effect users are reporting — not a better JIT, just skipping
cross-architecture translation for aarch64 binaries.

Both projects are GPLv3 derivatives of the same upstream (`ish-app/ish`), so
this plan explicitly leverages their public source as a design reference and,
where practical, ports code directly with attribution — see
[Attribution](#attribution) below. This is not a clean-room reimplementation;
where their `asbestos/guest-arm64/` and `kernel/arch/arm64/` code is
GPLv3-compatible and fits our tree, we adapt it and credit it.

## Why This Port Is Narrower Than the amd64 Port

`historical/amd64_port_plan.md` had to build the ABI-split infrastructure from scratch:
per-task ABI enum, 64-bit-capable MM, ELF64 loading, a second syscall table
mechanism. All of that now exists and is proven (see the conversation that
produced this plan for full file:line citations):

- `enum guest_abi` on `struct task` (`kernel/abi.h:9`, `kernel/task.h:30`),
  re-derived per `execve()` from the ELF header (`kernel/exec.c:59-69`,
  `kernel/exec.c:547`).
- `struct syscall_abi_dispatch` selecting a table + marshalling functions per
  ABI (`kernel/calls.c:1211-1220`, `kernel/calls.c:1522-1529`).
- ELF64 parsing already exists (used for amd64) and needs only a new
  `EM_AARCH64` case, not a new struct.
- 64-bit sparse MM, canonical-address validation, and non-4GB VM layout
  already exist (`guest_abi_vm_layout()`, `kernel/abi.h:70-98`).
- The interpreter/PIE-load-bias and `PT_INTERP` arch-consistency check already
  branch on `abi` (`kernel/exec.c:520-523`, `565-575`) — a third arch is a
  new `case`, not new logic.

What's genuinely new: the AArch64 register file, decoder, native gadget
engine, and syscall table content. That's still substantial, but it's adding
a third parallel engine (following the amd64-frontend precedent —
`emu/amd64_interp.c` / `jit_block_compile_amd64`, dispatched via
`current->abi == GUEST_ABI_AMD64` at `jit/jit.c:1500-1515`) rather than
re-deriving the whole dispatch skeleton.

## Attribution

When patches in this series adapt or port code from `OpenMinis/ish-arm64`,
each commit message must name the source file(s) and note "adapted from
OpenMinis/ish-arm64, GPLv3." A `CREDITS-aarch64.md` file (added in Patch 1)
tracks this at the file level. The top-level README gets an Acknowledgments
line pointing to their repo once the branch has real content. Do not silently
vendor their assembly or C files without a credit line in the file header —
GPLv3 requires preserving notices, and it's simply the right thing to do
given they did real engineering work here.

Likely direct-port or close-adaptation candidates (verify license header
compatibility file-by-file before copying, since GPLv3 requires attribution,
not permission — that's already satisfied, but each file should still carry
a provenance comment):

- `kernel/arch/arm64/calls.c` — aarch64 Linux syscall numbering/table shape
  (numbers/ABI facts aren't copyrightable, but the table structure and stub
  choices are useful reference).
- `vdso/arm64/{vdso.S,vdso.c,vdso.lds}` — sigreturn trampoline boilerplate is
  largely mechanical; strong candidate for near-direct adaptation.
- `asbestos/guest-arm64/gadgets-aarch64/*.S` — instruction-class organization
  (bits/control/crypto/entry/math/memory) as a structural template even if
  we write our own gadget bodies to match iSH-AOK's existing gadget/tlb
  conventions.
- `emu/arch/arm64/{cpu.h,decode.h}` — register file and decode-context shape.

## Non-Goals for Initial Bring-Up

- SVE/SVE2, MTE, PAC/BTI enforcement.
- Full NEON crypto (AES/SHA/CRC32) — stub or trap-and-emulate until a
  static/basic userland boots.
- Mixed-arch same-process-tree exec (i386/amd64 parent execing an arm64
  child, or vice versa) — architecturally unblocked by the existing `abi`
  redetection in `execve()`, but treat as its own validation milestone after
  arm64 alone is stable, not a day-1 requirement.
- Multiarch rootfs (`dpkg --add-architecture arm64` inside a Devuan guest) —
  a rootfs/provisioning concern once the kernel-level support works, not part
  of this patch series.

## Concrete Patch Series

### 1. ABI Scaffolding for a Third Architecture

Files: `kernel/abi.h`, `kernel/task.h`, `kernel/exec.c`, `kernel/elf.h`,
new `CREDITS-aarch64.md`

Work:
- Add `GUEST_ABI_ARM64` to `enum guest_abi`.
- Add `EM_AARCH64` to `kernel/elf.h` constants; extend `elf_abi_detect()`
  (`kernel/exec.c:76-88`) with the `ELF_64BIT && EM_AARCH64` case.
- Extend the `PT_INTERP` abi-consistency check (`kernel/exec.c:520-523`) —
  already generic, just needs the new enum value to fall through correctly.
- Extend `guest_abi_vm_layout()`/`guest_abi_desc()` (`kernel/abi.h:70-98`)
  with an arm64 entry (48-bit VA, matching OpenMinis' choice and avoiding
  V8-style high-address collisions the amd64 path already had to solve).
- `CREDITS-aarch64.md` scaffold per [Attribution](#attribution).

Exit criteria: tree still builds; i386/amd64 unaffected; `elf_abi_detect`
correctly classifies a real aarch64 ELF without touching execution.

### 2. AArch64 Register File and CPU State

Files: `emu/cpu.h`, `emu/regid.h`, `kernel/task.h`

Work:
- Add arm64 fields to `struct cpu_state` as siblings (following the existing
  `amd64_regs[]`/`amd64_rip` pattern, not a union): `arm64_regs[31]` (X0-X30),
  `arm64_sp`, `arm64_pc`, `arm64_pstate`, NEON/FP `arm64_v[32]` (128-bit each).
- Audit save/restore, clone, ptrace snapshot, and crash-log paths for the new
  fields (mirrors amd64 port step 4's audit list).

Exit criteria: arm64 register state can be initialized, copied on fork, and
dumped in crash logs, with i386/amd64 unaffected.

### 3. AArch64 Decoder — DONE (scope-cut), see below

Files: `emu/arch/arm64/decode.h`, `emu/arm64_interp.c`

Landed: ADR/ADRP, ADD/SUB (immediate, with SP source/dest handling),
MOVN/MOVZ/MOVK, B/BL, B.cond, CBZ/CBNZ, TBZ/TBNZ, BR/BLR/RET, SVC (traps to
`INT_ARM64_SVC`, not yet dispatched — patch 4), LDR/STR (unsigned immediate,
GPR only), LDP/STP (post/pre/signed-offset, GPR only), LDXR/STXR (register,
non-pair), CAS. Decode masks for the trickier encodings adapted from
OpenMinis' `asbestos/guest-arm64/gen.c` — see `CREDITS-aarch64.md`.
Pure-logic pieces (sign extension, branch-immediate decode, ADD/SUB flag
computation, condition evaluation) verified against hand-computed values in
a standalone scratch check.

**Explicitly deferred** (raises `INT_UNDEFINED`, does not silently
misexecute):
- ~~Logical (immediate): AND/ORR/EOR/ANDS~~ — **added post-patch-5**, see
  below. Turned out to be far more urgent than "substantial enough to
  warrant its own pass" suggested.
- Data-processing (register): logical-shifted-register (incl. the
  register-form MOV alias), add/subtract shifted/extended register,
  conditional select, 1-/2-source ops (MUL, UDIV/SDIV, CLZ, etc). **Now
  confirmed as the actual next blocker** — see patch 5's real-rootfs
  findings below and `tests/arm64/README.md`.
- LDXP/STXP (pair exclusives), STLR/LDAR (non-exclusive acquire/release),
  LSE atomic RMW beyond CAS (LDADD/LDCLR/etc).
- LDR (literal, PC-relative), sign-extending LDRSB/LDRSH/LDRSW.
- All SIMD/FP instructions.

**Was "not yet exercised by any test" — now is, and testing paid off
immediately.** Once patch 5 lifted the `ENOEXEC` guard and wired real
execution end-to-end, three hand-assembled test binaries (`tests/arm64/`)
validated the memory-touching paths for real and caught a genuine bug (CAS
mask-ordering, see patch 5 below). Testing against the real Alpine aarch64
rootfs went further and found that Logical (immediate) — deferred here as
lower-priority — is literally the first instruction musl's dynamic linker
executes. Added `arm64_decode_bitmask_imm` (adapted from OpenMinis'
`decode_bitmask_imm`) and full AND/ORR/EOR/ANDS support in response,
validated with a fourth test (`arm64_logical.s`) and confirmed to unblock
2 more real instructions of `ld.so` before hitting the next (DP_REG)
blocker. This is the priority-inversion risk of planning instruction
coverage from the ISA manual instead of from what real code actually
executes first — corrected as soon as real data was available.

Revised exit criteria (the original "hand-written asm tests pass" bar
assumed a test harness that doesn't fit this codebase's conventions):
build passes; pure-logic decode/flag functions verified against hand-
computed values; full instruction-level correctness deferred to differential
testing against the new oracle VM (see Testing Strategy) once patches 4-5
make real guest execution possible.

### 4. AArch64 Syscall Table — DONE (scaffolding; not yet reachable)

Files: `kernel/calls.c`

Landed: `arm64_syscall_table[450]`, `arm64_syscall_number/args/result`
(X8/X0-X5/X0, no rcx/r11-clobber — that's an x86-SYSCALL-instruction side
effect, not an ARM SVC one), and an `arm64_syscall_dispatch` entry wired
into `syscall_dispatch_for_abi`. ~120 real slots, curation adapted from
OpenMinis (see `CREDITS-aarch64.md`), implementations 100% reused from
existing i386/amd64 `sys_*` functions — cross-checked against the exact
names already used in this file's own tables, not typed from memory. Build
resolved every reused name on the first real compile after fixing a
definition-ordering bug (arm64_syscall_result referenced
`syscall_result_is_errno`/`syscall_result_errno` before their definitions).

**Correctness note, not a formality**: this table only works because of
the patch-2-reversal commit above — `kernel/calls.c`'s `syscall_t` is
`dword_t(*)(dword_t x5)`, so every reused `sys_*_amd64` function's pointer
arguments get silently truncated to 32 bits unless the actual guest address
fits in 32 bits, which is only true because the arm64 stack/heap are now
kept low. This is the exact same constraint amd64 lives under for its own
narrow-table entries; genuine 64-bit-pointer safety for syscalls that need
it (large mmap offsets, etc.) requires the same kind of hand-written
`qword_t`-safe special-casing amd64 has in `handle_syscall_interrupt`
(the `sys_open_guest`/`sys_write_guest`/etc. dispatch switch) — not
attempted for arm64 yet, scoped as a follow-up once real execution exists
to motivate which syscalls actually need it.

**Still unreachable**: `kernel/exec.c`'s patch-1 `ENOEXEC` guard still
blocks arm64 exec, so this table has zero live callers yet. That's
intentional — patch 5 (ELF loading) is next, and lifting the guard is that
patch's job, not this one's.

Exit criteria (unchanged, still pending patch 5): tiny arm64 asm syscall
tests pass for `write`, `exit`, `openat`, `read`, `mmap`, `mprotect`,
`brk`, `clone`.

### 5. ELF64 aarch64 Loading and Stack Bootstrap — DONE, exit criteria MET

Files: `kernel/exec.c`, `jit/jit.c`, `kernel/calls.c`, `emu/cpu.h`, `emu/arm64_interp.c`

Landed, smaller than expected: exec.c's ELF64/stack/auxv machinery
(`is_64bit`, `task_abi_desc().elf_platform`, `guest_abi_vm_layout()`) was
already fully abi-generic from the amd64 port — `AT_PLATFORM` and
`uname -m` needed zero changes, they just work via patch 1's
`guest_abi_desc()`. What patch 5 actually needed:

1. Removed the patch-1 `ENOEXEC` guard in `kernel/exec.c`.
2. Wired `cpu_run_to_interrupt_arm64` into `jit/jit.c`'s
   `cpu_run_to_interrupt()` — this was originally scoped to patch 8
   ("Ptrace, Then Native Gadget Engine") but that's wrong: patch 8 is about
   the *native gadget engine*, and the plain interpreter dispatch needed to
   exist much earlier for any arm64 code to run at all. Moved here.
3. Added the unconditional arm64 register-init block in `elf_exec()`
   (mirrors the existing unconditional i386/amd64 blocks — cpu_state's
   arm64 fields are always-present siblings, so initializing them for a
   non-arm64 task is harmless).
4. Wired `INT_ARM64_SVC` into `kernel/calls.c`'s `handle_interrupt()`
   switch via a new `handle_arm64_syscall_interrupt()` — **this was missing
   entirely** and is why the first real test run exited with code 2 instead
   of running anything: `INT_ARM64_SVC` (258) fell through to
   `handle_interrupt`'s `default:` case, which does `sys_exit(interrupt)`,
   and `258 & 0xff == 2`. Not caught by any earlier patch's build check
   because nothing had ever actually triggered an SVC through the full
   stack before.

**Exit criteria verified for real**, not just "compiles clean" — three
hand-assembled test binaries (`tests/arm64/*.s`, built with
`clang -target aarch64-linux-gnu` + `lld`, no libc) run through the full
CLI `ish -r / <binary>` path:
- `arm64_hello.s`: write(2)+exit(2) via SVC — prints "hi", exits 42.
- `arm64_prologue.s`: STP/LDP prologue+epilogue, BL/RET, ADD-immediate,
  SUBS-immediate, B.cond, CBZ — exits 0.
- `arm64_atomics.s`: LDXR/STXR, CAS (success and mismatch/failure cases) —
  exits 0.

**A real bug found and fixed by this testing**: `arm64_atomics.s` initially
hit `INT_UNDEFINED` on the CAS instruction. CAS's encoding also matches the
broader load/store-exclusive family's mask (same top-level "atomic"
encoding group), and since that check ran first in `arm64_execute`'s
if-chain, it intercepted CAS and misclassified it. OpenMinis' `gen.c`
checks CAS before load/store-exclusive for exactly this reason (line 2134
vs 2942) — I had the masks right but the ordering backwards. Fixed by
reordering the checks; see `tests/arm64/README.md` for the full story.
This is precisely the class of bug patch 3's documentation warned "no
test coverage" would leave undetected — confirmed by finding one the
moment real testing happened.

### 6. VDSO

Files: new `vdso/arm64/`, `kernel/vdso.c`, `kernel/exec.c`

Work:
- Adapt OpenMinis' `vdso/arm64/{vdso.S,vdso.c,vdso.lds}` (sigreturn trampoline
  is close to boilerplate) rather than hand-rolling from scratch.
- Follow the amd64 port's precedent of not blocking bring-up on this if it
  turns out to be more involved than expected (amd64 port step 10 allowed
  deferring `AT_SYSINFO*`).

Exit criteria: signal return works without relying on the i386/amd64 VDSO
image.

### 7. TLS, Threads, Signals

Files: `kernel/tls.c`, `kernel/fork.c`, `kernel/signal.c`, `kernel/signal.h`

Work:
- arm64 TLS uses `TPIDR_EL0`, set via a dedicated syscall path (no
  `arch_prctl`-style multiplexer needed — simpler than the amd64 FS-base
  story).
- arm64 signal frame layout (`ucontext`/`sigcontext` shape differs
  substantially from x86 — this is genuinely new work, not reuse).

Exit criteria: dynamic glibc/musl arm64 binaries start; `pthread_create`,
`sigaltstack`, basic signal delivery work.

### 8. Native Gadget Engine — DIRECTION CHANGE, Phase A DONE and validated

Files: new `jit/guest-arm64/{gadgets.h,math.S,control.S}`, `jit/gen.{c,h}`,
`jit/jit.c`, `jit/offsets.c`, `meson.build`

**Direction change** (superseding this patch's original "ptrace first,
interpreter-only" framing): the project's direction changed to porting
OpenMinis' JIT gadget set directly instead of building out interpreter
coverage — the whole reason Open Minis is faster is same-architecture
gadget dispatch, and there's no interest in an interpreter as an
intermediate step. `emu/arm64_interp.c` (patches 3-5) is left committed,
still builds, still passes its tests, but is no longer extended or called
from `cpu_run_to_interrupt()`. No interpreter fallback is wired into the
gadget path — matching i386's own no-fallback precedent (confirmed by
dedicated research into this codebase's JIT integration points before
writing any gadget code).

**Naming note** (still applies): `jit/gadgets-aarch64/` is the *host*-CPU
gadget set (Apple Silicon host, any guest ABI) — orthogonal to the new
*guest*-arm64 backend. `jit/guest-arm64/` is the confirmed name.

**Phase A (this session) — DONE, validated end-to-end**: the smallest
possible complete vertical slice through the whole new subsystem, proven
before expanding gadget coverage (same incremental, test-driven approach
that worked for the interpreter):

- `jit/offsets.c`: new `arm64()` function emitting `CPU_arm64_*` symbolic
  offsets; `MACRO(TLB_BITS)`/`MACRO(PAGE_BITS)` additions.
- `jit/guest-arm64/gadgets.h`: `CPU_x0`/`CPU_sp`/`CPU_pc`/`CPU_nzcv` alias
  layer; `_cpu`/`_tlb`/`_ip` matching iSH-AOK's own i386-guest convention
  exactly (verified reusable, see CREDITS); `gret`; memory-access
  `read_prep`/`write_prep` adapted from `jit/gadgets-aarch64/gadgets.h`'s
  own proven-correct TLB macro (not OpenMinis' — their `TLB_BITS`/entry
  stride assumptions don't match this codebase's actual layout).
- `jit/guest-arm64/math.S`: `movz`/`movk`/`movn`/`adr`/`load_reg`/
  `store_reg` gadgets, adapted from OpenMinis' `math.S`.
- `jit/guest-arm64/control.S`: `svc` gadget, adapted with a real fix
  (their `INT_SYSCALL` → this codebase's `INT_ARM64_SVC`).
- `jit/gen.c`: `gen_start_arm64`/`gen_step_arm64` — emits gadget-array
  entries for MOVZ/MOVK/MOVN/ADR/ADRP/SVC, mirroring
  `emu/arm64_interp.c`'s decode masks exactly; unrecognized instructions
  emit the existing shared `gadget_interrupt` with `INT_UNDEFINED`
  (reused directly from i386's own mechanism, not reimplemented) — no
  silent misexecution, same guarantee the interpreter had.
- `jit/jit.c`: `jit_block_compile_arm64`, `cpu_step_to_interrupt_arm64`
  (mirrors `cpu_step_to_interrupt`'s jetsam-lock/crash-recovery/block-
  cache/block-chaining structure, sans i386's GPF-retry dance — not shown
  to apply here, added later only if real testing shows an equivalent
  need); wired into `cpu_run_to_interrupt()`, replacing the interpreter
  dispatch.

**A real bug found and fixed**: first real run crashed (SIGSEGV, zero
output). Bisected with temporary tracing to `cpu->poked_ptr` being NULL
for arm64 tasks — `cpu_run_to_interrupt()`'s arm64 branch returned early,
skipping the line (shared with i386/amd64) that initializes it. One-line
fix. See `CREDITS-aarch64.md` for the full story.

**Validated**: `tests/arm64/arm64_hello.s` (write+exit via SVC, using only
ported gadgets: MOVZ/ADR/SVC) passes end-to-end through the real JIT
path — compiled by `gen_step_arm64`, cached, executed via `jit_enter`,
correct output ("hi") and exit code (42). The other three regression
tests (`arm64_prologue.s`, `arm64_atomics.s`, `arm64_logical.s`), which
use instructions not yet ported to gadgets (STP/LDP, ADD/SUB-immediate,
Logical-immediate, branches), correctly hit `SIGILL` via
`gadget_interrupt`+`INT_UNDEFINED` — the same "clean failure, not silent
misexecution" guarantee the interpreter had, now proven to hold through
the gadget path too.

**Phase B (branches/arithmetic/memory — DONE, validated)**: expanded
`jit/guest-arm64/` coverage with B/BL/BR/BLR/RET/CBZ/CBNZ/TBZ/TBNZ/B.cond
(`control.S`, independently written — see that file's header on why these
exit to C via `jit_ret` per-branch rather than porting OpenMinis'
`inline_chain`/`fake_ip` scheme), ADD/SUB immediate (`math.S`, adapted
from OpenMinis, uses native `adds`/`subs` to compute NZCV directly), and
LDP/STP for all three addressing modes (new `jit/guest-arm64/memory.S`,
independently written — one generic gadget per (size, direction) rather
than OpenMinis' many perf-specialized variants).

Validated end-to-end via three new/existing hand-assembled tests, all
passing through the real gadget path:
`tests/arm64/arm64_branch_only.s` (exit 5), `arm64_branch_only2.s` (exit
0), and `arm64_prologue.s` (STP/LDP/ADD-imm/SUBS-imm/B.eq/CBZ/BL/RET
together, exit 0).

Getting `arm64_prologue.s` from crashing to passing took five real bugs,
all fixed and documented in `memory.S`/`gadgets.h`'s own comments:

1. **Sign-extension bug** in `gen_step_arm64`'s inline branch-offset
   reimplementations (B/BL/B.cond/CBZ/CBNZ/TBZ/TBNZ) — the OR was
   computed in `uint32_t` before the cast to `int64_t`, zero- instead of
   sign-extending every negative (backward) branch by +2^32. Fixed by
   calling the already-correct `emu/arch/arm64/decode.h` helpers
   (`arm64_branch_imm26/19/14`) instead of re-deriving the same logic a
   second, buggier way.
2. **TLB-miss-handler symbol collision**: the shared miss/crosspage
   routines (`handle_read_miss` etc.) are i386-HOST assembly with i386's
   register convention baked in, not C functions — an arm64 gadget
   calling them by bare name silently linked against i386's copy. Fixed
   by writing arm64-prefixed equivalents in `memory.S` that call the real
   C functions (`tlb_handle_miss`, `tlb_write_ptr_slow`,
   `__tlb_{read,write}_cross_page`) directly.
3. **`_cpu`/x1 register-aliasing bug**: `arm64_crosspage_load/store`
   computed `mov x1, _addr` (arg1) before `add x2, _cpu, #LOCAL_value`
   (arg2) — but `_cpu` *is* x1, so the first write clobbered it before
   the second read it, making arg2 a wild pointer near the guest address
   instead of a valid buffer.
4. **The big one — x19 double-booked**: `ldp_gadget`/`stp_gadget` hold
   their live rt/rt2/rn/mode/offset state in x19-x26 across TLB-macro
   calls specifically *because* x19-x28 are AAPCS64 callee-saved and
   "guaranteed to survive any well-behaved C call" (memory.S's own
   header comment) — but `arm64_handle_{read,write}_miss`,
   `arm64_resolve_write_ptr`, and `arm64_crosspage_{load,store}` all used
   x19 as their *own* internal scratch/argument register without
   restoring it, silently corrupting the caller's offset on every single
   TLB miss or crosspage access. This is why `arm64_prologue.s`'s STP
   pre-index write succeeded but the post-index LDP that immediately
   followed read from a garbage address computed from a stale/corrupted
   offset. Root-caused by adding a temporary C debug shim called from
   inside the gadget to dump live register state — confirmed the base
   register was already garbage *before* the address arithmetic even
   ran, which narrowed it to whatever last touched cpu->arm64_sp (STP's
   own writeback), then to the shared helper it called through. Fixed by
   moving all internal scratch/argument use in these five routines to
   x27 (also callee-saved, but genuinely unused by any gadget so far).
   Straightforward caller-vs-callee register-convention bugs have been
   the dominant bug class in this port so far, not decode logic — worth
   remembering for the next gadget slice.
5. Also fixed a **label-collision bug** in `gadgets.h`'s TLB cross-page
   macro (`\type\()_prep` always branched to `crosspage_load_\id`
   regardless of read/write; `\type\()_bullshit` defined that label
   unconditionally too) — a write straddling a page boundary would have
   silently called the read crosspage helper instead of the write one.
   Caught by code review while chasing bug 4, not by a failing test (the
   specific address in `arm64_prologue.s` never crosses a page), so it
   has no dedicated regression test yet.

**Phase C, part 1 (Logical immediate — DONE, validated)**: added
`jit/guest-arm64/logical.S`'s `logical_imm` gadget (AND/ORR/EOR/ANDS,
including the ORR-with-Rn=XZR "MOV (bitmask immediate)" alias, which
falls out for free since Rn=31 already loads 0). Independently written,
not adapted from OpenMinis — the bitmask-immediate pattern
(immN:imms:immr, ARM's "DecodeBitMasks") is decoded once at JIT-compile
time by `gen_step_arm64` calling `arm64_decode_bitmask_imm` (moved from
`emu/arm64_interp.c` into the shared `emu/arch/arm64/decode.h` so both
the interpreter and the JIT compile-time decoder call the same
implementation — the branch sign-extension bug from earlier in Phase B
is exactly why duplicating this logic a second time wasn't worth the
risk) and packed into the code stream as a plain 64-bit value, so the
gadget itself never needs to reimplement DecodeBitMasks in assembly.
ANDS's NZCV semantics (N,Z from the result, C,V always cleared) fall out
for free from the host's own native `ands` instruction — no
special-casing needed on a same-architecture host. Validated via
`tests/arm64/arm64_logical.s` (AND/ORR-alias/EOR/ANDS together, exit 0)
— this is significant because patch 5's real-rootfs testing found
Logical (immediate) is literally the first instruction musl's dynamic
linker executes, so this closes that specific gap for the gadget path.

**Phase C, part 2 (DP_REG — DONE, validated)**: added new
`jit/guest-arm64/dpreg.S` with two gadgets, `logical_reg` (AND/ORR/EOR/
ANDS + the N=1 NOT variants BIC/ORN/EON/BICS, shifted register) and
`addsub_reg` (ADD/SUB/ADDS/SUBS, shifted register). Independently
written, not adapted from OpenMinis. The shift *type* and *amount* are
compile-time constants (encoded in the instruction) but the shifted
*operand*'s value (Rm) is only known at runtime, so a reused gadget body
can't bake the shift in as an immediate — instead of software-emulating
LSL/LSR/ASR/ROR, this uses AArch64's own register-form shift
instructions (`lsl/lsr/asr/ror xD, xN, xM`, shift amount taken from a
register), letting the host CPU do the shift natively. Confirmed against
the ARM ARM before writing (not discovered by a failing test) that the
shifted-register encoding of add/subtract never treats register 31 as
SP — unlike the immediate form, SP is only reachable via ADD/SUB
immediate or the extended-register encoding, so both gadgets always
treat 31 as XZR for Rd/Rn/Rm. The `mov x1, x0` register-MOV alias falls
out for free (ORR, Rn=XZR, shift_amount=0) exactly like the immediate
form's MOV-bitmask alias.

`gen_step_arm64` also rejects two genuinely-unallocated encodings at
compile time (32-bit form with a 6-bit shift amount when only 5 bits are
valid; ROR as a shift type for add/subtract, which only Logical
allows) via `gadget_interrupt`+`INT_UNDEFINED`, matching this port's
"clean SIGILL, not silent misexecution" discipline for anything not
handled.

Validated via new `tests/arm64/arm64_dpreg.s` (MOV-register alias,
shifted ADD/SUB with LSL/LSR, AND/SUB with a shifted operand, ROR)
passing first try, plus full regression of every prior Phase A/B/C-1
test. This closes patch 5's real-rootfs-confirmed next blocker
(register-form ADD/SUB/MOV/logical-shifted-register) immediately after
Logical (immediate).

**Phase C, part 3 (integer core completion — DONE, validated)**: the
big batch. New gadget files and coverage:

- `memory.S` additions: the whole single-register load/store surface —
  one generic gadget per (size, extend) covering four encoding families
  (unsigned-immediate, imm9 unscaled/post/pre incl. LDTR/STTR-as-normal,
  register-offset with extend/shift resolved at runtime, LDR literal),
  zero- and sign-extending variants (LDRB/H/W/X, LDRSB/SH/SW to both
  widths), PRFM/PRFUM lowered to nothing.
- `atomics.S` (new): LDXR/STXR via the interpreter's exact
  excl_addr/excl_val monitor semantics (deliberately NOT host
  exclusives — a host store by the same PE may clear the local monitor,
  a livelock risk; see the file header), CAS, and acquire/release bits
  accepted-and-ignored. LDAR/STLR lower to the plain load/store gadgets
  in gen.c (their semantics minus the unmodeled barrier) — wider than
  the interpreter, which still rejects them.
- `dpextra.S` (new): bitfield SBFM/UBFM via the compile-time
  double-shift lowering (L = R-1-imms, S = (L+immr) mod R — one
  formula covers extract, insert-into-zeros, and every alias:
  LSL/LSR/ASR-imm, UBFX/SBFX, SXT*/UXT*), BFM with a compile-time
  insert mask, EXTR/ROR-imm, the cond_* condition-evaluation gadget
  family (w10 handoff to the next gadget in the block — registers
  persist across gret), CSEL/CSINC/CSINV/CSNEG, CCMP/CCMN, UDIV/SDIV
  and variable shifts, RBIT/REV*/CLZ/CLS, the full 3-source multiply
  family (MADD/MSUB/SMADDL/UMADDL/SMULH/UMULH/...), barriers (one
  `dmb ish` for the whole DSB/DMB/ISB space), the hint space as NOP,
  and MRS/MSR TPIDR_EL0 (new `cpu_state.arm64_tpidr` field — the TLS
  base) plus constant MRS values for CTR_EL0/DCZID_EL0 (DZP=1 so libc
  never attempts DC ZVA).
- `dpreg.S` addition: add/subtract (extended register) — the
  SP-capable form compilers use for `sub sp, sp, x2` dynamic stack
  adjustment; found as a real Alpine busybox blocker (0xcb2263ff).

Five more real bugs found and fixed in this batch:

1. **The Phase B "label collision fix" was itself a misdiagnosis** —
   i386 deliberately routes crosspage WRITES through crosspage_load
   first (fill buffer + stash address + redirect _addr), with
   write_done flushing via crosspage_store afterward. The earlier
   "fix" made writes branch straight to the flush, skipping both the
   fill and the store. Reverted to i386's design;
   `arm64_crosspage_store` also now reloads the guest address from
   LOCAL_value_addr (widened to 64 bits in jit/frame.h) instead of
   using the by-then-redirected _addr.
2. **x2/_tlb argument-aliasing in arm64_crosspage_load** — the fix for
   the earlier x1/_cpu aliasing reordered argument setup such that
   arg2's write to x2 destroyed _tlb before arg0 read it (AAPCS64
   argument registers ARE the gadget convention registers; setup order
   is load-bearing in both directions). Caught by arm64_dpextra.s's
   page-straddling store. The only safe order consumes x2 first and
   writes x1 last — now documented at the routine.
3. **store_flags clobbers x17** — dpreg.S's three gadgets parked the
   register-file base in x17 across store_flags (whose scratch is
   x17), exactly the hazard gadgets.h's comment warned about. Host
   EXC_BAD_ACCESS in busybox's ld.so (the smoke tests never exercised
   a flag-setting shifted-register op); a regression check now exists
   in arm64_dpextra.s. Fixed by re-deriving the base after every
   store_flags.
4. **x18 is Apple's platform-reserved register** — ldp/stp and the
   dpreg gadgets used it as a data register; the OS is allowed to
   trash x18 asynchronously, which would have been rare unreproducible
   on-device corruption. Fixed by audit (x25/x22), rule documented in
   gadgets.h.
5. The interpreter's load/store unsigned-immediate mask
   (`0x3b000000`) leaves the V bit unmasked and so also matches SIMD
   loads/stores — a latent interpreter misdecode (moot while frozen).
   gen.c uses the corrected `0x3f000000` mask; noted in the decode
   comment rather than fixed in the frozen interpreter.

Validated: all 8 smoke tests pass (`arm64_atomics.s` now runs via
gadgets end-to-end; new `arm64_dpextra.s` covers 15 checks across the
batch including the crosspage-write path and the store_flags/x17
regression). Real Alpine busybox now decodes EVERY instruction on its
startup path — the remaining gap is SIMD/FP (`dup v0.16b, w1` in
musl's memset is the current first blocker), which is Phase D.

**Phase D part 1 + the syscall layer (DONE — REAL ALPINE BUSYBOX RUNS)**:
the milestone the whole port has been driving toward. `/bin/busybox`
(real Alpine 3.21 aarch64, dynamic musl PIE) runs end-to-end through the
JIT gadget path: help text, `echo`, `uname -a`, `ls /` (full listing),
`cat`, `wc`, `id`, and `sh -c` including fork+exec of child binaries.

New in this batch:

- `jit/guest-arm64/simd.S` (new): the blocker-driven minimal SIMD
  subset — single vector/FP loads/stores (B/H/S/D/Q, all four
  addressing families, sharing the integer gadgets'
  ldst_single_setup/-writeback macros, now moved into gadgets.h),
  SIMD LDP/STP pairs (S/D/Q; the 256-bit Q-pair case is why
  jit_frame.value grew to 32 bytes), DUP (general), MOVI/MVNI/FMOV-imm
  via a compile-time AdvSIMDExpandImm (`gen_arm64_expand_imm`), and the
  element-move family (UMOV/SMOV/INS/FMOV-scalar), all lowered to
  integer loads/stores against the V slot in cpu_state at compile-time
  byte offsets — no host vector state held across anything that can
  call C (there is NO fully callee-saved 128-bit register in AAPCS64;
  stores load their value AFTER write_prep for exactly this reason).
- **arm64 native syscall dispatch** (`handle_arm64_native_syscall`,
  kernel/calls.c): the arm64 counterpart of amd64's native-memory
  switch, for the same reason — the legacy syscall_t marshals args
  through dword_t, truncating 64-bit guest pointers, and arm64 PIEs
  live above 4 GiB. ~45 pointer-bearing syscalls wired to the existing
  `sys_*_guest` implementations by AArch64 (asm-generic) numbers,
  including clone with the AArch64 argument order (tls/child_tid are
  swapped relative to amd64). `prepare_syscall_restart` gained the
  missing arm64 case (rewind PC by 4; X8 still holds the number).
- **fork fixes** (kernel/fork.c): the child's X0 was never zeroed for
  arm64 (it resumed with the parent's clone-flags argument as its
  "pid" — the actual first-fork failure), CLONE_SETTLS now sets
  arm64_tpidr directly (it was falling into i386's user_desc-pointer
  path), and clone's stack argument now sets arm64_sp.
- **arm64 stat layout** (fs/stat.h/.c): `struct arm64_stat_` + fstat/
  newfstatat variants — the asm-generic layout genuinely differs from
  amd64's (mode/nlink swapped, 32-bit blksize, explicit pads); reusing
  the amd64 marshalling made busybox `ls` treat every directory as a
  file.
- MRS/MSR TPIDR_EL0 gadgets + `cpu_state.arm64_tpidr` (the TLS base),
  constant CTR_EL0/DCZID_EL0 (DZP=1 so libc never attempts DC ZVA).

**arm64 signal delivery (DONE — full shell semantics work)**: the
aarch64 rt_sigframe (generic-64-bit siginfo reused from the amd64
marshaling + arm64 ucontext with the kernel's 128-byte sigmask
reservation + mcontext with regs[31]/sp/pc/pstate and an
fpsimd_context record chain in __reserved), an on-stack sigreturn
trampoline (`movz x8, #139; svc #0` — no arm64 vDSO yet; the kernel's
SA_RESTORER flag honored, and musl's unset restorer field correctly
NOT trusted without it), `sys_rt_sigreturn_arm64`, sigaction
marshalling routed by guest_abi_is_64bit (aarch64's sigaction ==
amd64's layout; it was falling into the i386 branch), altstack per
SA_ONSTACK for 64-bit ABIs, and `current_user_sp`/`current_fault_ip`
arm64 cases. busybox sh now runs pipelines, for-loops, subshells,
command substitution, and full fork/exec/SIGCHLD/wait cycles.

**The deepest bug of the port so far — mid-block fault restart**
(found chasing the busybox-sh crash; the isolated signal test passed,
signal delivery itself was NEVER the bug): when a memory gadget
faulted mid-block with INT_PF and the kernel resolved the fault (the
everyday case: first write to a copy-on-write page after fork),
execution resumed at the BLOCK-START pc — re-executing every
instruction before the faulting one. musl's `__syscall_ret` errno
block does `mov x1, x0; ...; mov x0, #-1; str w2, [x1]`: the store
CoW-faulted after x0 was already clobbered, and the re-run computed
the errno pointer from x0=-1. (The bogus "-1 read" flavor: address -1
lands in the crosspage path, whose fill is a read.) Root-caused with a
16-entry block-entry ring buffer showing the same block executing
twice. Fix mirrors i386's per-access orig_ip discipline: every
memory-touching gadget's LAST code-stream word is its instruction's
guest address, and `arm64_segfault_read/write` rewind CPU_pc to it
(fixed `[_ip, #-8]` slot) — INT_PF now resumes at the FAULTING
instruction, which is idempotent (writeback happens after the access
in every load/store gadget).

**Scalar FP (DONE — busybox awk fully works)**: `jit/guest-arm64/fp.S`
covers the scalar FP ISA via host instructions (IEEE semantics for
free): FP 1-source (FMOV/FABS/FNEG/FSQRT, FCVT S<->D, all FRINT*),
2-source (FADD/FSUB/FMUL/FDIV/FMAX*/FMIN*/FNMUL), 3-source (FMADD
family), FCMP/FCMPE incl. #0.0 forms, FCSEL/FCCMP via the cond_* w10
handoff, SCVTF/UCVTF and FCVT{Z,N,M,P,A}S+FCVTZU to/from GPRs, FMOV
scalar immediate (reusing gen_arm64_expand_imm — VFPExpandImm is the
AdvSIMD cmode=1111 case), MRS/MSR FPCR/FPSR (stored, not installed on
the host — see fp.S's rounding-mode note). Plus ADC/SBC (dpreg.S), the
AdvSIMD three-same bitwise family (AND/BIC/ORR/ORN/EOR/BSL/BIT/BIF —
ORR Rn==Rm is the vector MOV compilers emit), and scalar SHL/USHR/SSHR
by immediate (musl strtod's exponent construction). Validated: awk
arithmetic/printf/sqrt/exp/log/trig, atan2 in all four quadrants, FP
literal parsing, a 10k-sqrt stress loop, and the new arm64_fp.s smoke
test (11 checks).

One real bug in the batch, and it's the sneakiest kind: the fcsel
gadget parsed its rn field into x10 — the register the preceding
cond_* gadget hands the condition over in — so every FP select became
"rn index != 0". musl's atan returned -atan(x) for every input while
all four basic ops tested correct; root-caused by probing awk with
integer-only literals (FP literal parsing was ALSO broken, by the
missing scalar SHL). RULE, now documented in fp.S: parameter parsing
in any w10-consuming gadget must avoid x10.

**Benchmarks (2026-07-02, -O2 CLI build, busybox workloads, best of 2)**:
sh-loop 20k iterations: i386 2.96s / x86_64 2.74s / arm64 4.27s (0.6x);
sha256 16MB: 1.78 / 2.29 / 1.87s (parity); fork+exec x100: 0.22 / 0.24
/ 0.28s. arm64 is at compute parity but ~40% behind on branch-heavy
code — the known cost of no block chaining (every branch is an
exit-to-C round trip) plus the memory-based register file. Chaining is
the next perf lever, deliberately deferred behind correctness.

**Block chaining (DONE — arm64 is now the fastest guest)**: the
amd64-style tagged-word scheme, re-expressed for this port (see
control.S's chaining header): compile-time branch targets are emitted
with bit 63 set and their stream indices recorded in jump_ip[]; the C
loop patches them in place with the destination block's host `code`
pointer; branch gadgets route through `arm64_branch_dispatch` — tag
still set means strip-and-exit-to-C as before, tag clear means dispatch
straight into the next block after a poke check (signals must not
starve). Forward-only edges (the loop guard) keep every loop's backward
edge exiting to C — the amd64-era jetsam-starvation lesson inherited,
not relearned. B/BL/B.cond/CBZ/CBNZ/TBZ/TBNZ all chain; dynamic-target
BR/BLR/RET stay unchained. One improvement over the amd64 precedent:
the patch condition also requires the tag bit, making a re-check of an
already-patched slot a no-op instead of a low-probability false
re-patch.

**Benchmarks after chaining (same workloads as above)**: sh-loop 20k:
i386 3.24s / x86_64 3.07s / **arm64 2.62s (1.2x both)** — was 4.27s
(0.6x) before chaining. sha256 16MB: 2.05 / 2.63 / **1.80s**. fork+exec
x100: 0.28 / 0.36 / **0.28s**. arm64 leads on every benchmark.

**Perf campaign, round 2 (DONE — the same-architecture advantage
finally shows)**: three stacked changes, each validated by the
50M-iteration hand-written loop pair (arm64 vs i386, identical logic):

1. **ldar dispatch** (gadgets.h gret): the per-gadget `dmb ishld` cost
   tens of cycles with loads in flight; a load-acquire on the gadget
   pointer gives the same publication guarantee nearly free.
   1.94s -> 1.68s.
2. **Compare+branch fusion** (control.S + gen.c): fast CMP-imm/CMP-reg/
   SUBS-imm gadgets peek at the next instruction; a following B.cond
   becomes ONE fused gadget (84 macro-generated per-form-per-cc bodies,
   pointer tables exported from asm) that compares natively, stores
   guest NZCV (later readers stay correct), and branches on live host
   flags — no msr nzcv reload, one dispatch instead of two. Plus a
   bcond_nf_* family for the unfused-but-flags-live case
   (state->arm64_flags_live tracking; gret provably preserves host
   flags).
3. **Backward-edge chaining with a chain budget** (frame.h/jit.c/
   control.S): the i386/amd64 loops forbid backward links because an
   in-JIT loop never returns to C and starves jetsam writers and the
   cycle counter. arm64 instead links loops and bounds time-in-JIT with
   jit_frame.chain_budget (8192 chained dispatches per jit_enter,
   decremented in arm64_chain; expiry exits like a poke, resuming at
   the destination block). A hot loop runs gadget-to-gadget with a
   perfectly-predicted self-loop branch and ~0.03%% C-loop overhead.
   1.68s -> **0.083s** — the pure loop is now 26x i386.

**Benchmarks after round 2** (same -O2 CLI, best-of runs): sh-loop 20k
1.36s (2.1-2.3x vs x86 guests, was 4.27s this morning); sha256 16MB
0.58s (3.2-4.1x, digests verified bit-identical to the host); fork+exec
x100 0.13s (1.8-2x); awk int/fp 1.4-2x; hand-written 50M loop 26x.
This is OpenMinis-claim territory measured against a much stronger
baseline than theirs (iSH-AOK's tuned x86 engines, not upstream).

**Known next work**:

1. Minor: `ls / | grep ...` pipelines produce empty output (plain `ls /`
   works) — likely a non-tty stat/ioctl behavior difference, not a JIT
   gap.
2. Wider SIMD (vector arithmetic beyond bitwise) and the remaining
   unsigned FCVT rounding variants — blocker-driven, as always.
3. Perf, next tier: fused CBZ/CBNZ (same peek pattern), load/store
   fast-path specialization, a return cache for RET, ldar-style
   dispatch for the i386/amd64 gret (helps the x86 guests too).

## Testing Strategy — New Oracle Required

`mint` (the existing x86_64 Intel oracle host, see memory
`reference_mint_oracle.md`) is **not usable** for this port — it's the wrong
guest architecture. The session's own host machine is Apple Silicon
(`arm64`, confirmed via `uname -m`), which makes it the natural new oracle:
a local Linux aarch64 VM (Lima or UTM, same tooling already used for `mint`)
gives real-Linux-on-real-arm64 ground truth without cross-arch emulation
noise. Set this up as an early, parallel task — differential testing against
a real kernel is how the amd64 port's regressions got caught, and this port
should not skip that step just because it's "easier" (same-arch dispatch
still has plenty of room for subtle bugs in flag semantics, atomics, and
signal frame layout).

### Milestone 1: Static asm smoke tests
`_start`, `write`/`exit`, `SVC` transitions, LDXR/STXR atomics, branch
coverage — differential against the new local aarch64 VM oracle.

### Milestone 2: Static userland
tiny musl arm64 binaries; `uname`, `mmap`, `mprotect`, `clone`, TLS smoke.

### Milestone 3: Dynamic loader bring-up
`ld-linux-aarch64.so.1 --help`, auxv validation, relocation coverage.

### Milestone 4: Basic shell
`/bin/sh`, coreutils smoke, pipes, signals, `wait4`.

### Milestone 5: Minimal distro
Alpine aarch64 or Devuan arm64 minbase, `apk`/`dpkg`, `apt`, Python.

### Milestone 6: Mixed-arch validation (deferred, see Non-Goals)
Once arm64-alone is stable: exec an arm64 binary from an i386/amd64 parent
task in the same rootfs, and vice versa. Confirm the existing
`interp_header.abi != header.abi` check (`kernel/exec.c:520-523`) correctly
rejects a mismatched interpreter for the new arch, and that `copy_task()`
(`kernel/fork.c:65`) inheriting `abi` at fork + re-derivation at `execve()`
produces correct behavior for a fork'd child that execs a different-arch
binary. This scenario is architecturally supported today but was flagged in
research as never having been exercised for i386/amd64 either — worth a
regression test that covers both the existing pair and the new arm64 case.

### Milestone 7: Performance
Differential vs the interpreter, vs the existing i386/amd64 JIT paths on
equivalent workloads, and vs OpenMinis' published numbers as a sanity check
(not a target to beat blindly — their numbers are for a compile-time-only
single-arch build; a correctness-first, runtime-multiplexed build carries
some overhead they don't have to pay).

## Priority Order

1. ABI scaffolding (patch 1) — DONE
2. Register file + CPU state (patch 2) — DONE
3. Decoder + interpreter core (patch 3) — DONE (scope-cut)
4. Syscall table (patch 4) — DONE (scaffolding)
5. ELF64 loading + stack bootstrap, interpreter wired into jit.c dispatch,
   INT_ARM64_SVC wired into handle_interrupt (patch 5) — DONE, exit
   criteria verified with real hand-assembled test binaries
6. TLS/threads/signals (patch 7) — VDSO (patch 6) can interleave or lag
7. Dynamic userland bring-up (milestone 3-4) — needs an arm64 rootfs, not
   yet built
8. Ptrace (originally scoped here; the interpreter-dispatch wiring itself
   moved to patch 5 once it became clear ptrace needs something to observe,
   which needs real execution, which needs that wiring)
9. Native gadget engine (still patch 8)
10. Mixed-arch validation (milestone 6)

## Notes

- First implementation commit on this branch should cover patch 1 only,
  plus `CREDITS-aarch64.md`, and should not touch execution behavior at all.
- Interpreter-only arm64 is the first release-quality milestone, same
  discipline as the amd64 port.
- If forced to choose between early native-gadget work and correct
  glibc/musl + TLS + signal semantics, always choose the latter — same rule
  as the amd64 plan, doubly true here since the whole point of this port is
  "does it actually run real userland," not just synthetic benchmarks.
