# iSH-AOK riscv64 Guest Port Plan (archived, not maintained)

**This file is history.** Pre-implementation port plan dated 2026-07-10,
written in future tense throughout. The riscv64 guest shipped in full
(`GUEST_ABI_RISCV64`, `jit/guest-riscv64/*`), but this document's one concrete
forward-looking design — a generic, all-architecture vendor-extension hook at
`jit/ext.c`/`jit/ext.h` (§5b) — is not what was built; the actual
implementation is the riscv64-specific
[`jit/riscv64_vendor_ext.c`](../../jit/riscv64_vendor_ext.c), which
[`docs/book/ch07-four-guests.md`](../book/ch07-four-guests.md) §7.5 names as
"the reference implementation." Several source comments still point here
(`kernel/abi.h`, `emu/interrupt.h`, `emu/arch/riscv64/decode.h`, `jit/gen.c`,
`jit/riscv64_vendor_ext.c`, `tests/riscv64/riscv64_vendor_ext.s`) — the
patch-numbered sections they cite are the parts of this plan that did land.

---

# iSH-AOK riscv64 Guest Port Plan

Date: 2026-07-10
Branch: `riscv`

## Motivation

Add RISC-V (riscv64, RV64GC) as the fourth guest architecture alongside
i386, amd64, and arm64. RISC-V userland is now a first-class citizen in the
distro world (Alpine ships official riscv64 minirootfs tarballs since 3.20;
Debian trixie ships riscv64 as a release architecture), and the ISA is clean
enough that a riscv64-guest-on-arm64-host JIT should land between the arm64
guest (near 1:1) and the x86 guests (heavy semantic translation) in both
effort and speed.

Unlike the aarch64 port, there is no known iSH fork with riscv guest support
to adapt from (OpenMinis has none). This port is greenfield, but it is the
*third* time we add a 64-bit asm-generic-ABI guest, and the second time we
add a JIT gadget engine, so nearly every kernel-layer decision is already
made and templated. The porting rule throughout: **grep `GUEST_ABI_ARM64`
and mirror every site** (there are ~30+ decision points in `kernel/calls.c`
alone), because riscv64 and arm64 share the Linux asm-generic syscall ABI.

## Scope: What the Target Actually Is

- **Guest ISA: RV64GC** = RV64I base + M (mul/div) + A (atomics) + F/D
  (float/double) + C (compressed) + Zicsr/Zifencei. This is what Alpine and
  Debian riscv64 binaries are compiled for. **C (compressed, 16-bit
  instructions) is mandatory, not optional** - real distro binaries are
  full of them; a 4-byte-only decoder will not execute the first function
  of musl's ld.so.
- **Host: aarch64 only** (Apple Silicon devices and arm64 CLI builds), the
  same restriction as the arm64 guest engine. x86_64 hosts get a stub that
  `die()`s, exactly like `gen_step_arm64`'s `#else` branch.
- **Engine: JIT gadgets only, no interpreter.** Same standing directive as
  the aarch64 port ("we do not need or want an interpreter"). The bring-up
  tool is the same one that worked for arm64: the "no gadget for insn"
  printk fallback in gen.c - run a real binary, read the encoding, port
  that instruction family next. Priority order comes from what ld.so and
  busybox actually execute, NOT from the ISA manual (the aarch64 plan's
  "logical-immediate was deferred and turned out to be ld.so's first
  instruction" lesson).

## Why This Is Easier Than the arm64 Port (and where it is not)

Easier:
- **No flags register.** RISC-V has no NZCV/EFLAGS; comparisons are
  explicit (`slt`) and branches compare two registers directly
  (`beq/bne/blt/bge[u]`). The entire `arm64_flags_live` host-flags-liveness
  machinery, lazy-flags tracking, and cmp+branch fusion tables have no
  riscv analog. A conditional branch gadget is just `ldr, ldr, cmp, b.cond`.
- **Tiny base ISA.** RV64IMAFDC is on the order of ~90 distinct
  instruction semantics before SIMD; there is no vector extension in scope
  (distros do not assume V). Compare arm64's simd/dpextra/crypto surface.
- **All kernel-layer infrastructure exists.** guest_abi plumbing, 64-bit
  MM, ELF64 loading, the asm-generic syscall table content, the arity
  classifier, the native 64-bit dispatch pattern, sigframe patterns, stat
  marshalling: all of it is the arm64 code with registers renamed.

Harder / new:
- **Cross-ISA translation again.** arm64 guest gadgets were near 1:1 host
  instructions. riscv64 gadgets are aarch64 host assembly *implementing*
  riscv semantics. Fortunately the mapping is benign: arithmetic/logic/
  shifts are 1:1, W-suffix (32-bit) ops map to arm64 W-register forms,
  branches map to cmp+b.cond, LR/SC maps to ldxr/stxr, AMOs map to LSE
  atomics, F/D float maps 1:1 onto host scalar FP.
- **RVC decode.** Instruction stream is a mix of 2- and 4-byte encodings
  (alignment 2). Standard technique, used by qemu and everyone else:
  expand each 16-bit C.x encoding to its defined 32-bit equivalent at
  decode time, then feed the ordinary decoder. One table, ~40 entries.
  Consequence: guest PC advances by 2 or 4, and block-start/branch-target
  addresses can be 2-mod-4. The JIT block key already uses byte addresses
  so this is a decoder concern, not a cache concern.
- **x0 is hardwired zero.** Decode-time policy: reads of x0 load a
  guaranteed-zero cpu_state slot; writes with rd=x0 either skip the
  writeback or are redirected to a scratch sink field (`riscv64_zero_sink`)
  so gadget bodies stay uniform. The x0 slot itself is never written.

## Non-Goals for Initial Bring-Up

- RV32 guests. RVV (vector), bitmanip (Zb*), crypto (Zk*) extensions:
  trap as undefined; add blocker-driven only if a real workload hits them.
  HWCAP/cpuinfo must advertise exactly what we implement (rv64imafdc).
- riscv vDSO: 64-bit ABIs already run vDSO-less here (amd64/arm64 do);
  inherit that path.
- guest ptrace regsets (strace-on-guest): defer, mirror the late arm64
  ptrace-regset patch when needed.
- Mixed-arch exec trees and multiarch rootfs: same deferral as aarch64 plan.
- Xcode/app shipping before the CLI engine is solid (see patch 11). Note:
  the app builds the emulator exclusively through meson (`xcode-meson.sh` /
  `xcode-ninja.sh` legacy targets, cpu_family aarch64), so meson source
  additions ARE the app-side engine wiring; the pbxproj emulator-source
  references belong only to the MakeXcodeAutoCompleteWork dummy target
  (IDE indexing). An earlier draft of this plan got that wrong.
- An interpreter, in any form. Reconfirmed 2026-07-10: straight to JIT;
  the interpreter step does not add much and we have done this port shape
  multiple times now.

## Concrete Patch Series

Each patch keeps i386/amd64/arm64 green (`ninja -C build test` plus an
arm64 busybox smoke) and ends with its exit criterion demonstrated.

### 1. ABI scaffolding for a fourth architecture

Files: `kernel/abi.h`, new `kernel/abi/riscv64.h`, `kernel/elf.h`,
`kernel/exec.c`, `kernel/task.h` (comment only)

- `GUEST_ABI_RISCV64 = 3` in `enum guest_abi`.
- `kernel/abi/riscv64.h`: copy `kernel/abi/arm64.h` (LP64 typedefs,
  16-byte iovec static_assert).
- `guest_abi_desc`: `{.name="riscv64", .uname_machine="riscv64",
  .elf_platform="riscv64", 8/8/8}`.
- `guest_abi_user_addr_max`: `1ULL << 38` (Sv39, the Linux riscv64
  default; Alpine/Debian userland is built for Sv39-sized VA). Matching
  `guest_abi_vm_layout` case copied from arm64's, with mmap ceiling under
  the Sv39 limit and the same deliberately-low 32-bit stack placement
  (the "most syscall marshalling is still 32-bit" constraint documented
  at abi.h:111-138 applies identically).
- `kernel/elf.h`: `ELF_RISCV 243`. `elf_abi_detect` (`kernel/exec.c:76`):
  add the `ELF_64BIT && ELF_RISCV` case. ELF64 structs are reused as-is.
- PIE placement: riscv64 joins arm64's dynamic `find_hole_for_elf()`
  branch (exec.c:580-598), not amd64's pinned bias.

Exit criteria: tree builds on both hosts; `elf_abi_detect` classifies a
real riscv64 ELF (readelf cross-check); existing guests unaffected.

### 2. CPU state, offsets, TLS, exec register init

Files: `emu/cpu.h`, `jit/offsets.c`, `kernel/exec.c`, `kernel/fork.c`,
`emu/interrupt.h`

- `emu/cpu.h` sibling block (NOT a union), following the arm64 block at
  cpu.h:226-269: `riscv64_regs[32]` (x0..x31; x0 slot always 0), a
  `riscv64_zero_sink` scratch, `riscv64_pc`, `riscv64_f[32]` (64-bit
  each; F regs are NaN-boxed 32-in-64 for single precision), `riscv64_fcsr`,
  LR/SC reservation fields `riscv64_res_addr`/`riscv64_res_val`. Keep the
  `sizeof(struct cpu_state) < 0xffff` static_assert honest.
- **No separate TLS field**: riscv TLS is the `tp` register = x4, already
  in `riscv64_regs`. `CLONE_SETTLS` in `kernel/fork.c:160-175` gets a case
  writing `riscv64_regs[4]`. (Contrast arm64's out-of-band `arm64_tpidr`.)
- `enum riscv64_reg` with the ABI names (ra=x1, sp=x2, gp=x3, tp=x4,
  a0=x10 ... a7=x17) so kernel code reads `a0`/`a7` not magic indices.
- `jit/offsets.c`: add `riscv64()` emitting `CPU_riscv64_regs/_pc/_f/
  _fcsr/_res_addr/...`.
- `emu/interrupt.h`: `INT_RISCV64_ECALL 0x104` (0x103 is INT_BUS).
- `kernel/exec.c:865-899`: riscv64 register-init block (zero file, set
  `riscv64_pc = entry`, `riscv64_regs[2] = sp`).

Exit criteria: builds; fork/exec/ptrace-snapshot/crash-dump paths audited
for the new fields (the amd64-port step-4 audit list); state initializes,
copies on fork, and dumps.

### 3. Decoder header + RVC expander

Files: new `emu/arch/riscv64/decode.h`

Model on `emu/arch/arm64/decode.h` (213 lines): pure field-extraction
helpers, no state. Contents:
- 32-bit field extractors: opcode[6:0], rd, rs1, rs2, funct3, funct7, and
  the immediate reassemblers for I/S/B/U/J formats (the scrambled B/J
  immediate bit orders are THE classic riscv decode bug; each reassembler
  gets a hand-computed-vector check like the arm64 sign-extension helpers
  got, verified in a standalone scratch check before any gadget work).
- `riscv64_expand_rvc(uint16_t) -> uint32_t`: the C-extension expansion
  table (quadrants 0/1/2; c.addi4spn, c.lw/ld/sw/sd, c.addi/li/lui/
  addi16sp, c.slli/srli/srai/andi, c.mv/add/and/or/xor/sub[w], c.j/jal/
  jr/jalr, c.beqz/bnez, c.lwsp/ldsp/swsp/sdsp, c.ebreak). Returns the
  canonical 32-bit encoding; illegal/reserved encodings return 0 (which
  is conveniently a defined-illegal 32-bit instruction).
- `riscv64_insn_length(low16)`: 2 if `(low16 & 3) != 3` else 4.

Exit criteria: build passes; immediate reassemblers and the RVC table
verified against an independent oracle (`llvm-mc -triple riscv64
-mattr=+c` round-trips, or spike/qemu disasm) for a corpus extracted from
a real Alpine riscv64 busybox binary.

### 4. Syscall table, dispatch, ecall plumbing

Files: `kernel/calls.c`, `kernel/calls.h`

Mirror the arm64 additions one-for-one (riscv64 uses the SAME asm-generic
numbering as arm64, which makes this the most copy-shaped patch):
- `riscv64_syscall_table[463]`: start as a copy of `arm64_syscall_table`,
  then per-entry review. Drop arm64-only entries; riscv64 quirk list is
  short (riscv_flush_icache 259 as a success no-op; riscv_hwprobe 258 as
  ENOSYS initially - glibc probes and falls back).
- Accessors: `riscv64_syscall_number` (a7 = regs[17]),
  `riscv64_syscall_args` (a0-a5 = regs[10..15]), `riscv64_syscall_result`
  (a0 = regs[10]). Bind into `riscv64_syscall_dispatch` and add the case
  in `syscall_dispatch_for_abi`.
- `riscv64_syscall_legacy_arg_count()`: copy of the arm64 arity table
  (same numbering), re-verified. This is what keeps raw-syscall garbage
  in unused upper arg registers from SIGSYS'ing (the amd64/arm64 ports'
  hardest-won lesson class; see comments at calls.c:2229-2261, 3578-3645).
- `handle_riscv64_native_syscall()`: clone of `handle_arm64_native_syscall`
  (calls.c:2174) for the full-64-bit-pointer syscalls (mmap 222, brk 214,
  etc.). riscv64's O_* constants are asm-generic like arm64's, but verify
  the open-flag mapping table entry-by-entry rather than assuming.
- `prepare_syscall_restart`: riscv64 case = `pc -= 4`, restore a7 and
  orig a0 (a0 has the same dual first-arg/return role as arm64's x0;
  there is NO compressed form of ecall, so -4 is always correct).
- `handle_interrupt`/`current_fault_ip`/`record_guest_fault_event`: add
  `INT_RISCV64_ECALL` routing and the riscv64 pc case.

Exit criteria: builds; table resolves every reused `sys_*` name; not yet
reachable (no engine) - same "scaffolding, unreachable" bar as aarch64
plan patch 4.

### 5. JIT gadget engine bring-up (the big one)

Files: new `jit/guest-riscv64/` (`gadgets.h`, `math.S`, `logical.S`,
`control.S`, `memory.S`, `atomics.S`, `fp.S`, `muldiv.S`), `jit/gen.c`,
`jit/gen.h`, `jit/jit.c`, `emu/tlb.c`, `emu/tlb.h`, `meson.build`

Structure copied from `jit/guest-arm64/` (that layout IS our template now):
- **Reuse `jit/gadgets-aarch64/entry.S` unmodified** (`jit_enter`/`gret`/
  `jit_exit`); guest-riscv64/gadgets.h aliases `CPU_x10` style names onto
  the generated `CPU_riscv64_regs` offsets, exactly like guest-arm64's
  header does (including the `#undef` dance around i386 field-name
  collisions).
- Memory-based GPR access (ldr/str per touch), same choice as guest-arm64
  and for the same reason (32 guest GPRs cannot live in host registers).
- `gen.c`: `gen_start_riscv64` + `gen_step_riscv64` under a new
  `ISH_JIT_RISCV64_GUEST` define (set alongside `ISH_JIT_ARM64_GUEST` when
  `jit_cpu_family == 'aarch64'` in meson.build), `bool riscv64` in
  `struct gen_state`, dispatch added to `gen_step()`. **`gen_start` must
  zero the new flag** - the exact uninitialized-`state->arm64` regression
  (c4310ab7) that SIGILL'd i386/amd64 branch-wide is the first thing to
  not repeat.
- `jit.c`: `jit_block_compile_riscv64` + `cpu_step_to_interrupt_riscv64`
  frontend, `current->abi == GUEST_ABI_RISCV64` branch in
  `cpu_run_to_interrupt`. Modeled on the arm64 frontend (jit.c:1329-1568).
- **Mid-block fault restart from day one, not retrofitted**: every
  memory-touching gadget's last code-stream word is its guest pc, and
  segfault paths rewind `CPU_pc` from `[_ip,#-8]` so a kernel-resolved
  INT_PF (CoW after fork) resumes at the faulting instruction. This was
  the deepest bug of the arm64 port; we know the design, install it
  before the first ldr gadget lands.
- Atomics: `emu/tlb.c` helpers `riscv64_lr`/`riscv64_sc` (reservation =
  the `res_addr/res_val` fields; SC = check + CAS in one atomic op, the
  `arm64_stxp` design, reservation PRESERVED across INT_PF so COW-restart
  succeeds) and `riscv64_amo` (delegate to the existing `arm64_lse_rmw`
  machinery; AMOs are add/and/or/xor/swap/min/max on 32/64 bits, all
  expressible there). Fences map to host `dmb ish`.
- FP: scalar F/D gadgets in fp.S mapping 1:1 to host FP; fcsr rounding
  mode switched via FPCR on entry to FP gadgets that use dynamic
  rounding (`rm=111`). NaN-boxing enforced on single-precision writes.

Hard-won host-register rules carried over verbatim from the arm64 port
(each was a real bug there): never x18 (Apple-reserved); x19-x26 for
values live across TLB-prep calls; helpers' scratch is x27; AAPCS64
x0/x1/x2 ARE `_tmp`/`_cpu`/`_tlb` (consume x2 before writing it, write x1
last); crosspage writes go through the fill+redirect+write_done design;
never parse params into x10 in gadgets consuming a w10 handoff.

Bring-up milestones (each is a commit):
  a. Static hand-assembled hello (ecall write/exit) runs: proves entry,
     memory gadgets, ecall trap, syscall dispatch end-to-end.
  b. Alpine riscv64 musl ld.so loads a dynamic binary: proves the RVC
     expander, auipc/jalr, and the load/store family against real code.
  c. Real busybox sh runs pipelines/fork/exec: proves branches, atomics
     (musl uses LR/SC), signal-adjacent paths.
Assembly toolchain for (a): Homebrew LLVM has the riscv64 backend
(`/opt/homebrew/opt/llvm/bin/clang -target riscv64-linux-gnu -nostdlib
-static -fuse-ld=lld`); Apple clang does NOT - same pattern as
tests/arm64.

Exit criteria: busybox sh interactive under `./build/ish -f <riscv-fakefs>`
on an arm64 host; i386/amd64/arm64 benchmarks unchanged.

### 5b. Vendor/user extension hook (generic, all arches)

Files: new `jit/ext.c` + `jit/ext.h`, `jit/gen.c` (miss paths), a callback
gadget per host gadget set, CLI flag in `main.c`

RISC-V's defining quirk is that vendors extend the ISA: the custom-0..3
major opcodes (0x0B/0x2B/0x5B/0x7B) are permanently reserved for vendor
extensions and can never be claimed by ratified standard extensions. To
support those (and to let people test new instructions on ANY guest arch)
without an interpreter layer, add a decode-miss hook:

- Runtime registry of `{abi, uint32 mask, uint32 match, handler}` entries.
  Handler signature `int (*)(struct cpu_state *, struct tlb *, uint64_t
  insn, guest_addr_t pc)` returning an interrupt code or "continue".
- Hook point: each `gen_step_*` frontend, on failing to decode an
  instruction, consults the registry BEFORE raising INT_UNDEFINED. On a
  match it emits one generic callback gadget (the amd64_jit_* helper-call
  pattern that already exists) invoking the handler. Standard
  instructions never touch this path, and the check runs at block-compile
  time only, so the hot path is unaffected.
- **Registration policy is the whole safety story and is per-arch**:
  riscv64 accepts ONLY the custom-0..3 major opcodes; arm64 only
  architecturally unallocated encodings; x86 only encodings that
  currently #UD. The validator rejects anything else, so this can never
  become an interpreter crutch for the ratified ISA (the no-interpreter
  directive stays intact: the JIT remains the only engine for standard
  instructions).
- Registering/unregistering MUST flush the JIT block cache (compiled
  blocks have the old miss behavior baked in); reuse the existing
  invalidation machinery.
- Handler sources, in tiers: (1) compiled-in vendor packs (e.g. T-Head)
  toggled at runtime and reflected in cpuinfo/HWCAP/hwprobe only when
  enabled - works in the App Store build; (2) CLI-only dlopen plugin
  (`ish -X ext.dylib`) for development - iOS cannot load code at
  runtime; (3) stretch: a small validated declarative micro-op format
  (`rd = f(rs1, rs2)` over a fixed op set) for simple on-device user
  additions without arbitrary code.
- Free tier 0, document it: guest userspace can already trap-and-emulate
  via a SIGILL handler, exactly as on real hardware.

Exit criteria: a demo custom-0 instruction (e.g. a register-swap or
popcount toy) registered via a CLI plugin executes inside a riscv64
guest binary; the same mechanism demonstrated for one unallocated arm64
encoding; registry register/unregister under load doesn't leave stale
compiled blocks (invalidate verified); zero measurable slowdown on the
standard-ISA benchmark set.

### 6. Signal delivery

Files: `kernel/signal.c`

- `struct rt_sigframe_riscv64`: riscv ucontext = uc_flags/uc_link/uc_stack/
  uc_sigmask + `struct sigcontext` = `__riscv_mc_gp_state` (pc, then
  x1..x31 in order) + `__riscv_fp_state.__d` (f[32] + fcsr). static_assert
  every struct size against the kernel ABI values; the arm64 experience
  says a wrong mcontext layout means every handler reads garbage.
- `setup_rt_sigframe_riscv64` / `restore_riscv64_mcontext` /
  `sys_rt_sigreturn_riscv64` (nr 139), branches added to the
  `guest_abi_is_64bit` deliver path and the sigreturn dispatcher.
- On-stack trampoline (no vDSO): `li a7, 139` (`0x08b00893`) + `ecall`
  (`0x00000073`), with `ra` (x1) pointed at it unless SA_RESTORER.
- siginfo_/stack_t_/sigaction 64-bit marshalled forms: reuse the
  amd64/arm64-shared asm-generic ones, as arm64 did.

Exit criteria: the guest-side signal suite subset (signal_core,
signal_restart, signal_altstack from tests/manual) passes on a riscv64
fakefs; fork+SIGCHLD busybox flows keep working.

### 7. Struct marshalling and /proc

Files: `fs/stat.h`, `fs/stat.c`, `fs/proc/root.c`, `kernel/uname.c` (free)

- stat: riscv64 uses the asm-generic layout, byte-identical to
  `struct arm64_stat_`. Reuse it (typedef or shared name) plus a
  dispatch case; do NOT hand-copy a third 128-byte struct.
- flock/iovec/sigaction/timespec: all asm-generic LP64, reuse the
  amd64/arm64 forms already in place.
- uname comes free via `guest_abi_desc` ("riscv64").
- `/proc/cpuinfo` (`proc_show_cpuinfo`): new riscv format block -
  `processor`, `hart`, `isa : rv64imafdc`, `mmu : sv39`, `uarch` -
  kept in lockstep with `AT_HWCAP` (exec.c auxv: COMPAT_HWCAP_ISA_
  I|M|A|F|D|C bits) and with what the JIT actually implements. Parsers
  key on the `isa` line.

Exit criteria: fs_conformance.c passes on riscv64; `uname -m` =
riscv64; `cat /proc/cpuinfo` parses in the guest's own tooling.

### 8. Rootfs and provisioning

Files: `tools/build-devuan-minirootfs.sh`, docs

- Test rootfs: Alpine riscv64 minirootfs (3.23.x tarballs confirmed
  available on dl-cdn) through the existing `tools/fakefsify` (arch-blind,
  no changes). Same "mkdir tmp+dev" caveat as the arm64 minirootfs.
- Devuan/Debian: `build-devuan-minirootfs.sh` gains `riscv64` in ARCHES,
  `arch_suffix riscv64 -> riscv64`, docker platform `linux/riscv64`.
  Verify Devuan actually publishes riscv64 (Debian does; if Devuan
  lags, park the Devuan image and ship Alpine first).

### 9. Guest regression suite

Files: `tests/riscv64/` (new), `tests/manual/riscv64/` (new), `fs/aok-tests.manifest`

- `tests/riscv64/`: hand-assembled smoke tests mirroring `tests/arm64/`
  (hello/branches/atomics/fp/signal), built with Homebrew clang+lld.
- `tests/manual/`: the cross-arch C suite already runs anywhere; add a
  `riscv64/` subdir only for arch-specific probes (LR/SC edge cases,
  RVC-heavy code paths, fcsr rounding).
- Oracle strategy: no riscv hardware in the rig, so the differential
  oracle is `qemu-riscv64` (user-mode) on the mint Lima VM or via
  Homebrew qemu locally; same role mint's real-Intel plays for x86.

### 10. Performance pass (after correctness)

Only after busybox/gcc-level workloads run: block chaining with the
arm64-style bit63-tagged targets and chain budget (backward edges gated
by `jit_frame.chain_budget`, the jetsam-starvation rule), fused
compare-and-branch gadgets (riscv branches are ALREADY fused
compare+branch, so the per-cc gadget explosion arm64 needed does not
apply - one gadget per branch op suffices), and load/store fast paths.
Benchmark against the same harness (/tmp/bench.py pattern) with the
arm64 guest numbers as the bar to beat for a RISC ISA.

### 11. App integration (last)

Files: `app/Roots.m`, `app/AppDelegate.m`, `iSH-AOK.xcodeproj/project.pbxproj`

- Roots.m `BundledRootChoices()`: riscv64 entry, download-on-demand via
  `kBundledRootDownloadURLKey` (rootfs-assets release), guestABI
  metadata string "riscv64"; AppDelegate recovery string.
- Engine wiring in the app comes free: the app builds the emulator via
  meson (xcode-meson.sh), so patch 5's meson additions cover it. Add
  "riscv64" to the `guest_archs` choices/default (see below) and to the
  `ISH_GUEST_ARCHS` line in app/iSH.xcconfig. Optionally add
  `jit/guest-riscv64/` file references to the MakeXcodeAutoCompleteWork
  target for IDE indexing only.
- Validated as its own change: Xcode build + on-device smoke.

### Per-arch build selection (`guest_archs`, landed 2026-07-10)

The build has a `guest_archs` meson array option (default: all arches;
at least one required, enforced at configure). Each arch gets a
`-DISH_GUEST_<ARCH>=0/1` define; `kernel/exec.c:elf_abi_detect` is the
master gate (disabled arch ELFs get ENOEXEC). For arm64 the option also
drops `emu/arm64_interp.c` + `jit/guest-arm64/*.S` and suppresses
`ISH_JIT_ARM64_GUEST`; i386/amd64 stay compiled regardless (host gadget
sets and jit/gen.c link amd64_jit_* helpers) so for those two it is a
functional gate only. Xcode passes it via `ISH_GUEST_ARCHS` in
app/iSH.xcconfig -> xcode-meson.sh.

riscv64 integration points when patch 1 lands: add 'riscv64' to the
option choices and default in meson_options.txt, `ISH_GUEST_RISCV64`
define + misc.h fallback, gate the new `elf_abi_detect` case with it,
condition `jit/guest-riscv64/*.S` + `ISH_JIT_RISCV64_GUEST` on
`guest_archs.contains('riscv64') and jit_cpu_family == 'aarch64'`, and
extend the xcconfig default list.

## Testing Strategy Summary

1. Per-patch: `ninja -C build test` + arm64/i386/amd64 smoke stays green.
2. Decoder: offline verification of immediate reassembly + RVC expansion
   against llvm-mc/qemu disasm BEFORE any execution path exists.
3. Execution: hand-assembled smoke tests, then Alpine riscv64 busybox,
   then the tests/manual guest suite on a riscv64 fakefs.
4. Differential: qemu-riscv64 user-mode as the instruction-semantics
   oracle (register-state diffing for suspect instruction families, the
   tests/remote conductor pattern if it earns its keep).
5. Device: iPad smoke only after the CLI engine passes the manual suite.

## Risks / Open Questions

- **RVC immediate scrambling** is the likeliest source of silent
  miscomputation; mitigated by the offline decoder oracle in patch 3.
- **Sv39 (1<<38) vs Sv48**: plan says Sv39 to match Linux defaults and
  keep the address space small; revisit only if a real workload wants
  more VA (the low-stack/32-bit-marshalling constraint is unaffected).
- **riscv_hwprobe**: newer glibc uses it eagerly; ENOSYS fallback is
  believed sufficient, verify against the actual Alpine/Debian libc
  versions early in bring-up.
- **Devuan riscv64 availability** (patch 8) may force Alpine-first or
  Debian-based images for this arch.
- Gadget engine effort estimate: guest-arm64 is ~7.5K lines of .S +
  ~3.5K lines of gen.c; riscv64 should come in well under (no
  flags/simd/crypto surface, ~90 semantics), call it ~3-4K .S +
  ~1.5-2K gen.c.
