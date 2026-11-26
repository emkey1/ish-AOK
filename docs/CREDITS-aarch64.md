# aarch64 Guest Port — Credits

The aarch64 (native ARM64 guest) work on this branch is motivated by, and in
places directly adapted from, [OpenMinis/ish-arm64](https://github.com/OpenMinis/ish-arm64),
the fork of `ish-app/ish` powering the Open Minis app's "Linux shell" feature.
Both `ish-AOK` and `ish-arm64` are GPLv3 derivatives of the same upstream
(`ish-app/ish`), so attribution here is required by license and is also just
the right thing to do — they did real engineering work identifying that
same-architecture (arm64 guest on arm64 host) dispatch avoids the
cross-architecture translation cost that dominates i386/amd64 emulation.

This file tracks, at the file level, which parts of this port are adapted
from their source versus independently written against AOK-Workstation's own
conventions. Every commit that adapts their code names the source file(s) in
its commit message in addition to the entry added here.

See [aarch64_guest_plan.md](aarch64_guest_plan.md) for the overall plan this
credits file supports.

## Adapted / closely modeled on OpenMinis/ish-arm64 — JIT gadget port

- `jit/guest-arm64/gadgets.h`, `jit/guest-arm64/math.S`, `jit/guest-arm64/control.S`
  — the JIT gadget port itself (aarch64_guest_plan.md's direction change).
  Gadget bodies (`movz`/`movk`/`movn`/`adr`/`load_reg`/`store_reg`) are
  adapted near-verbatim from `asbestos/guest-arm64/gadgets-aarch64/math.S`;
  register-access macros (`load_guest_reg`/`store_guest_reg`/`load_guest_sp`/
  `load_guest_pc`/`load_flags`/`store_flags`) from their `gadgets.h`.
  Mechanical rename: `_pc` → `_ip` (this codebase's i386-guest convention
  for the code-stream pointer, which this file matches exactly — verified
  by direct investigation that `jit_enter`/`gret`/`jit_exit`
  (`jit/gadgets-aarch64/entry.S`) are guest-ABI-agnostic and safely
  reusable unmodified: `jit/jit.c:1444` calls the exact same `jit_enter`
  for amd64 blocks already, and `jit/jit.c:1467` explicitly re-syncs
  `frame->cpu.eip` from `amd64_rip` right after, proving the codebase
  already treats `load_regs`/`save_regs`'s i386-field touching as
  harmless/ignorable for non-i386 guests — no separate entry.S needed).

  **Real adaptation, not mechanical**: their `svc` gadget uses
  `INT_SYSCALL` (0x80, i386's interrupt number); changed to
  `INT_ARM64_SVC`, this codebase's dedicated arm64 SVC interrupt code
  (patch 5). Their TLB fast-path macro (`read_prep`/`write_prep` in their
  `gadgets.h`) assumes `TLB_BITS=13` and a 32-byte `tlb_entry` stride —
  this codebase's is `TLB_BITS=10`/24-byte stride (`emu/tlb.h`) — so
  this file's `read_prep`/`write_prep` is instead adapted from
  `jit/gadgets-aarch64/gadgets.h`'s own already-correct, already-tested
  version (i386's host-gadget TLB lookup), not from OpenMinis' version;
  see that macro's comment for the full reasoning.

  **A real bug found by testing**: the first real run crashed (SIGSEGV)
  with zero output. Bisected with temporary tracing to `jit_should_yield()`
  → `cpu_take_poke()` dereferencing `cpu->poked_ptr`, which was never
  initialized for arm64 tasks — `cpu_run_to_interrupt()`'s arm64 branch
  returned early, before the line (shared with i386/amd64) that sets
  `cpu->poked_ptr = &cpu->_poked`. One-line fix; see the commit for
  reasoning. Confirms the value of testing against the actual JIT
  execution path rather than only reading code for correctness.

- `jit/offsets.c`'s new `arm64()` function and `MACRO(TLB_BITS)`/
  `MACRO(PAGE_BITS)` additions — independently written (this codebase's
  own `offsets.c`/`staticdefine.sh` mechanism has no OpenMinis
  equivalent to adapt from), but exists specifically to let the ported
  gadget files above reference this codebase's actual struct layout via
  symbolic `CPU_arm64_*` names, aliased to the bare `CPU_x0`/`CPU_sp`/
  `CPU_pc`/`CPU_nzcv` names their gadget bodies use (see gadgets.h's
  `#undef CPU_sp` comment for why a plain alias isn't safe — a real
  collision with i386's own `sp` field, caught by the compiler, not
  hypothetical).

- `jit/gen.c`'s `gen_start_arm64`/`gen_step_arm64` and `jit/jit.c`'s
  `jit_block_compile_arm64`/`cpu_step_to_interrupt_arm64` — independently
  written against this codebase's own amd64-frontend precedent (mirrors
  `gen_start_amd64`/`gen_step_amd64` and `cpu_step_to_interrupt`'s
  jetsam-lock/crash-recovery/block-cache/block-chaining structure almost
  exactly), not adapted from OpenMinis (their `gen.c` fuses decode and
  gadget-emission differently, and they have no equivalent to this
  codebase's jetsam/crash-recovery machinery to adapt from). Field
  extraction inside `gen_step_arm64` deliberately mirrors
  `emu/arm64_interp.c`'s `arm64_execute()` masks exactly — see that
  function's own credits entry for which of those masks came from
  OpenMinis originally.

- `jit/guest-arm64/control.S`'s B/BL/BR/BLR/RET/CBZ/CBNZ/TBZ/TBNZ/B.cond
  gadgets and `jit/guest-arm64/memory.S` (new file, LDP/STP for all three
  addressing modes) — Phase B. Independently written, NOT adapted from
  OpenMinis' equivalents: their control-flow gadgets use `inline_chain`/
  `fake_ip` block-linking (a real optimization requiring their own
  differently-shaped block-chaining scheme); their LDP/STP support is
  split across many perf-specialized fast-path gadgets. This port instead
  reuses this codebase's own `jump_ip[]`/`jumps_from[]` chaining
  (unconditional exit-to-`jit_ret` per branch) and one generic gadget per
  (size, direction) for LDP/STP — correctness-first, matching the
  discipline used throughout this port. `math.S`'s new `add_imm`/
  `sub_imm` gadgets ARE adapted from OpenMinis' math.S (native
  `adds`/`subs` to compute NZCV directly).

  Five real bugs found and fixed getting `tests/arm64/arm64_prologue.s`
  passing — a sign-extension bug in `gen_step_arm64`'s branch-offset
  decoding, a symbol collision against i386's host-only TLB-miss
  assembly, a `_cpu`/x1 register-aliasing bug, an x19 double-booking bug
  (the caller's live LDP/STP offset vs. the shared miss-handler routines'
  own scratch use — the dominant bug class in this port so far), and a
  TLB cross-page label collision. Full writeups in
  `aarch64_guest_plan.md`'s Phase B section and inline in `memory.S`/
  `gadgets.h`.

- `jit/guest-arm64/logical.S` (new file, Phase C part 1) — independently
  written, not adapted from OpenMinis' equivalent. Decodes the bitmask
  immediate at JIT-compile time via `arm64_decode_bitmask_imm` (moved
  from `emu/arm64_interp.c` into the shared
  `emu/arch/arm64/decode.h` — see that header for why: same "one
  implementation, not two chances to get it wrong" discipline the
  branch-offset sign-extension bug taught this port), so the gadget
  itself never reimplements ARM's DecodeBitMasks pseudocode a third
  time (interpreter, gen.c, AND assembly). ANDS uses the host's own
  native `ands` instruction directly — its NZCV semantics fall out
  correctly for free on a same-architecture host.

- `jit/guest-arm64/dpreg.S` (new file, Phase C part 2) — independently
  written, not adapted from OpenMinis' equivalents. Uses AArch64's own
  register-form shift instructions (`lsl/lsr/asr/ror` with a register
  shift amount) since the shifted operand's *value* is only known at
  runtime, letting the host CPU compute the shift natively rather than
  software-emulating it — same same-architecture-dispatch principle
  applied throughout this port. Confirmed against the ARM ARM (not
  discovered by a failing test) that the shifted-register encoding of
  add/subtract never treats register 31 as SP, unlike the immediate
  form.

- `jit/guest-arm64/memory.S`'s single-register load/store gadgets,
  `jit/guest-arm64/atomics.S`, and `jit/guest-arm64/dpextra.S` (Phase C
  part 3) — independently written, not adapted from OpenMinis. The
  atomics deliberately mirror this codebase's OWN interpreter's
  excl_addr/excl_val monitor semantics (emu/arm64_interp.c) rather than
  OpenMinis' approach or host exclusives (see atomics.S's header for
  why host ldxr/stxr would be a livelock risk). The bitfield double-
  shift lowering, cond_*-gadget w10 handoff, and extended-register
  add/sub are all derived directly from the ARM ARM.

- `jit/guest-arm64/simd.S` (Phase D part 1), the arm64 native syscall
  dispatch (`handle_arm64_native_syscall`, kernel/calls.c), the arm64
  clone/TLS/stat fixes (kernel/fork.c, fs/stat.[ch]) — independently
  written against this codebase's own amd64 precedents (the
  native-syscall switch mirrors `handle_amd64_native_memory_syscall`;
  the stat marshalling mirrors `stat_convert_amd64`), not adapted from
  OpenMinis. The AdvSIMDExpandImm compile-time expansion
  (jit/gen.c's `gen_arm64_expand_imm`) is a direct transcription of the
  ARM ARM pseudocode.

## Adapted / closely modeled on OpenMinis/ish-arm64 (interpreter, patches 3-5)

- `emu/arch/arm64/decode.h` — adapted near-verbatim from their
  `emu/arch/arm64/decode.h`: condition-code enum and evaluation
  (`arm64_cond_check`), instruction classification, sign extension, and
  branch/ADR immediate extraction. Field names renamed with an `arm64_`
  prefix (`cpu->arm64_nf` instead of `cpu->nf`) because this codebase's
  `struct cpu_state` is shared across i386/amd64/arm64 tasks and bare
  `zf`/`cf` already name the i386 eflags-bitfield fields; `addr_t` (32-bit,
  i386-specific here) swapped for `guest_addr_t` (64-bit) in
  `arm64_read_insn`; added a `guest_abi_range_valid` bounds check that has
  no equivalent in their arm64-only build.
- `emu/arm64_interp.c` — the bit-field masks and layouts for the trickier
  encodings (load/store exclusive LDXR/STXR, CAS, LDP/STP, LDR/STR
  unsigned-immediate, MOVN/MOVZ/MOVK, ADD/SUB immediate, Logical immediate
  AND/ORR/EOR/ANDS, B/BL/B.cond/CBZ/CBNZ/TBZ/TBNZ/BR/BLR/RET, ADR/ADRP, SVC)
  are adapted from their `asbestos/guest-arm64/gen.c`, cited by source line
  number in the comment above each decode site in this file. Also includes
  `arm64_decode_bitmask_imm`, a direct translation of their
  `decode_bitmask_imm` (gen.c:1018) — itself a transcription of the ARM
  ARM's DecodeBitMasks pseudocode, so algorithmically an ISA fact, but the
  code structure (loop-based highest-set-bit search, variable naming) came
  from their file. Their file is a JIT code generator
  (decode and gadget-emission fused together); only the decode/field-layout
  logic is adapted here — the actual arithmetic and flag-computation
  semantics are original, since a same-architecture JIT has no portable-C
  expression of instruction semantics to adapt (the host CPU executes the
  real instruction natively; there's nothing written down in C to copy).
  Their exclusive-monitor comment ("For single-threaded emulation, we can
  treat these as simple loads/stores") is explicitly NOT followed — this
  codebase is SMP-aware, so LDXR/STXR here implement local-monitor-plus-
  value-check semantics instead, flagged in comments as needing real
  differential testing once multiple guest threads are exercised.

  **Validated the adaptation was worth doing**: patch 5's real-hardware
  test binaries (`tests/arm64/*.s`) caught a genuine ordering bug — CAS's
  encoding also matches the broader load/store-exclusive mask, and this
  file's if-chain checked load/store-exclusive first, misclassifying every
  CAS instruction. OpenMinis' `gen.c` checks CAS before load/store-exclusive
  (line 2134 vs 2942) for exactly this reason; the masks I adapted from them
  were correct, but I got the ordering backwards when transcribing them into
  a flat if-chain instead of their structure. Fixed by matching their
  ordering. See `tests/arm64/README.md`.

## Independently written, informed by their design

- `kernel/abi/arm64.h`, the `GUEST_ABI_ARM64` enum value, and the
  `guest_abi_desc`/`guest_abi_vm_layout` cases in `kernel/abi.h` — written
  against AOK-Workstation's existing `guest_abi` dispatch machinery (which
  OpenMinis' fork does not have; their arm64 support is a compile-time
  `#ifdef GUEST_ARM64` build variant, not a per-task runtime ABI). The
  48-bit VA size mirrors their choice; the *stack placement* within it does
  not (patch 2 initially copied their top-of-space placement to dodge a V8
  CodeRange collision, then reversed it once patch 4's syscall-table work
  made concrete that this codebase's narrow `dword_t`-arg `syscall_t` needs
  a low stack/heap the same way amd64's does — see the dedicated commit
  fixing this).
- `struct cpu_state`'s arm64 register fields (`emu/cpu.h`) — structurally
  similar to their `emu/arch/arm64/cpu.h` (same register set, same choice to
  reuse a 128-bit union for V0-V31) but written as siblings within a shared
  multi-ABI struct rather than a per-arch-build struct.

  **Reversal**: originally deliberately *without* their redundant packed-
  `nzcv`-kept-in-sync-with-decoded-flags representation, on the reasoning
  that a single representation avoids a sync-drift bug class. Reverted once
  the project's direction changed (see below) — `cpu->arm64_nzcv` is now a
  single packed field in their exact PSTATE bit layout (31:28 = N,Z,C,V),
  matching their design, because the reason for the original choice
  (interpreter-first correctness) no longer applies once the interpreter is
  no longer the target of ongoing work.
- `emu/arm64_interp.c`'s overall structure (register/memory access helpers,
  step-and-dispatch loop, `cpu_run_to_interrupt_arm64` driver) mirrors this
  codebase's own `emu/amd64_interp.c`, not OpenMinis' code — they have no
  interpreter (their arm64 support is JIT-only).

  **Direction change**: `aarch64_guest_plan.md` originally called for
  interpreter-first, JIT-later (mirroring the amd64 port's discipline). The
  project's direction changed to directly porting OpenMinis' JIT gadget set
  instead — see the plan doc's "JIT gadget port (direction change)" section
  and `jit/guest-arm64/` (new). `emu/arm64_interp.c` is left as-is
  (committed, still builds, still passes its regression tests, already
  caught two real bugs) but is no longer being extended with new
  instruction coverage. Unlike the amd64 port, there is no interpreter
  fallback wired into the gadget path — matching i386's own no-fallback
  precedent, per the sibling-fork research that motivated this change.

## Adapted (curation) / independently written (implementation): syscall table

`kernel/calls.c`'s `arm64_syscall_table` — Linux aarch64 syscall *numbers*
(asm-generic based) are ABI facts, not copyrightable expression, so the
numbering itself isn't "adapted from" anyone. What IS adapted from
OpenMinis' `kernel/arch/arm64/calls.c` is the **curation**: which ~120 of
~450 syscall slots are worth a real implementation versus a stub, reflecting
their real-world experience of what busybox/musl/glibc/Node/Python/git
actually call. That curation choice is credited even though the underlying
facts (numbers) aren't copyrightable — it's real engineering judgment, and
crediting it is simply accurate and fair.

The actual implementations wired into each slot are 100% AOK-Workstation's own —
OpenMinis' function names don't exist in this codebase at all (different
kernel/ entirely). Every slot reuses an existing i386/amd64 `sys_*`
function, verified against the exact names already used in this file's
`i386_syscall_table`/`amd64_syscall_table` above it. Notably, the several
`_amd64`-suffixed functions reused here (`sys_stat_amd64`,
`sys_mmap_amd64`, `sys_newfstatat_amd64`, etc.) are named for amd64 but
implement the Linux 64-bit-ABI shape (64-bit stat, direct 64-bit pointer
args) generically, not anything x86-specific — safe and correct to reuse
verbatim for arm64's identical 64-bit-ABI requirements, once the stack/heap
placement fix above made that reuse's narrow-argument-marshalling
assumption hold for arm64 too.

## Independently written, informed by their design (continued) — scalar fixed-point convert

- `jit/guest-arm64/simd_shift.S`'s `scvt_fixed_sd` macro
  (`sscvtf_fix`/`sucvtf_fix`/`sfcvtzs_fix`/`sfcvtzu_fix`) and the
  corresponding decode added to `jit/gen.c`'s "AdvSIMD scalar shift by
  immediate, remaining opcodes" block (opcodes 0x1c/0x1f) — rtorrent's MSE
  Diffie-Hellman/SHA-1 path SIGILL'd on `ucvtf d2, d2, #0x14` (encoding
  `0x7f6ce442`), which decoded to this instruction class with every other
  opcode handled except these two. OpenMinis' `asbestos/guest-arm64/gen.c`
  (dispatch around their `SHL/SSHR/USHR` scalar-immediate block) and
  `gadgets-aarch64/math.S`'s `scvtf_fixpt_scalar`/`ucvtf_fixpt_scalar`/
  `fcvtzs_fixpt_scalar`/`fcvtzu_fixpt_scalar` already implement this exact
  instruction family — consulting their code (after an initial from-scratch
  attempt) caught a real gap: the first pass here only handled the 64-bit
  (D) element size, but OpenMinis' code handles both S (32-bit) and D
  (64-bit), and disassembling `ucvtf s2, s2, #0xa` on host arm64
  (`0x7f36e442`) confirmed S is a real, separate, architecturally-valid
  encoding — a D-only fix would have left an near-identical crash one
  `immh` bit away. Decode structure (recognizing opcodes 0x1c/0x1f as this
  class's fixed-point-convert pair, S-vs-D dispatch) is adapted from their
  `gen.c`; the gadget body is independently written to match this
  codebase's `vcvt_fixed` convention (precompute the exact 2^n scale as a
  raw float/double bit pattern at JIT-generation time and multiply/divide
  by it once, rather than OpenMinis' approach of reconstructing 2^fbits at
  every execution via runtime shift+convert+`fdiv`) — mechanically simpler
  to fit this file's existing code-stream format and consistent with the
  sibling vector gadget already in this file. Verified against real host
  arm64 hardware (not just self-consistency): both S and D, U and S
  variants, cross-checked bit-for-bit against the identical instructions
  executed natively via `tests/arm64/arm64_ucvtf_fix.s`.
