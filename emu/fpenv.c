// The guest's floating-point environment on the host FPU.
//
// Every engine does IEEE arithmetic on the host FPU: native gadgets for the
// arm64 and riscv64 guests and for amd64 SSE, C helpers (emu/vec.c, avx.c,
// amd64_interp.c) for the rest of SSE and AVX. The x87 is the exception -- it
// is soft-float (emu/float80.c) with its own control and status words, which
// emu/fpu.c looks after. So for everything else, the guest's rounding mode has
// to be the host's while guest code runs, and the flags its operations raise
// arrive in the host's sticky flags. Before this, neither happened: every
// guest ran in the host's default round-to-nearest, whatever fesetround()
// said, and no flag was ever raised.
//
// Measured on an Apple M5, per instruction: FDIV 1.2 ns, MRS FPSR 12 ns, MSR
// FPSR 26 ns, an MSR FPCR that changes the mode 22 ns (4 ns when it does not).
// Switching the host FPCR around each operation would make FP code 20-40x
// slower, so nothing here runs per operation:
//
//  - Control is installed by fpenv_enter and when the guest writes it
//    (LDMXCSR and FXRSTOR, MSR FPCR, a CSR write of frm or fcsr), each a
//    compare against what this host thread already has. It is NOT taken off
//    at exit: that cost two FPCR writes per syscall (~45 ns) for any guest not
//    in the host's default -- every riscv64 guest, since DN is always set for
//    it. So emulator C code on a guest's thread runs in the guest's mode; its
//    floating point is cosmetic (process accounting, checkpoint timing,
//    profiling) and a last-bit difference there is harmless. A native program
//    replacing the image starts from the host default (fpenv_host_default).
//  - Flags accumulate in the host FPSR while guest code runs. fpenv_exit folds
//    them into the guest's own register (MXCSR, FPSR, fflags), and so does a
//    guest read of that register (STMXCSR, FXSAVE, MRS FPSR, a CSR read). A
//    guest write of its flags discards the host's. Only a read is paid on each
//    exit and entry; see fpenv_enter for when the FPSR is written.
//
// Between an exit and the next enter the whole FP state is in cpu_state, and
// that is what signal delivery, sigreturn, fork, ptrace, exec and checkpoints
// read and write -- so none of them needs to know about any of this: the next
// fpenv_enter installs whatever it finds. Emulator C code runs in the host's
// default mode, and whatever it raises is dropped at enter rather than charged
// to the guest.
//
// Differences from the real hardware that remain:
//  - x86 DAZ has no aarch64 equivalent. MXCSR.FTZ maps to FPCR.FZ, which also
//    flushes denormal inputs.
//  - Underflow: aarch64 detects tininess before rounding, x86 and RISC-V after,
//    so a result that rounds up to the smallest normal raises UE/UF on one and
//    not the other.
//  - RISC-V RMM (round to nearest, ties to max magnitude) has no FPCR encoding
//    and runs as RNE in arithmetic; conversions that name it statically are
//    exact (fcvta).
//  - Exception traps (unmasked MXCSR exceptions, FPCR trap enables,
//    feenableexcept) are not delivered. The arm64 enables read back as zero,
//    as on hardware without trapping, so glibc's feenableexcept reports
//    failure instead of silently not trapping.

#include "emu/fpenv.h"
#include "emu/cpu.h"
#include "emu/fpu.h"
#include "kernel/abi.h"

// Host FPCR/FPSR in the aarch64 layout. On other hosts fenv.h stands in, and
// the flags are converted to this layout.
#define FPCR_RMODE_SHIFT 22
#define FPCR_RMODE (3ull << FPCR_RMODE_SHIFT)
#define FPCR_FZ16 (1ull << 19)
#define FPCR_FZ (1ull << 24)
#define FPCR_DN (1ull << 25)
#define FPCR_AHP (1ull << 26)
// The bits a guest may control; trap enables and FEAT_AFP stay the host's.
#define FPCR_GUEST (FPCR_RMODE | FPCR_FZ16 | FPCR_FZ | FPCR_DN | FPCR_AHP)
enum { RMODE_RN = 0, RMODE_RP = 1, RMODE_RM = 2, RMODE_RZ = 3 };

#define FPSR_IOC (1ull << 0)
#define FPSR_DZC (1ull << 1)
#define FPSR_OFC (1ull << 2)
#define FPSR_UFC (1ull << 3)
#define FPSR_IXC (1ull << 4)
#define FPSR_IDC (1ull << 7)
#define FPSR_QC (1ull << 27)
#define FPSR_FLAGS (FPSR_IOC | FPSR_DZC | FPSR_OFC | FPSR_UFC | FPSR_IXC | FPSR_IDC | FPSR_QC)

#if defined(__aarch64__)

static inline uint64_t host_fpcr_read(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v) : : "memory");
    return v;
}
static inline void host_fpcr_write(uint64_t v) {
    __asm__ volatile("msr fpcr, %0" : : "r"(v) : "memory");
}
static inline uint64_t host_fpsr_read(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, fpsr" : "=r"(v) : : "memory");
    return v;
}
static inline void host_fpsr_write(uint64_t v) {
    __asm__ volatile("msr fpsr, %0" : : "r"(v) : "memory");
}

#else

#include <fenv.h>

// Only the rounding mode and the five IEEE flags are reachable portably.
static inline uint64_t host_fpcr_read(void) {
    switch (fegetround()) {
#ifdef FE_UPWARD
        case FE_UPWARD: return (uint64_t) RMODE_RP << FPCR_RMODE_SHIFT;
#endif
#ifdef FE_DOWNWARD
        case FE_DOWNWARD: return (uint64_t) RMODE_RM << FPCR_RMODE_SHIFT;
#endif
#ifdef FE_TOWARDZERO
        case FE_TOWARDZERO: return (uint64_t) RMODE_RZ << FPCR_RMODE_SHIFT;
#endif
        default: return 0;
    }
}
static inline void host_fpcr_write(uint64_t v) {
    static const int modes[4] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    fesetround(modes[(v & FPCR_RMODE) >> FPCR_RMODE_SHIFT]);
}
static inline uint64_t host_fpsr_read(void) {
    int e = fetestexcept(FE_ALL_EXCEPT);
    uint64_t f = 0;
    if (e & FE_INVALID) f |= FPSR_IOC;
    if (e & FE_DIVBYZERO) f |= FPSR_DZC;
    if (e & FE_OVERFLOW) f |= FPSR_OFC;
    if (e & FE_UNDERFLOW) f |= FPSR_UFC;
    if (e & FE_INEXACT) f |= FPSR_IXC;
    return f;
}
static inline void host_fpsr_write(uint64_t v) {
    (void) v;
    feclearexcept(FE_ALL_EXCEPT);
}

#endif

// What this host thread has in its FPCR, so that installing the same thing
// again costs a compare rather than an MSR.
static __thread struct {
    bool init;
    uint64_t base;      // the host FPCR with the guest-controlled bits cleared
    uint64_t dflt;      // the guest-controlled bits as the host had them
    uint64_t installed; // the guest-controlled bits in the FPCR now
} host;

static void host_install(uint64_t bits) {
    if (!host.init) {
        uint64_t v = host_fpcr_read();
        host.base = v & ~FPCR_GUEST;
        host.dflt = host.installed = v & FPCR_GUEST;
        host.init = true;
    }
    if (bits != host.installed) {
        host_fpcr_write(host.base | bits);
        host.installed = bits;
    }
}

static void host_install_default(void) {
    if (host.init)
        host_install(host.dflt);
}

// Returns the host's sticky flags and clears them.
static uint64_t host_take_flags(void) {
    uint64_t v = host_fpsr_read();
    if (v & FPSR_FLAGS)
        host_fpsr_write(v & ~FPSR_FLAGS);
    return v & FPSR_FLAGS;
}

// ---- x86: MXCSR ------------------------------------------------------------

#define MXCSR_IE (1u << 0)
#define MXCSR_DE (1u << 1)
#define MXCSR_ZE (1u << 2)
#define MXCSR_OE (1u << 3)
#define MXCSR_UE (1u << 4)
#define MXCSR_PE (1u << 5)
#define MXCSR_RC_SHIFT 13
#define MXCSR_FTZ (1u << 15)

static uint64_t x86_control(const struct cpu_state *cpu) {
    // RC: nearest, down, up, toward zero.
    static const uint8_t rmode[4] = {RMODE_RN, RMODE_RM, RMODE_RP, RMODE_RZ};
    uint64_t bits = (uint64_t) rmode[(cpu->mxcsr >> MXCSR_RC_SHIFT) & 3] << FPCR_RMODE_SHIFT;
    if (cpu->mxcsr & MXCSR_FTZ)
        bits |= FPCR_FZ;
    return bits;
}

static void x86_fold(struct cpu_state *cpu, uint64_t f) {
    dword_t m = 0;
    if (f & FPSR_IOC) m |= MXCSR_IE;
    if (f & FPSR_IDC) m |= MXCSR_DE;
    if (f & FPSR_DZC) m |= MXCSR_ZE;
    if (f & FPSR_OFC) m |= MXCSR_OE;
    if (f & FPSR_UFC) m |= MXCSR_UE;
    if (f & FPSR_IXC) m |= MXCSR_PE;
    // A result flushed to zero by FTZ is inexact, and x86 says so. aarch64's
    // FZ raises only UFC for it, and with FZ on UFC means exactly that.
    if ((f & FPSR_UFC) && (cpu->mxcsr & MXCSR_FTZ))
        m |= MXCSR_PE;
    cpu->mxcsr |= m;
}

void fpenv_x86_sync_mxcsr(struct cpu_state *cpu) {
    uint64_t f = host_take_flags();
    if (f)
        x86_fold(cpu, f);
}

void fpenv_x86_load_mxcsr(struct cpu_state *cpu) {
    host_take_flags();
    host_install(x86_control(cpu));
}

// ---- arm64: FPCR/FPSR -------------------------------------------------------

// What MSR FPCR keeps. Everything else reads back as zero: the trap enables
// (as on cores without FP trapping, Apple's included) and FEAT_AFP, which the
// guest is not told about.
#define ARM64_FPCR_WRITABLE FPCR_GUEST
#define ARM64_FPSR_WRITABLE FPSR_FLAGS

uint64_t fpenv_arm64_sysreg(struct cpu_state *cpu, unsigned op, uint64_t value) {
    switch (op) {
        case 0:
            return cpu->arm64_fpcr;
        case 1:
            cpu->arm64_fpcr = (dword_t) (value & ARM64_FPCR_WRITABLE);
            host_install(cpu->arm64_fpcr & FPCR_GUEST);
            return 0;
        case 2:
            cpu->arm64_fpsr |= (dword_t) host_take_flags();
            return cpu->arm64_fpsr;
        case 3:
            host_take_flags();
            cpu->arm64_fpsr = (dword_t) (value & ARM64_FPSR_WRITABLE);
            return 0;
    }
    return 0;
}

// ---- riscv64: fcsr ------------------------------------------------------------

// fflags: NV DZ OF UF NX in bits 4..0 -- the aarch64 order reversed.
#define RV_NX (1u << 0)
#define RV_UF (1u << 1)
#define RV_OF (1u << 2)
#define RV_DZ (1u << 3)
#define RV_NV (1u << 4)

static uint64_t riscv64_control(const struct cpu_state *cpu) {
    // frm: RNE, RTZ, RDN, RUP, RMM. RMM has no FPCR encoding (see the top of
    // this file); 5 and 6 are reserved and 7 is not a valid frm.
    static const uint8_t rmode[8] = {RMODE_RN, RMODE_RZ, RMODE_RM, RMODE_RP,
                                     RMODE_RN, RMODE_RN, RMODE_RN, RMODE_RN};
    // RISC-V gives every NaN result the canonical NaN, which is what DN does.
    return ((uint64_t) rmode[(cpu->riscv64_fcsr >> 5) & 7] << FPCR_RMODE_SHIFT) | FPCR_DN;
}

static void riscv64_fold(struct cpu_state *cpu, uint64_t f) {
    dword_t m = 0;
    if (f & FPSR_IOC) m |= RV_NV;
    if (f & FPSR_DZC) m |= RV_DZ;
    if (f & FPSR_OFC) m |= RV_OF;
    if (f & FPSR_UFC) m |= RV_UF;
    if (f & FPSR_IXC) m |= RV_NX;
    cpu->riscv64_fcsr |= m;
}

void fpenv_riscv64_sync_fflags(struct cpu_state *cpu) {
    uint64_t f = host_take_flags();
    if (f)
        riscv64_fold(cpu, f);
}

void fpenv_riscv64_load_fcsr(struct cpu_state *cpu, bool flags_written) {
    if (flags_written)
        host_take_flags();
    host_install(riscv64_control(cpu));
}

void fpenv_raise_invalid(void) {
#if defined(__aarch64__)
    host_fpsr_write(host_fpsr_read() | FPSR_IOC);
#else
    feraiseexcept(FE_INVALID);
#endif
}

// ---- entry and exit -------------------------------------------------------------

// The guest's own flags, in host FPSR bits: what the host FPSR may still hold
// without anything being charged to the guest twice or wrongly.
static uint64_t guest_flags_as_host(const struct cpu_state *cpu, int abi) {
    uint64_t f = 0;
    switch (abi) {
        case GUEST_ABI_I386:
        case GUEST_ABI_AMD64:
            if (cpu->mxcsr & MXCSR_IE) f |= FPSR_IOC;
            if (cpu->mxcsr & MXCSR_DE) f |= FPSR_IDC;
            if (cpu->mxcsr & MXCSR_ZE) f |= FPSR_DZC;
            if (cpu->mxcsr & MXCSR_OE) f |= FPSR_OFC;
            if (cpu->mxcsr & MXCSR_UE) f |= FPSR_UFC;
            if (cpu->mxcsr & MXCSR_PE) f |= FPSR_IXC;
            break;
        case GUEST_ABI_ARM64:
            f = cpu->arm64_fpsr & FPSR_FLAGS;
            break;
        case GUEST_ABI_RISCV64:
            if (cpu->riscv64_fcsr & RV_NV) f |= FPSR_IOC;
            if (cpu->riscv64_fcsr & RV_DZ) f |= FPSR_DZC;
            if (cpu->riscv64_fcsr & RV_OF) f |= FPSR_OFC;
            if (cpu->riscv64_fcsr & RV_UF) f |= FPSR_UFC;
            if (cpu->riscv64_fcsr & RV_NX) f |= FPSR_IXC;
            break;
    }
    return f;
}

// Writing the FPSR costs about as much as a syscall's worth of bookkeeping
// (26+ ns), so exit only reads it: whatever it folds stays in the host FPSR,
// which is harmless while it is a subset of the guest's own flags -- folding
// is an OR. Enter clears it only when it holds a bit the guest does not have:
// one raised by emulator C code since exit (process accounting, checkpoint
// timing and native programs all do floating point), or one the guest no
// longer has because sigreturn, exec or ptrace replaced its flags. A guest
// write of its flags clears the host FPSR at once (host_take_flags).
void fpenv_enter(struct cpu_state *cpu, int abi) {
    switch (abi) {
        case GUEST_ABI_I386:
        case GUEST_ABI_AMD64:
            host_install(x86_control(cpu));
            // float80's rounding and precision are per host thread; the
            // control word they follow is per guest thread, and may have been
            // changed by sigreturn, ptrace, exec or a checkpoint restore since.
            fpu_sync_control(cpu);
            break;
        case GUEST_ABI_ARM64:
            host_install(cpu->arm64_fpcr & FPCR_GUEST);
            break;
        case GUEST_ABI_RISCV64:
            host_install(riscv64_control(cpu));
            break;
    }
    uint64_t live = host_fpsr_read();
    if (live & FPSR_FLAGS & ~guest_flags_as_host(cpu, abi))
        host_fpsr_write(live & ~FPSR_FLAGS);
}

void fpenv_host_default(void) {
    host_install_default();
}

void fpenv_exit(struct cpu_state *cpu, int abi) {
    uint64_t f = host_fpsr_read() & FPSR_FLAGS;
    if (f) {
        switch (abi) {
            case GUEST_ABI_I386:
            case GUEST_ABI_AMD64:
                x86_fold(cpu, f);
                break;
            case GUEST_ABI_ARM64:
                cpu->arm64_fpsr |= (dword_t) f;
                break;
            case GUEST_ABI_RISCV64:
                riscv64_fold(cpu, f);
                break;
        }
    }
}
