# iSH-AOK amd64 Port Plan (archived, not maintained)

**This file is history.** Pre-implementation bring-up plan dated 2026-04-08.
Its "Hard Blockers" (`arch_prctl` unimplemented, no register room for
`rip`/`r8`-`r15`, "bring up the interpreter first, not the JIT") are all now
false: amd64 is a fully shipped guest architecture with its own JIT
(`sys_arch_prctl` in `kernel/misc.c`, `amd64_regs[]`/`amd64_rip` in
`emu/cpu.h`). The current account is
[`docs/book/ch07-four-guests.md`](../book/ch07-four-guests.md). Kept because
[`docs/aarch64_guest_plan.md`](../aarch64_guest_plan.md) cites this file as its
own explicit baseline ("narrower than the amd64 port").

---

# iSH-AOK amd64 Port Plan

Date: 2026-04-08

This document turns the amd64/x86_64 bring-up plan into a concrete patch series
for the current iSH-AOK tree. It is based on the code as it exists now, not on
an abstract emulator design.

## Hard Blockers

- Global guest-sized types are 32-bit. `misc.h` defines `addr_t`, `uint_t`,
  and many guest-visible scalars as 32-bit, and that assumption leaks through
  MM, exec, syscall marshalling, signals, and ptrace.
- CPU architectural state is i386-specific. `emu/cpu.h` only models 8 GPRs and
  `eip`; there is no room for `rip`, `r8-r15`, `orig_rax`, `fs_base`,
  `gs_base`, or `xmm8-xmm15`.
- The decoder is 16/32-bit only. `emu/decode.h` still treats `0x40..0x4f` as
  INC/DEC, and `emu/modrm.h` only supports 32-bit ModRM/SIB addressing.
- Memory management is still a fixed 4 GB design. `emu/memory.h` caps the guest
  VA with `MEM_PAGES`, `emu/memory.c` scans a hard-coded 32-bit hole range, and
  `kernel/exec.c` hard-codes 32-bit stack placement.
- Syscall entry is hard-wired to i386 ABI rules. `kernel/calls.c` reads the
  syscall number from `eax` and arguments from `ebx/ecx/edx/esi/edi/ebp`.
- ELF loading is ELF32/x86-only. `kernel/exec.c` rejects anything but
  `ELFCLASS32` plus `EM_386`, and the auxv/stack builder is 32-bit specific.
- TLS, signals, ptrace, and VDSO are all i386 ABI-specific. `arch_prctl` is not
  implemented, TLS relies on the current GS hack, signal frames are 32-bit
  layouts, ptrace exposes i386 register sets, and the built-in VDSO helper is
  ELF32-oriented.

## Execution Rules

- Treat amd64 as a new guest ISA/ABI, not as a widening of the existing i386
  port.
- Bring up amd64 in the interpreter first.
- Do not start with the JIT.
- Target x86-64-v1 first. No AVX, XSAVE, CET, MPX, or full x87 completeness
  work until dynamic Debian or Devuan userland is stable.
- Keep guest pages at 4 KB for Linux ABI compatibility even on iOS hosts with
  16 KB hardware pages.

## Concrete Patch Series

### 1. ABI Split Scaffolding

Files:

- `misc.h`
- `kernel/task.h`
- `kernel/calls.h`
- new `kernel/abi/{i386,amd64}.*`

Work:

- Introduce a per-task guest arch or ABI enum.
- Split internal kernel widths from guest ABI widths.
- Keep i386 guest layouts intact.
- Add explicit amd64 guest pointer, word, and register-width marshalling types.

Exit criteria:

- The tree still builds and runs i386 unchanged.
- No global type widening has happened yet.

### 2. Replace Fixed 4 GB MM Assumptions

Files:

- `emu/mmu.h`
- `emu/memory.h`
- `emu/memory.c`
- `kernel/mm.h`
- `kernel/mmap.c`
- `kernel/user.c`

Work:

- Replace `MEM_PAGES` as the global address-space boundary.
- Move from the current 32-bit page-directory model to a sparse 64-bit guest VA
  model.
- Make hole finding and mmap layout architecture-specific.
- Add canonical-address validation for the supported user range.
- Treat execute permission as a real access class instead of a mostly ignored
  flag.
- Design backing allocation around iOS host-page realities so 4 KB guest pages
  do not force wasteful one-host-page-per-guest-page behavior.

Exit criteria:

- i386 still runs on the new MM core.
- Sparse mappings at high guest addresses work.
- Fault handling and `/proc/pid/maps` remain correct.

### 3. Split ELF32 and ELF64 Loading

Files:

- `kernel/elf.h`
- `kernel/exec.c`
- `kernel/uname.c`

Work:

- Add separate ELF32 and ELF64 header and program-header parsing.
- Accept `EM_X86_64` for amd64 tasks.
- Add a distinct amd64 initial stack builder with 8-byte argc, argv, envp, and
  auxv entries.
- Preserve 16-byte stack alignment at entry.
- Set `AT_PLATFORM` to `x86_64`.
- Keep amd64 VDSO optional at first instead of blocking bring-up on it.

Exit criteria:

- A trivial static amd64 binary reaches `_start` and can exit successfully.

### 4. Widen CPU Architectural State

Files:

- `emu/cpu.h`
- `emu/regid.h`
- `jit/offsets.c`
- `kernel/task.h`

Work:

- Extend CPU state to 16 GPRs, `rip`, `rflags`, `orig_rax`, `fs_base`,
  `gs_base`, and XMM0-15.
- Preserve an i386 register overlay so existing interpreter code can continue to
  compile during the transition.
- Audit save and restore paths, cloning, ptrace snapshots, and crash logging.

Exit criteria:

- i386 still executes.
- amd64 state can be initialized, copied, and dumped correctly.

### 5. Add Long-Mode Decode Context

Files:

- `emu/decode.h`
- `emu/modrm.h`
- `emu/amd64_interp.c`

Work:

- Introduce an explicit decode context carrying guest mode, operand size,
  address size, and REX bits.
- Reinterpret `0x40..0x4f` as REX in long mode.
- Add extended register, index, and base decoding.
- Implement RIP-relative addressing.
- Enforce amd64 rules for default operand and address sizes.
- Implement the zero-extension rule for 32-bit writes to GPRs in long mode.

Exit criteria:

- Decode-only tests pass for REX prefixes, extended registers, 32-bit write
  zero-extension, and RIP-relative effective addresses.

### 6. Bring Up the amd64 Interpreter Core

Files:

- `emu/amd64_interp.c`
- `emu/cpuid.h`

Work:

- Implement the minimum instruction set needed by the dynamic loader and libc
  startup.
- Required integer core:
  - `mov`, `movzx`, `movsx`, `movsxd`, `lea`
  - `push`, `pop`
  - `add`, `sub`, `and`, `or`, `xor`, `cmp`, `test`
  - shifts and rotates
  - `imul`
  - `cmovcc`, `setcc`
  - `call`, `ret`, `jmp`, `jcc`
  - `xchg`, `xadd`, `cmpxchg`, `lock`
- Required platform ops:
  - `cpuid`
  - `rdtsc`
  - `syscall`
- Expose only a conservative x86-64-v1-level CPUID feature set.

Exit criteria:

- Hand-written amd64 asm tests pass for function prologues, PLT-style calls,
  atomic update loops, and syscall transitions.

### 7. Split Syscall Entry and Numbering

Files:

- `kernel/calls.c`
- `kernel/calls.h`
- new amd64 syscall table file

Work:

- Add a second syscall dispatch path for amd64.
- Use amd64 Linux rules:
  - syscall number in `rax`
  - arguments in `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9`
  - return in `rax`
  - `rcx` and `r11` clobbered on entry and exit
- Remove `socketcall` from the amd64 path and wire direct socket syscalls.
- Preserve separate syscall-number tables for i386 and amd64.

Exit criteria:

- Tiny asm syscall tests pass for `write`, `exit`, `openat`, `read`, `mmap`,
  `mprotect`, `brk`, `futex`, and `clone`.

### 8. Split Guest Struct Marshalling by ABI

Files:

- `kernel/calls.h`
- syscall implementation files for fs, time, epoll, poll, signal, stat, IPC,
  and resource handling

Work:

- Do not widen existing shared structs in place.
- Add amd64 marshalling for guest-visible layouts such as:
  - `iovec`
  - `timespec`, `timeval`, `itimerspec`
  - `stat`, `statx`
  - `rlimit`
  - `epoll_event`
  - `sigaction`
  - robust futex list structures
  - clone argument layouts where applicable
- Keep common syscall semantics underneath these wrappers.

Exit criteria:

- ABI-specific syscall family tests pass without requiring a full distro.

### 9. Fix TLS, Threads, and Signals

Files:

- `kernel/tls.c`
- `kernel/misc.c`
- `kernel/fork.c`
- `kernel/signal.h`
- `kernel/signal.c`

Work:

- Implement `arch_prctl(ARCH_SET_FS/GET_FS)` and make FS base first-class.
- Preserve current i386 TLS behavior separately.
- Add amd64 signal-frame layouts and `rt_sigreturn`.
- Handle amd64 red-zone rules correctly when building signal frames.
- Carry FS or GS base through clone and exec semantics as required.

Exit criteria:

- Dynamic glibc amd64 binaries start.
- `pthread_create`, `sigaltstack`, and basic signal delivery work.

### 10. Split VDSO and Trampoline Strategy

Files:

- `kernel/vdso.h`
- `kernel/vdso.c`
- `kernel/exec.c`
- `vdso/`

Work:

- Stop assuming the built-in VDSO image is good for both ABIs.
- Either:
  - add a separate amd64 VDSO image and ELF64 symbol lookup, or
  - omit `AT_SYSINFO*` for amd64 initially and provide only the signal-return
    trampoline through a separate mechanism
- Keep the amd64 bring-up unblocked if VDSO support lags slightly behind.

Exit criteria:

- Signal return works in amd64 without relying on the i386 VDSO image.

### 11. User-Visible Arch Plumbing and Diagnostics

Files:

- `kernel/uname.c`
- `fs/proc/root.c`
- `fs/proc/pid.c`
- guest-address logging sites such as `kernel/calls.c`

Work:

- Report `x86_64` where appropriate instead of `i686`.
- Audit all guest-address formatting and tracing for 64-bit correctness.
- Make `/proc` output and crash or fault logs render sane 64-bit addresses.

Exit criteria:

- `uname`, `/proc/cpuinfo`, `/proc/self/maps`, and tracing reflect amd64
  correctly.

### 12. Ptrace Then JIT

Files:

- `kernel/ptrace.h`
- `kernel/ptrace.c`
- then `jit/gen.c`
- `jit/jit.c`
- `jit/frame.h`
- `jit/gadgets-aarch64/*`
- `jit/gadgets-x86_64/*`

Work:

- Add amd64 ptrace register sets and syscall-stop state after basic userland is
  already booting.
- Only then start the amd64 JIT.
- Refactor JIT front-end assumptions around:
  - 8 GPR register files
  - `eip`
  - 32-bit decode templates
  - current gadget layouts

Exit criteria:

- Interpreter-only amd64 userland is stable before the JIT starts.
- amd64 JIT correctness is validated against the interpreter.

## Testing Strategy

### Milestone 1: Static asm smoke tests

- `_start`
- `write` and `exit`
- direct stack walking
- `syscall`
- REX and RIP-relative instruction coverage

### Milestone 2: Static userland

- tiny musl amd64 binaries
- tiny glibc amd64 binaries
- `uname`, `mmap`, `mprotect`, `futex`, `clone`, TLS smoke

### Milestone 3: Dynamic loader bring-up

- `ld-linux-x86-64.so.2 --help`
- loader-only environment setup
- auxv validation
- relocation coverage

### Milestone 4: Basic shell

- `/bin/sh`
- coreutils smoke
- pipes, signals, and `wait4`

### Milestone 5: Minimal distro

- Debian or Devuan minbase
- `dpkg`
- `apt`
- Python

### Milestone 6: Performance and JIT

- interpreter vs JIT differential instruction tests
- iOS memory pressure and jetsam profiling
- guest 4 KB page to host 16 KB page overhead measurement

## Priority Order

1. ABI split scaffolding
2. 64-bit-capable MM core
3. ELF64 loading and amd64 stack bootstrap
4. Long-mode CPU state and decode
5. amd64 interpreter plus `syscall`
6. amd64 syscall marshalling
7. TLS, threads, and signals
8. dynamic userland
9. ptrace
10. JIT

## Non-Goals for Initial Bring-Up

- AVX, AVX2, AVX-512
- XSAVE or full modern xstate handling
- CET
- MPX
- complete x87 corner-case parity before shell-level bring-up
- full amd64 VDSO parity before static amd64 binaries run

## Notes

- The first implementation branch should cover patches 1 through 3 only.
- The first release-quality amd64 milestone should be interpreter-only.
- Restore a non-JIT interpreter build option after the amd64 JIT path is
  complete and stable enough to stop owning the only amd64 execution mode.
- If forced to choose between early JIT work and correct glibc plus TLS
  semantics, always choose the latter.
