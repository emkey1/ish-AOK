# build 558 musts

Work that must be done, or explicitly decided, before 558 is tagged. Each
entry says what is **established**, what the **next step** is, and how to
**prove** it afterwards.

Started 2026-10-05, after `builds/iSH-AOK_557`. Supersedes
[docs/build_556_musts.md](build_556_musts.md) (557 had none).

**Status, 2026-10-05:**

| § | item | state |
|---|---|---|
| 1 | RVA23 on the riscv64 guest | **OPEN** (the maintainer put it on this list on 2026-10-05) |

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

**Next step:** confirm the Ubuntu 25.10+ baseline and pull an Ubuntu riscv64
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
