# build 558 musts

Work that must be done, or explicitly decided, before 558 is tagged. Each
entry says what is **established**, what the **next step** is, and how to
**prove** it afterwards.

Started 2026-10-05, after `builds/iSH-AOK_557`. Supersedes
[docs/build_556_musts.md](build_556_musts.md) (557 had none).

**Status, 2026-10-05:**

| § | item | state |
|---|---|---|
| 1 | RVA23 on the riscv64 guest | **DONE for 558** (2026-10-05): the scalar extensions and V are in, and Ubuntu 25.10 (RVA23) runs apt/python3/gcc on the Mac and the M4; V is all gadgets and advertised (2x on OpenSSL ChaCha20); Supm deliberately not |

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

**V, 2026-10-05:** RVV 1.0 with Zvbb, Zvfhmin and Zvkt at VLEN 128, all
of it gadgets (jit/guest-riscv64/vector.S, chosen in gen.c
gen_riscv64_vector; the maintainer's rule: instructions are gadgets, never
C -- the first cut's C core, jit/riscv64_vector.c, is deleted). The state
is in cpu_state (so fork, exec -- vtype starts vill -- and checkpoints
carry it) and in signal frames as Linux lays it out (the RISCV_V_MAGIC
record after the ucontext, restored through its datap).
- **Typed by SEW.** gen.c knows the vtype a vsetvli earlier in the block
  set, or guesses the one in force at compile time, and emits the gadget
  for its SEW. Each checks the vtype at run time and, if another is in
  force, re-dispatches to the body for that SEW after checking the
  register groups against its LMUL. vill, a misaligned group, a widening
  op at LMUL 8 or SEW 64, an FP op below SEW 32 and a nonzero vstart on an
  arithmetic instruction (which the spec allows to trap) are illegal
  instructions from the gadget; vop, what decodes to nothing, is one too.
- **Integer:** every single-width op (vv/vx/vi, masked and not, vmerge,
  Zvbb vandn/vror/vrol), multiply, multiply-add, mulh*, div/rem with
  RISC-V's x/0 and overflow results, compares, reductions (and widening),
  widening and narrowing (vwadd* ... vnsra, vwsll), slides, vrgather(ei16),
  vcompress, viota, vadc/vmadc/vsbc/vmsbc, the Zvbb unary ops, the mask
  ops, vmsbf/vmsif/vmsof, vcpop, vfirst, vid, vzext/vsext, vmv.x.s/s.x,
  vmv<nr>r; fixed point (vsadd*, vssub*, vaadd*, vasub*, vsmul, vssrl,
  vssra, vnclip*) rounding by vxrm, vxsat sticky. NEON lanes where an op
  maps onto them, x registers element by element where not.
- **FP** at SEW 32/64: NEON lanes under the host FPCR that carries frm
  (emu/fpenv.c), so they round and raise as RISC-V does; masked-off and
  tail lanes get inputs that raise nothing; vfmin/vfmax return the number
  against a signalling NaN (minimumNumber), the FMAs are single fused
  fmla/fmls. Conversions (all, with RISC-V's NaN-to-max and NV-alone
  saturation, rod by fcvtxn, Zvfhmin's f16), vfclass, vfrsqrt7/vfrec7 (the
  spec's tables), the reductions and the widening ops go element by
  element in scalar FP.
- **Memory:** unit-stride loads/stores have a one-copy fast path;
  everything else -- strided, indexed, segments, whole-register, mask,
  fault-only-first, masked, elements across a page -- is vmem_any, which
  decodes the instruction at run time and takes each element through the
  TLB as the scalar gadgets do (tlb_handle_miss, tlb_write_ptr_slow). A
  fault stops at its element with vstart there and the instruction
  resumes from it; a fault-only-first load past element 0 ends with vl
  there.
- **vset*, CSRs:** vsetvli/vsetivli with a legal immediate vtype are
  worked out at translation; vsetvl, an illegal vtype and vsetvli x0,x0
  are decided in the gadget (vill when they must be). vstart, vxrm, vxsat
  and vcsr read and write in a gadget; vl, vtype and vlenb read.
- **Checked by** tests/manual/riscv64/riscv64_rvv_gadgets.c, generated by
  tools/gen-rvv-gadget-test.py: every instruction under every SEW/LMUL
  its encoding allows, typed and re-dispatched, masked and not, at random
  and special-value data, all four rounding modes and vxrm modes, against
  a model written from the spec (fflags, vxsat and memory compared too),
  plus SIGILL for the illegal cases, fault-only-first and precise faults
  at a PROT_NONE page, and the CSRs and vsetvl; a positive control fired
  for every gadget family. And riscv64_rvv.c (gcc -O3 auto-vectorized
  kernels against the same kernels built for rv64gc, 1.7M checks),
  riscv64_rvv_signal.c and riscv64_rvv_ptrace.c. On the Mac and the M4.
- **Speed** (M4, vbench kernels against the same loops built scalar): add
  3.0x, sum 2.2x, max 3.3x, count 3.2x, select (vmul) 4.4x, shift 2.3x;
  gather 0.8x. OpenSSL ChaCha20 on its vector path (forced): 108 MB/s
  against 52 scalar; it was 3.5x slower than scalar in the C core.
- **Advertised** (2026-10-05): riscv_hwprobe IMA_V with Zvbb, Zvkb,
  Zvkt and Zvfhmin (and MISALIGNED_VECTOR_PERF, unknown), AT_HWCAP 'v',
  and the isa line (rv64imafdcv ... _zve32f ... _zve64x_zvbb_zvfhmin_zvkb
  _zvkt). Neither Ubuntu 25.10's glibc (2.42) nor Devuan's (2.41) has
  vector ifuncs; their OpenSSL 3.5 now picks ChaCha20 with Zvkb by itself
  (OPENSSL_riscvcap reads ZBA_ZBB_ZBS_V_ZVBB_ZVKB): 121 and 126 MB/s on
  the Mac CLI.
- PR_RISCV_V_SET/GET_CONTROL as Linux has them (kernel/misc.c): V reads as
  on; a running program cannot turn it off (EPERM) but sets what the next
  exec gets and whether that is inherited; an exec with V off has no
  AT_HWCAP 'v' and every vector instruction is an illegal one (the JIT
  translates them so; turning V back on drops the translations). Checked
  by tests/manual/riscv64/riscv64_rvv_ctrl.c.
- The NT_RISCV_VECTOR ptrace regset reads and writes it
  (tests/manual/riscv64/riscv64_rvv_ptrace.c).

**Supm, decided against for now:** user pointer masking exists only for a
program that asks for it, prctl(PR_SET_TAGGED_ADDR_CTRL) with a PMLEN
(HWASan does), and Linux refuses that call on hardware without it, which
callers handle. AOK refuses it too (EINVAL, measured), so nothing breaks;
honouring it would mean masking every guest address in every memory
gadget for one sanitizer.

**Proved 2026-10-05 (Mac CLI): Ubuntu 25.10 riscv64 runs.** ubuntu-base-
25.10-base-riscv64.tar.gz (cdimage.ubuntu.com, SHA256 e6dcaa68...) as a
fakefs root (build/ubuntu-riscv64-rva23): apt-get update and install of
python3 3.13 and gcc 15 (90.9 MB, dpkg and every maintainer script), then
python3 (hashlib/json/sqlite3/ssl/zlib; the same hash as on Alpine and
Devuan) and gcc compiling and running a program. Its gcc's default target
is the RVA23 set, V included. One AOK bug found on the way: a hard-linked
file named by the wrong link in /proc/self/exe, which stopped every one of
Ubuntu's (uutils) coreutils -- fixed, 59575749a. Its Python spends ~3% of
its time in vector instructions (libc's string functions), so V's speed is
not what limits it; the rest is the scalar riscv64 JIT, as on Alpine.
apt printed ~2000 "Tried to start delayed item" warnings for the one
package whose first fetch failed (Ign) and was retried successfully: apt
repeating itself once per pass of its fetch loop over a slow (118 kB/s)
12-minute download, not seen to be an AOK fault.

**And on the M4** (installed with manage-roots.sh from the same URL, run
by chroot with /proc mounted): apt-get update/install of python3 and gcc
(no warnings this time -- the Mac's came with its flaky fetch), the same
python hash, gcc building and running, and the Python microbench at
99/535/448/387 ms (fib/method/dict/str) -- level with the Alpine 3.24.2
riscv64 root on the same device (95/562/428/413). dmesg clean.

**Left:** the V items above, and vector gadgets for the string-function instructions
(vsetvli, unit-stride vle/vse/vle-ff, vmseq/vmsne.vi, vfirst.m and csrr vl
have them now), before V is advertised.

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
