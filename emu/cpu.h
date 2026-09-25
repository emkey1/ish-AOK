#ifndef EMU_H
#define EMU_H

#include "misc.h"
#include "emu/mmu.h"
#include "emu/float80.h"

#ifdef __KERNEL__
#include <linux/stddef.h>
#else
#include <stddef.h>
#endif

struct cpu_state;
struct tlb;
struct task;
int cpu_run_to_interrupt(struct cpu_state *cpu, struct tlb *tlb);
int cpu_run_to_interrupt_amd64(struct cpu_state *cpu, struct tlb *tlb);
// Wired into jit.c's cpu_run_to_interrupt() dispatch as of
// aarch64_guest_plan.md patch 5 — interpreter-only, no native gadget
// engine yet (that's patch 8, jit/guest-arm64/).
int cpu_run_to_interrupt_arm64(struct cpu_state *cpu, struct tlb *tlb);
int amd64_jit_ret_imm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long imm16);
int amd64_jit_leave(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long pop_size, unsigned long next_ip);
int amd64_jit_pop_rm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip);
int amd64_jit_push_flags(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long push_size, unsigned long next_ip);
int amd64_jit_pop_flags(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long pop_size, unsigned long next_ip);
int amd64_jit_xchg_rm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip);
int amd64_jit_syscall(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip);
int amd64_jit_rdtsc(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip);
int amd64_jit_cpuid(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip);
int amd64_jit_xgetbv(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip);
int amd64_jit_vmcall(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip);
int amd64_jit_port_io(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long insn_ip);
int amd64_jit_moffs_accum(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip);
int amd64_jit_string_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip);
int amd64_jit_mov_imm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long reg_size, unsigned long value, unsigned long next_ip);
int amd64_jit_reg_reg_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long op_regs_size, unsigned long next_ip);
int amd64_jit_reg_imm_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long op_group_rm_size, unsigned long value, unsigned long next_ip);
int amd64_jit_imul_imm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip);
int amd64_jit_accum_imm_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip);
int amd64_jit_mem_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long meta, unsigned long disp, unsigned long next_ip);
int amd64_jit_movx(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long op2, unsigned long next_ip);
int amd64_jit_0f_rm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long op2, unsigned long next_ip);
int amd64_jit_0f_vec_rm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long op2, unsigned long next_ip);
int amd64_jit_grp3_test(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip);
int amd64_jit_loop_addr32(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long target_ip, unsigned long next_ip);
int amd64_jit_sse3_haddsub(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long op2, unsigned long next_ip);
int amd64_jit_0f38(struct cpu_state *cpu, struct tlb *tlb, unsigned long start_ip);
int amd64_jit_popcnt(struct cpu_state *cpu, struct tlb *tlb, unsigned long next_ip);
int amd64_jit_sreg(struct cpu_state *cpu, struct tlb *tlb, unsigned long next_ip);
int amd64_jit_iret(struct cpu_state *cpu, struct tlb *tlb, unsigned long start_ip);
int amd64_jit_ud2(struct cpu_state *cpu, struct tlb *tlb, unsigned long start_ip);
int amd64_jit_vex(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long lead, unsigned long start_ip);
int amd64_jit_x87(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip);
int amd64_jit_cmpxchg8b(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip);
int amd64_jit_grp3_op(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip);
int amd64_jit_modrm_imm(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip);
int amd64_jit_shift(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long opcode, unsigned long next_ip);
int amd64_jit_fe_group(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip);
int amd64_jit_ff_group(struct cpu_state *cpu, struct tlb *tlb,
        unsigned long next_ip);
void cpu_poke(struct cpu_state *cpu);
void dump_amd64_cc1_trace(const struct cpu_state *cpu);
void dump_amd64_as_trace_task(const struct task *task);
void dump_amd64_as_state_task(const struct task *task);
void dump_amd64_as_stack_task(const struct task *task);

enum amd64_reg {
    amd64_rax = 0,
    amd64_rcx = 1,
    amd64_rdx = 2,
    amd64_rbx = 3,
    amd64_rsp = 4,
    amd64_rbp = 5,
    amd64_rsi = 6,
    amd64_rdi = 7,
    amd64_r8 = 8,
    amd64_r9 = 9,
    amd64_r10 = 10,
    amd64_r11 = 11,
    amd64_r12 = 12,
    amd64_r13 = 13,
    amd64_r14 = 14,
    amd64_r15 = 15,
    amd64_reg_count = 16,
};

// AArch64 X0-X30. SP and PC are architecturally distinct registers (not
// X31 — X31 decodes to either XZR or SP depending on instruction context)
// so they get their own cpu_state fields instead of a 32nd array slot.
enum arm64_reg {
    arm64_x0 = 0, arm64_x1 = 1, arm64_x2 = 2, arm64_x3 = 3,
    arm64_x4 = 4, arm64_x5 = 5, arm64_x6 = 6, arm64_x7 = 7,
    arm64_x8 = 8, arm64_x9 = 9, arm64_x10 = 10, arm64_x11 = 11,
    arm64_x12 = 12, arm64_x13 = 13, arm64_x14 = 14, arm64_x15 = 15,
    arm64_x16 = 16, arm64_x17 = 17, arm64_x18 = 18, arm64_x19 = 19,
    arm64_x20 = 20, arm64_x21 = 21, arm64_x22 = 22, arm64_x23 = 23,
    arm64_x24 = 24, arm64_x25 = 25, arm64_x26 = 26, arm64_x27 = 27,
    arm64_x28 = 28, arm64_x29 = 29, // x29 is the frame pointer by convention
    arm64_x30 = 30, // link register (LR) by convention
    arm64_reg_count = 31,
};

// RISC-V x0-x31 with the standard ABI mnemonics. Unlike AArch64, SP and the
// TLS pointer are ordinary GPRs (x2/x4), so there are no out-of-band fields
// for them; only PC is architecturally separate. x0 is hardwired zero — the
// riscv64_regs[0] slot exists so register access stays uniform, but it is
// never written: decode redirects rd==x0 writebacks to riscv64_zero_sink
// (see struct cpu_state below).
enum riscv64_reg {
    riscv64_zero = 0, // hardwired zero
    riscv64_ra = 1,   // return address
    riscv64_sp = 2,   // stack pointer
    riscv64_gp = 3,   // global pointer
    riscv64_tp = 4,   // thread pointer (TLS base)
    riscv64_t0 = 5, riscv64_t1 = 6, riscv64_t2 = 7,
    riscv64_s0 = 8,   // frame pointer by convention
    riscv64_s1 = 9,
    riscv64_a0 = 10, riscv64_a1 = 11, riscv64_a2 = 12, riscv64_a3 = 13,
    riscv64_a4 = 14, riscv64_a5 = 15, riscv64_a6 = 16, riscv64_a7 = 17,
    riscv64_s2 = 18, riscv64_s3 = 19, riscv64_s4 = 20, riscv64_s5 = 21,
    riscv64_s6 = 22, riscv64_s7 = 23, riscv64_s8 = 24, riscv64_s9 = 25,
    riscv64_s10 = 26, riscv64_s11 = 27,
    riscv64_t3 = 28, riscv64_t4 = 29, riscv64_t5 = 30, riscv64_t6 = 31,
    riscv64_reg_count = 32,
};

// Full guest-visible amd64 register state is still a separate bring-up task.
// Until then, keep syscall-entry registers in a shadow block so the kernel can
// route amd64 syscalls without forcing the interpreter/JIT state layout over in
// one step.
struct amd64_syscall_state {
    qword_t rax;
    qword_t rdi;
    qword_t rsi;
    qword_t rdx;
    qword_t r10;
    qword_t r8;
    qword_t r9;
    qword_t rcx;
    qword_t r11;
};

#define AMD64_STORE_TRACE_COUNT 32
struct amd64_store_trace {
    qword_t rip;
    qword_t addr;
    qword_t value;
    uint8_t opcode;
};

union mm_reg {
    qword_t qw;
    dword_t dw[2];
};
union xmm_reg {
    unsigned __int128 u128;
    qword_t qw[2];
    uint32_t u32[4];
    uint16_t u16[8];
    uint8_t u8[16];
    float f32[4];
    double f64[2];
};
static_assert(sizeof(union xmm_reg) == 16, "xmm_reg size");
static_assert(sizeof(union mm_reg) == 8, "mm_reg size");

// A TLS entry as set_thread_area filled it: struct user_desc's base, its
// limit (20 bits) and its flags word (bits 0-6). flags is 0 for an empty
// entry, since a filled one is always 32-bit. See emu/i386_sreg.c.
struct i386_tls_desc {
    dword_t base;
    dword_t limit;
    dword_t flags;
};

struct cpu_state {
    struct mmu *mmu;
    long cycle;

    // general registers
    // assumes little endian (as does literally everything)
#define _REG(n) \
    union { \
        dword_t e##n; \
        word_t n; \
    }
#define _REGX(n) \
    union { \
        dword_t e##n##x; \
        word_t n##x; \
        struct { \
            byte_t n##l; \
            byte_t n##h; \
        }; \
    }

    union {
        struct {
            _REGX(a);
            _REGX(c);
            _REGX(d);
            _REGX(b);
            _REG(sp);
            _REG(bp);
            _REG(si);
            _REG(di);
        };
        dword_t regs[8];
    };
#undef REGX
#undef REG

    dword_t eip;

    qword_t amd64_regs[amd64_reg_count];
    qword_t amd64_rip;
    qword_t amd64_current_insn_rip;
    bool amd64_address_size_prefix;

    struct amd64_syscall_state amd64_syscall;
    struct amd64_store_trace amd64_store_trace[AMD64_STORE_TRACE_COUNT];
    unsigned amd64_store_trace_next;

    // AArch64 guest register file. Siblings of the i386/amd64 fields above,
    // not a union — same pattern as amd64_regs, so i386/amd64 tasks pay
    // nothing but the (currently zero-initialized, unused) memory. SP and PC
    // are separate from arm64_regs[] per the arm64_reg enum comment above.
    qword_t arm64_regs[arm64_reg_count];
    qword_t arm64_sp;
    qword_t arm64_pc;
    // NZCV condition flags, packed into bits 31:28 (N=31,Z=30,C=29,V=28)
    // matching AArch64 PSTATE/CPSR layout exactly, so the JIT gadget path
    // (jit/guest-arm64/) can load/store this with a single `msr nzcv, x`/
    // `mrs x, nzcv` pair instead of four separate bit-field touches.
    //
    // REVERSED from patch 2/3's original decoded-bool-fields design (one
    // representation, no sync-drift risk) once the project's direction
    // changed to porting OpenMinis' JIT gadget set directly rather than
    // building out an interpreter — see aarch64_guest_plan.md. With gadget
    // throughput as the actual goal, matching their packed representation
    // (their arm64_set_nzcv()/arm64_sync_nzcv() dance, emu/arch/arm64/
    // cpu.h) is the right tradeoff; the interpreter (emu/arm64_interp.c)
    // is no longer being extended and pays a small decode cost per flag
    // touch instead, which is fine since it's not the performance path.
    dword_t arm64_nzcv;
    // Exclusive monitor for LDXR/STXR (and CAS's fallback path). addr is
    // the guest address covered by the last LDXR; UINT64_MAX means "no
    // outstanding exclusive". Cleared on STXR/STLXR regardless of success,
    // per the architecture (a local monitor covers exactly one reservation).
    // excl_val_hi is the pair-high half armed by LDXP and checked by STXP
    // (arm64_ldxp/arm64_stxp, emu/tlb.c); single-register LDXR leaves it
    // stale, so a mixed LDXR->STXP sequence compares garbage — that
    // sequence is CONSTRAINED UNPREDICTABLE architecturally, and a
    // spurious status-1 just retries the guest's LL/SC loop.
    qword_t arm64_excl_addr;
    qword_t arm64_excl_val;
    qword_t arm64_excl_val_hi;
    // TPIDR_EL0 — the userspace-writable software thread ID register,
    // which every AArch64 libc uses as the TLS base pointer. Read/written
    // by the MRS/MSR gadgets (jit/guest-arm64/dpextra.S); the arm64
    // equivalent of i386's GDT-slot TLS / amd64's FS-base.
    qword_t arm64_tpidr;
    // V0-V31 are 128-bit, same layout as SSE's xmm[16] — reuse union xmm_reg
    // rather than duplicating an identical union under a new name.
    union xmm_reg arm64_v[32];
    dword_t arm64_fpsr;
    dword_t arm64_fpcr;

    // RISC-V (RV64GC) guest register file. Same sibling pattern as the
    // arm64 block above. riscv64_regs[0] is the hardwired-zero x0: reads
    // load it (it stays 0 forever), and decode redirects any rd==x0
    // writeback to riscv64_zero_sink so gadget bodies stay uniform without
    // a per-gadget "is rd zero" branch. SP (x2) and TP (x4, the TLS base)
    // live in the array — RISC-V has no out-of-band SP/TLS registers.
    qword_t riscv64_regs[riscv64_reg_count];
    qword_t riscv64_zero_sink;
    qword_t riscv64_pc;
    // LR/SC reservation (the RISC-V analog of arm64's exclusive monitor;
    // same design as arm64_excl_addr/_val, shared helper conventions in
    // emu/tlb.c). res_addr == UINT64_MAX means no outstanding reservation.
    // SC is implemented as reservation-check + CAS against res_val in one
    // atomic op; the reservation is PRESERVED across INT_PF so a CoW fault
    // mid LL/SC restarts correctly (the arm64_stxp lesson).
    qword_t riscv64_res_addr;
    qword_t riscv64_res_val;
    // f0-f31, 64-bit (D extension). Single-precision (F) values are
    // NaN-boxed into the low 32 bits per the ISA spec.
    qword_t riscv64_f[32];
    dword_t riscv64_fcsr;

    // flags
    union {
        dword_t eflags;
        struct {
            bitfield cf_bit:1;
            bitfield pad1_1:1;
            bitfield pf:1;
            bitfield pad2_0:1;
            bitfield af:1;
            bitfield pad3_0:1;
            bitfield zf:1;
            bitfield sf:1;
            bitfield tf:1;
            bitfield if_:1;
            bitfield df:1;
            bitfield of_bit:1;
            bitfield iopl:2;
        };
        // for asm
#define PF_FLAG (1 << 2)
#define AF_FLAG (1 << 4)
#define ZF_FLAG (1 << 6)
#define SF_FLAG (1 << 7)
#define DF_FLAG (1 << 10)
    };
    // please pretend this doesn't exist
    dword_t df_offset;
    // for maximum efficiency these are stored in bytes
    byte_t cf;
    byte_t of;
    // whether the true flag values are in the above struct, or computed from
    // the stored result and operands
    dword_t res, op1, op2;
    union {
        struct {
            bitfield pf_res:1;
            bitfield zf_res:1;
            bitfield sf_res:1;
            bitfield af_ops:1;
        };
        // for asm
#define PF_RES (1 << 0)
#define ZF_RES (1 << 1)
#define SF_RES (1 << 2)
#define AF_OPS (1 << 3)
        byte_t flags_res;
    };

    union mm_reg mm[8];
    union xmm_reg xmm[16];
    // fpu
    float80 fp[8];
    union {
        word_t fsw;
        struct {
            bitfield ie:1; // invalid operation
            bitfield de:1; // denormalized operand
            bitfield ze:1; // divide by zero
            bitfield oe:1; // overflow
            bitfield ue:1; // underflow
            bitfield pe:1; // precision
            bitfield stf:1; // stack fault
            bitfield es:1; // exception status
            bitfield c0:1;
            bitfield c1:1;
            bitfield c2:1;
            unsigned top:3;
            bitfield c3:1;
            bitfield b:1; // fpu busy (?)
        };
    };
    union {
        word_t fcw;
        struct {
            bitfield im:1;
            bitfield dm:1;
            bitfield zm:1;
            bitfield om:1;
            bitfield um:1;
            bitfield pm:1;
            bitfield pad4:2;
            bitfield pc:2;
            bitfield rc:2;
            bitfield y:1;
        };
    };

    // SSE control/status word (ldmxcsr/stmxcsr; also reported by fxsave). SSE
    // runs round-to-nearest with all exceptions masked, so the rounding/exception
    // bits are not yet honored, but the value is stored so a control-word
    // read-modify-write round-trips (Free Pascal's FPU init does this).
    dword_t mxcsr;

    // The base an FS override adds on i386: that of the descriptor FS
    // selected when it was loaded, 0 unless that is a TLS entry. It sits in
    // what was padding before tls_ptr, where the JIT's seg_fs gadget reaches
    // it with an immediate. See emu/i386_sreg.c.
    dword_t i386_fs_base;
    // The TLS base: FS's on amd64 (ARCH_SET_FS, CLONE_SETTLS), and on i386
    // GS's, the base of the descriptor GS selected when it was loaded (see
    // emu/i386_sreg.c). The JIT's seg_gs gadget adds it to every GS-relative
    // address.
    guest_addr_t tls_ptr;

    // for the page fault handler
    guest_addr_t segfault_addr;
    bool segfault_was_write;
    // The i386 JIT's memory-fault exits (segfault_read/segfault_write) set
    // this and its interrupt gadget clears it: segfault_addr then came from
    // a failed access, and is an address even when it is 0. The GPF handler
    // used to read 0 as "no address reported", so a NULL dereference fell
    // through to its opcode decoder, and anything the decoder does not know
    // (`cmp eax, [ecx]`, HotSpot's implicit null check) became SIGSEGV
    // SI_KERNEL at the PC instead of SEGV_MAPERR at 0. It sits in what was
    // padding before trapno, so no offset moves.
    bool segfault_reported;

    dword_t trapno;
    // access atomically
    bool *poked_ptr;
    bool _poked;

    // ---- AVX / AVX-512 state (GH #525) ----
    //
    // Deliberately LAST in the struct. The JIT's gadgets reach cpu_state
    // fields with immediate offsets of limited range (12-bit unsigned for
    // `add`, 9-bit signed for ldr/str), so growing anything in the MIDDLE of
    // the struct pushes later fields out of reach and fails to assemble.
    // Appending is always safe.
    //
    // xmm[0-15] above holds bits 0-127 of the first sixteen registers;
    // xmm_ext holds the same for registers 16-31, which only EVEX can name.
    // ymm_hi[i] is bits 128-255 of register i and zmm_hi[i] bits 256-511.
    // Splitting it this way rather than widening xmm[] keeps every existing
    // SSE gadget's cpu->xmm[i] offset exactly where it was.
    //
    // Not threaded through ptrace GETFPREGS / signal-frame construction /
    // clone: those only need state observable across a syscall, signal, or
    // debugger boundary, which no current guest workload exercises for
    // vector state. A debugger inspecting %ymm mid-signal won't see it --
    // a known, narrow gap, not a straight-line-execution bug.
    union xmm_reg xmm_ext[16];
    union xmm_reg ymm_hi[32];
    struct {
        uint8_t u8[32];
    } zmm_hi[32];
    // EVEX opmask registers k0-k7. k0 architecturally means "no masking" and
    // is never written by a masked instruction, but is stored like any other.
    uint64_t avx512_k[8];

    // amd64 segment selectors, indexed by the ModRM Sreg encoding
    // (AMD64_SREG_*): what `mov Sreg, r/m` and `pop fs/gs` last loaded into
    // ES, DS, FS and GS. Zero from exec, as Linux's start_thread leaves them,
    // and carried across fork and signal delivery, as Linux carries them. The
    // CS and SS slots are unused: a 64-bit user task's CS is always 0x33, and
    // 0x2b is the only selector its SS can be loaded with. See
    // amd64_sreg_op in emu/amd64_interp.c.
    word_t amd64_sreg[6];

    // i386 segment state; see emu/i386_sreg.c. i386_sreg holds what ES, SS,
    // DS, FS and GS were last loaded with -- by 8E, POP, sigreturn, ptrace or
    // exec -- indexed by the Sreg encoding, as amd64_sreg is. The CS slot is
    // unused: a 32-bit task's CS is always 0x23. i386_tls holds GDT entries
    // 12-14, the ones set_thread_area fills. Both are inherited by fork and
    // reset by exec, as Linux's are. FS's and GS's bases are i386_fs_base and
    // tls_ptr above.
    word_t i386_sreg[6];
    struct i386_tls_desc i386_tls[3];
};

#define AMD64_SREG_ES 0
#define AMD64_SREG_CS 1
#define AMD64_SREG_SS 2
#define AMD64_SREG_DS 3
#define AMD64_SREG_FS 4
#define AMD64_SREG_GS 5
#define AMD64_SEL_USER_CS 0x33
#define AMD64_SEL_USER_DS 0x2b

#define CPU_OFFSET(field) offsetof(struct cpu_state, field)

static_assert(CPU_OFFSET(eax) == CPU_OFFSET(regs[0]), "register order");
static_assert(CPU_OFFSET(ecx) == CPU_OFFSET(regs[1]), "register order");
static_assert(CPU_OFFSET(edx) == CPU_OFFSET(regs[2]), "register order");
static_assert(CPU_OFFSET(ebx) == CPU_OFFSET(regs[3]), "register order");
static_assert(CPU_OFFSET(esp) == CPU_OFFSET(regs[4]), "register order");
static_assert(CPU_OFFSET(ebp) == CPU_OFFSET(regs[5]), "register order");
static_assert(CPU_OFFSET(esi) == CPU_OFFSET(regs[6]), "register order");
static_assert(CPU_OFFSET(edi) == CPU_OFFSET(regs[7]), "register order");
static_assert(sizeof(struct cpu_state) < 0xffff, "cpu struct is too big for vector gadgets");

// flags
#define ZF (cpu->zf_res ? cpu->res == 0 : cpu->zf)
#define SF (cpu->sf_res ? (int32_t) cpu->res < 0 : cpu->sf)
#define CF (cpu->cf)
#define OF (cpu->of)
#define PF (cpu->pf_res ? !__builtin_parity(cpu->res & 0xff) : cpu->pf)
#define AF (cpu->af_ops ? ((cpu->op1 ^ cpu->op2 ^ cpu->res) >> 4) & 1 : cpu->af)

static inline void collapse_flags(struct cpu_state *cpu) {
    cpu->zf = ZF;
    cpu->sf = SF;
    cpu->pf = PF;
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    cpu->of_bit = cpu->of;
    cpu->cf_bit = cpu->cf;
    cpu->af = AF;
    cpu->af_ops = 0;
    cpu->pad1_1 = 1;
    cpu->pad2_0 = cpu->pad3_0 = 0;
    cpu->if_ = 1;
}

static inline void expand_flags(struct cpu_state *cpu) {
    cpu->of = cpu->of_bit;
    cpu->cf = cpu->cf_bit;
    cpu->zf_res = cpu->sf_res = cpu->pf_res = cpu->af_ops = 0;
}

enum reg32 {
    reg_eax = 0, reg_ecx, reg_edx, reg_ebx, reg_esp, reg_ebp, reg_esi, reg_edi, reg_count,
    reg_none = reg_count,
};

static inline const char *reg32_name(enum reg32 reg) {
    switch (reg) {
        case reg_eax: return "eax";
        case reg_ecx: return "ecx";
        case reg_edx: return "edx";
        case reg_ebx: return "ebx";
        case reg_esp: return "esp";
        case reg_ebp: return "ebp";
        case reg_esi: return "esi";
        case reg_edi: return "edi";
        default: return "?";
    }
}

#endif
