#ifndef EMU_FPENV_H
#define EMU_FPENV_H

#include <stdbool.h>
#include <stdint.h>

// The guest's floating-point environment -- rounding mode and sticky exception
// flags -- on the host FPU. See emu/fpenv.c for the design.

struct cpu_state;

// Around guest execution: cpu_run_to_interrupt calls these, so every engine
// and every exit (syscall, fault, timer, crash unwind) is covered. abi is the
// running task's guest ABI (kernel/abi.h).
void fpenv_enter(struct cpu_state *cpu, int abi);
void fpenv_exit(struct cpu_state *cpu, int abi);

// The guest's control stays on the host FPU after exit (see fpenv.c). Host
// code that must not inherit it -- a native program starting on the thread --
// calls this first.
void fpenv_host_default(void);

// x86 (i386 and amd64). Call sync before reading cpu->mxcsr's flags from
// inside guest execution (STMXCSR, FXSAVE), and load after writing cpu->mxcsr
// (LDMXCSR, FXRSTOR). The x87 control word is soft-float state; fpu.c keeps
// float80's rounding and precision in step with it.
void fpenv_x86_sync_mxcsr(struct cpu_state *cpu);
void fpenv_x86_load_mxcsr(struct cpu_state *cpu);

// arm64 MRS/MSR of FPCR and FPSR, from the gadgets. op: 0 = MRS FPCR,
// 1 = MSR FPCR, 2 = MRS FPSR, 3 = MSR FPSR. Returns the value an MRS reads.
uint64_t fpenv_arm64_sysreg(struct cpu_state *cpu, unsigned op, uint64_t value);

// riscv64: sync before reading fflags (fcsr bits 4:0); load after writing any
// of fcsr, and pass whether the flags themselves were written.
void fpenv_riscv64_sync_fflags(struct cpu_state *cpu);
void fpenv_riscv64_load_fcsr(struct cpu_state *cpu, bool flags_written);

// For a C helper that supplies an x86 invalid operation's result itself (the
// negative indefinite NaN) and so never ran the host operation that would
// have raised the flag: raise it, to be folded into MXCSR with the rest.
void fpenv_raise_invalid(void);

#endif
