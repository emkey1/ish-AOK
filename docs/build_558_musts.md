# build 558 musts

Work that must be done, or explicitly decided, before 558 is tagged. Each
entry says what is **established**, what the **next step** is, and how to
**prove** it afterwards.

Started 2026-10-05, after `builds/iSH-AOK_557`. Supersedes
[docs/build_556_musts.md](build_556_musts.md) (557 had none).

**Status, 2026-10-05:**

| § | item | state |
|---|---|---|
| 1 | RVA23 on the riscv64 guest | **PARTIAL**: the scalar extensions are in (2026-10-05); V (with Zvfhmin/Zvbb/Zvkt) and Supm are open |

---

## 1. RVA23 on the riscv64 guest

**Why.** The riscv64 guest implements rv64imafdc (with Zicsr/Zifencei) and
advertises exactly that (AT_HWCAP in kernel/exec.c, the `isa` line in
fs/proc/root.c); vector (V), bit-manipulation (Zb*) and crypto (Zk*) trap as
undefined, by the bring-up plan's rule "add blocker-driven only if a real
workload hits them" (docs/historical/riscv64_guest_plan.md). That held while
every distro built for rv64gc. It stops holding when a distro raises its
baseline: Ubuntu moved its riscv64 builds to the RVA23 profile from 25.10, so
such a root does not run here at all. RVA23 code is also shorter -- shNadd,
zext/sext, min/max, clz/ctz/cpop, czero, rev8 -- and instruction count is the
riscv64 engine's whole gap to arm64 (docs/TODO.md: "the RISC-V gap is
instruction count"), so this is a speed item as well as a reach one.

**Established:**
- RVA23U64 makes mandatory, beyond rv64gc (check each against the ratified
  RVA23 profile spec before relying on this list -- it is from memory):
  Zba, Zbb, Zbs, Zicond, Zcb, Zfa, Zfhmin, Zimop, Zcmop, Zawrs, Zihintpause,
  Zihintntl, Zicbom/Zicbop/Zicboz, Zicntr/Zihpm, Zkt, Za64rs, Zic64b, the
  Zicc* memory attributes, Supm, and the vector unit: V with Zvfhmin, Zvbb
  and Zvkt.
- Most of that list is small and fits the existing gadget engine the way
  the M/A/F/D work did: Zba/Zbb/Zbs, Zicond, Zcb and Zfa are ALU/FP
  instructions; Zimop/Zcmop are no-ops until something defines them; the
  hint and cache-block extensions are no-ops or zero-fills here; Zawrs is a
  wait hint; Supm (pointer masking) needs a prctl and an address mask.
- V is the large part: 32 vector registers (VLEN 128 is the minimum and the
  natural choice, matching NEON), vtype/vl/vstart state that has to be saved
  in signal frames, ptrace regsets and checkpoints, and a big decode space
  (loads/stores with strides and segments, masked ops, LMUL grouping). The
  arm64 guest's per-arrangement NEON gadgets (vspec) are the model.
- Detection: glibc and Linux userspace ask riscv_hwprobe (syscall 258) for
  extensions, not only AT_HWCAP; the `isa` string in /proc/cpuinfo needs the
  same list.

**Done 2026-10-05 -- the scalar part, advertised as it landed:**
- Zba, Zbb, Zbs, Zicond: gadgets in jit/guest-riscv64/alu.S, decoded by
  gen_riscv64_bitmanip (jit/gen.c) ahead of the base OP/OP-IMM tables (the
  register-cache classifier and the peephole fusions already reject the new
  funct7/imm bits, so they fall through to it).
- Zcb: c.lbu/lhu/lh/sb/sh and c.zext.b/h/w, c.sext.b/h, c.not, c.mul in
  riscv64_expand_rvc (emu/arch/riscv64/decode.h), with llvm-mc vectors in
  tests/riscv64/decode_vectors.h.
- Zimop/Zcmop (rd <- 0 / nop), Zawrs (nop), Zihintpause/Zihintntl (already
  a fence and x0 writes), Zicbop (ori x0), Zicbom (nop over coherent host
  memory), Zicboz (cbo.zero: a gadget zeroing the 64-byte block, Zic64b).
- Zfa (fli, fminm/fmaxm, fround/froundnx, fltq/fleq, fcvtmod.w.d -- a C
  helper, since FJCVTZS is not on the A10X) and Zfhmin (flh/fsh,
  fmv.x.h/fmv.h.x, fcvt between half and single/double).
- riscv_hwprobe (syscall 258) answers for all of it (kernel/calls.c), and
  /proc/cpuinfo's isa line lists it; AT_HWCAP keeps the single letters,
  as Linux does.
- Checked by tests/manual/riscv64/riscv64_rva23_scalar.c and
  riscv64_rva23_fp.c (in the guest suite): every instruction against C
  references built for rv64gc, a positive control per family.
- Zkt, Za64rs, Zic64b and the Zicc* attributes need nothing here; Zihpm's
  hpmcounters trap as on a default Linux (scounteren clear).

**Left:** V (VLEN 128; with Zvfhmin, Zvbb, Zvkt), and Supm (pointer
masking: PR_SET_TAGGED_ADDR_CTRL with a PMLEN, and the JIT masking
addresses). An RVA23 Ubuntu is built with V as its baseline, so its
compiled code vectorizes freely: V is what stands between AOK and booting
one. Plan: a correct V first -- a vector state in cpu_state (32 x 128-bit,
vtype/vl/vstart, saved in signal frames, the NT_RISCV_VECTOR regset and
checkpoints), each V instruction a C-helper call driven by vtype -- then
gadgets for what profiles show is hot. hwprobe/cpuinfo get V only when it is
complete enough for glibc's ifuncs.

**Next step (originally):** confirm the Ubuntu 25.10+ baseline and pull an Ubuntu riscv64
rootfs, run it with ISH_TRACE on undefined instructions to get the order
real code hits them in; implement the scalar extensions first (they are
cheap and also speed up rv64gc-plus distros that use them through hwprobe),
then V; add riscv_hwprobe and widen the advertised ISA only as each lands.

**Prove:** an Ubuntu riscv64 (RVA23) root boots and runs apt, python3 and gcc
on the Mac CLI and on the M4; per-extension differential tests against real
hardware (or QEMU with the RVA23 profile as the oracle, if no board is at
hand) in tests/manual/riscv64/; the five-root suite unchanged; and the
existing Devuan riscv64 root's gzip/gcc/python numbers re-measured, since its
own code may start taking the new paths through hwprobe.
