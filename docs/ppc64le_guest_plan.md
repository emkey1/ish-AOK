# iSH-AOK ppc64le Guest Port Plan

Date: 2026-10-17. Target: build 562.

## Status (2026-10-18)

Patches 1-7, 9 and 10 are in, and 8 for Alpine. Alpine 3.24.2 ppc64le
runs on the Mac CLI and as a device's booted root, on the M4 iPad (iOS 27)
and the A10X iPad Pro (iOS 17.7), 2026-10-18: apk over HTTPS, gcc, Python,
strace, and the manual suite 366 passed, none failed, on each. It passes on
the Mac too; the differential tests in
`tests/ppc64/` match qemu-ppc64le except where qemu is wrong (their headers
say where). There is no core dump writer for any guest, and a checkpoint
copies `cpu_state` whole, so 7's last two items needed nothing. The
ppc64 vDSO (`vdso/ppc64/`) is the arm64 one's design under powerpc's names.
What is left is the ppc64le entry in [TODO.md](TODO.md).

## What and why

A fifth guest architecture, 64-bit little-endian PowerPC (ppc64le, ELFv2),
the one Alpine (`ppc64le`) and Devuan/Debian (`ppc64el`) ship. AOK VM's QEMU
engine has PowerPC gadgets since 2026-10-16 (emkey1/qemu `aok-vm`,
`target/ppc/tcg/aok/`, merged at 0acbb07580), written so that this port can
take them: operands are byte offsets into a CPU state, the condition
register is a word per field, XER's SO/OV/CA a word each. They are 32-bit
big-endian (Mac OS on mac99), so the port widens them to 64 bits, drops the
byte reversal (a little-endian guest's memory is the host's), and adds what
a Linux userland needs that a G4 does not: the doubleword ops, the 64-bit
rotates, VSX and the ISA 2.07 (POWER8) additions.

The riscv64 port ([historical/riscv64_guest_plan.md](historical/riscv64_guest_plan.md))
is the template: the same patch order, the same host-register rules, the same
"grep the riscv64 site and mirror it" method, and gadgets only (no
interpreter, no C core).

## The target

- **ISA: Power ISA 2.07 (POWER8), 64-bit, little-endian.** That is the
  baseline both distributions compile for: Alpine 3.24's busybox and musl
  already use `lxvd2x`/`stxvd2x`, `mtfprd`/`mffprd`, `xscvdpspn`, `lwa`,
  `ldbrx`. The histogram of musl + busybox is 263 mnemonics, mostly
  integer; the order of work comes from what real binaries execute (the
  "no gadget for insn" printk), not from the manual.
- **Host: aarch64 only**, as for the arm64 and riscv64 guests.
- **Registers in cpu_state:** `ppc64_gpr[32]`, a zero word for `(rA|0)`,
  `ppc64_pc`, `lr`, `ctr`, `cr[8]` (a word per field, LT GT EQ SO = 8 4 2 1,
  QEMU's form), XER as `so`/`ov`/`ca`/`ov32`/`ca32` words plus the rest,
  `vsr[64]` of 16 bytes (FPR n = VSR n's doubleword 0; VR n = VSR 32+n),
  `fpscr`, `vscr`, `vrsave`, and the larx reservation (address, value,
  size). TLS is r13: no separate field.
- **Vector layout:** a VSR is held as the host's 128-bit value with
  big-endian element 0 the most significant, so `lvx`/`stvx` in LE mode is
  a plain 16-byte access, `lxvd2x` a 16-byte load and a doubleword swap,
  and a BE element i is host lane (n-1-i). FPR n is the high doubleword:
  byte offset +8 of its VSR.
- **FP exceptions:** fpenv's lazy model (emu/fpenv.c), as riscv64's fflags:
  the host FPSR gathers flags while guest code runs; `mffs` and fpenv_exit
  fold them into FPSCR; RN is the host FPCR's rounding mode. FPRF is not
  kept per instruction (only `fcmpu`/`fcmpo` write FPCC, as they write the
  CR field); nothing in a Linux userland reads it.

## The ABI (where ppc64 is not asm-generic)

- **ELF:** `EM_PPC64` (21), ELFCLASS64, EI_DATA LSB, e_flags ABI 2.
- **auxv:** AT_HWCAP = 32|64|FPU|MMU|ALTIVEC|VSX|ARCH_2_05|ARCH_2_06|TRUE_LE,
  AT_HWCAP2 = ARCH_2_07 (no HTM, no SCV, no crypto until those gadgets
  exist), AT_PLATFORM "power8", AT_BASE_PLATFORM, AT_DCACHEBSIZE /
  AT_ICACHEBSIZE / AT_UCACHEBSIZE 128 (glibc's memset uses `dcbz` by it),
  AT_IGNOREPPC entries as Linux writes them.
- **Syscalls:** `sc`; r0 = number, r3-r8 = arguments, r3 = result. An
  error sets CR0[SO] and returns the POSITIVE errno (musl:
  `sc; bns+ 1f; neg 3,3`). `scv` is not offered (no HWCAP2_SCV).
  Restart: pc -= 4, r3 = orig_gpr3.
- **Numbers:** powerpc's own table (`asm/unistd_64.h`, 415 names, close to
  i386's numbering). The table is built by name from the amd64 table's
  handlers (64-bit marshalling), with the ppc-only entries by hand: `ipc`
  and `socketcall` multiplexers, `clone` in the BACKWARDS order (arm64's),
  `mmap` with ppc's flags, `select`(82)/`_newselect`(142), `switch_endian`,
  `subpage_prot`, `rtas`, `spu_*`, `swapcontext`, `sys_debug_setcontext`.
- **Constants that differ from the internal (i386) values:**
  - open: O_DIRECTORY 040000, O_NOFOLLOW 0100000, O_LARGEFILE 0200000,
    O_DIRECT 0400000;
  - mmap: MAP_NORESERVE 0x40, MAP_LOCKED 0x80, PROT_SAO 0x10;
    mlockall: MCL_CURRENT 0x2000, MCL_FUTURE 0x4000, MCL_ONFAULT 0x8000;
  - sockets: SO_RCVLOWAT 16, SO_SNDLOWAT 17, SO_RCVTIMEO 18, SO_SNDTIMEO
    19, SO_PASSCRED 20, SO_PEERCRED 21;
  - errno: EDEADLOCK 58;
  - **ioctl encoding:** 13 size bits, 3 direction bits (NONE 1, READ 2,
    WRITE 4) against the generic 14/2; every `_IOC` number is re-encoded;
  - **termios:** powerpc's own `struct termios` (c_cc[19] before c_line,
    then c_ispeed/c_ospeed), its own flag values (IXON 0x200, ICANON 0x100,
    ECHO 0x8, ...), its own c_cc indices (VMIN 5, VTIME 7, ...) and its
    own request numbers (TCGETS = `_IOR('t', 19, struct termios)`,
    TIOCGWINSZ = `_IOR('t', 104, struct winsize)`, FIONREAD = `_IOR('f',
    127, int)`); the 0x54xx requests that powerpc kept (TIOCSCTTY,
    TIOCSTI, ...) map as they are.
  - struct stat is x86-64's layout; epoll_event is unpacked (arm64's);
    k_sigaction is {handler, flags, restorer, mask}; statfs is the 64-bit
    generic one.
- **Signals:** ppc64's rt_sigframe (ucontext first, then the trampoline,
  pinfo, puc, siginfo, the 288-byte red zone gap); sigcontext's gp_regs is
  pt_regs (gpr[32], nip, msr, orig_gpr3, ctr, link, xer, ccr, softe, trap,
  dar, dsisr, result), fp_regs 32 FPRs + FPSCR, v_regs pointing into
  vmx_reserve (32 VRs, VSCR, VRSAVE, then the VSX low doublewords). The
  handler gets r1 = frame - 128 with the back chain, r3 = signo, r4 =
  &info, r5 = &uc, r6 = frame, r12 = the handler (ELFv2 global entry), LR
  = the trampoline (`li r0,172; sc`). MSR in the frame says LE|FP|VEC|VSX.

## Patch series

Each patch keeps the other four guests green.

1. **ABI scaffolding:** `GUEST_ABI_PPC64`, `kernel/abi/ppc64.h`, ELF
   detection, `guest_archs` option, the VM layout (arm64's).
2. **CPU state, offsets, exec/fork/clone register setup, auxv, interrupt
   code for `sc`.**
3. **Decoder header** `emu/arch/ppc64/decode.h`: field extraction and the
   D/DS/DQ/X/XO/M/MD/MDS/XFX/XL/B/I/VA/XX* forms, checked against llvm-mc
   on the musl/busybox corpus.
4. **Syscalls:** the ppc64 table, the CR0.SO return, restart, arity, and
   the constant translations above (open, mmap, mlock, socket options,
   ioctl re-encoding, termios).
5. **Gadget engine** `jit/guest-ppc64/`, translator `jit/gen_ppc64.c`,
   frontend in `jit/jit.c`: (a) a static hello; (b) musl's ld.so and a
   dynamic busybox; (c) busybox sh with fork/exec/pipes; (d) FP and VSX
   until apk works.
6. **Signals.**
7. **/proc/cpuinfo, uname, HWCAP agreement, ptrace regsets, core dumps,
   checkpoint save/restore of the state, seccomp AUDIT_ARCH_PPC64LE.**
8. **Rootfs:** Alpine 3.24.2 ppc64le (download), Devuan ppc64el if
   published.
9. **Tests:** `tests/ppc64/` smoke programs, the manual suite on a ppc64le
   root, qemu-ppc64le on camd as the oracle (installed 2026-10-17 with
   `gcc-powerpc64le-linux-gnu`).
10. **App:** roots list, subtitle, `ISH_GUEST_ARCHS`, the bundled-root
    download; Xcode build and a device run.
