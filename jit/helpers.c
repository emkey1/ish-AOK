#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include "emu/cpu.h"
#include "emu/cpuid.h"
#include "emu/tlb.h"
#include "kernel/task.h"
#include "util/sync.h"
#include "emu/vec.h"

void helper_cpuid(dword_t *a, dword_t *b, dword_t *c, dword_t *d) {
    do_cpuid(a, b, c, d);
}

int cpuid_long_mode_override = -1;
struct cpuid_table cpuid_tables[2];

static void cpuid_fill(struct cpuid_answer *ans, dword_t leaf, dword_t subleaf) {
    dword_t a = leaf, b = 0, c = subleaf, d = 0;
    do_cpuid(&a, &b, &c, &d);
    *ans = (struct cpuid_answer) {a, b, c, d};
}

__attribute__((constructor)) void cpuid_tables_init(void) {
    for (int lm = 0; lm < 2; lm++) {
        cpuid_long_mode_override = lm;
        struct cpuid_answer *s = cpuid_tables[lm].slot;
        cpuid_fill(&s[CPUID_SLOT_LEAF0], 0, 0);
        cpuid_fill(&s[CPUID_SLOT_LEAF1], 1, 0);
        cpuid_fill(&s[CPUID_SLOT_LEAF7_0], 7, 0);
        cpuid_fill(&s[CPUID_SLOT_LEAFD_0], 0xd, 0);
        cpuid_fill(&s[CPUID_SLOT_LEAFD_1], 0xd, 1);
        cpuid_fill(&s[CPUID_SLOT_LEAFD_2], 0xd, 2);
        cpuid_fill(&s[CPUID_SLOT_LEAFD_5], 0xd, 5);
        cpuid_fill(&s[CPUID_SLOT_LEAFD_6], 0xd, 6);
        cpuid_fill(&s[CPUID_SLOT_LEAFD_7], 0xd, 7);
        cpuid_fill(&s[CPUID_SLOT_EXT0], 0x80000000u, 0);
        cpuid_fill(&s[CPUID_SLOT_EXT1], 0x80000001u, 0);
        cpuid_fill(&s[CPUID_SLOT_ABOVE_BASIC], cpuid_basic_max_leaf() + 1, 0);
        cpuid_fill(&s[CPUID_SLOT_ZERO], 2, 0);
        cpuid_fill(&s[CPUID_SLOT_ABOVE_EXT], 0x80000002u, 0);
    }
    cpuid_long_mode_override = -1;
}

// XGETBV for the i386 guest. Takes the same four-register frame as
// helper_cpuid so it can reuse that gadget's shape exactly: ecx selects the
// register on the way in, eax/edx take the value on the way out, ebx is
// untouched.
//
// Deviation: hardware raises #GP for any index other than 0, and we return
// zero instead. A gadget cannot raise an interrupt without going through the
// interrupt gadget, which is a lot of machinery for a case no real caller
// reaches -- XCR0 is the only register defined here, and glibc, gcc's ifunc
// resolvers and OpenSSL all pass 0. The amd64 interpreter, where raising it
// costs nothing, does return #GP.
void helper_xgetbv(dword_t *a, dword_t *b, dword_t *c, dword_t *d) {
    (void) b;
    qword_t value = *c == 0 ? xcr0_value() : 0;
    *a = (dword_t) value;
    *d = (dword_t) (value >> 32);
}

void helper_rdtsc(struct cpu_state *cpu) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t tsc = now.tv_sec * 1000000000l + now.tv_nsec;
    cpu->eax = tsc & 0xffffffff;
    cpu->edx = tsc >> 32;
    cpu->amd64_regs[amd64_rax] = cpu->eax;
    cpu->amd64_regs[amd64_rdx] = cpu->edx;
}

void helper_expand_flags(struct cpu_state *cpu) {
    expand_flags(cpu);
}

void helper_collapse_flags(struct cpu_state *cpu) {
    collapse_flags(cpu);
}

// Misaligned i386 LOCK operations, 16 and 32 bits. The gadgets' fast path is
// an ARM exclusive pair (ldaxr/stlxr), which needs a naturally aligned host
// address -- a misaligned one is an alignment fault that killed the whole host
// process (a `lock addl` at buf+15 took the app down). x86 permits any
// alignment, so the gadgets test it and bring a misaligned operand here, where
// x86_atomic_rmw/x86_atomic_cas do it exactly (see "Misaligned LOCK" in
// emu/tlb.c): a 16-byte host compare-exchange inside one aligned block, the
// address space's writer lock across two.
//
// The flags left behind are the gadgets' own lazy deposit, so every reader
// sees the same state either way: op1/op2/res for the add family (res
// sign-extended to 32 bits, as setf_zsp leaves it), eager CF/OF, flags_res
// with ZF/SF/PF_RES and AF_OPS; the logic family clears CF, OF and AF.
//
// Returns the gadget's new _tmp in the low 32 bits (the old value for xadd
// and xchg, else unchanged), or 1 << 32 on a fault with the fault address set
// for the gadget's segfault_write exit.
enum {
    UA_ADD, UA_SUB, UA_ADC, UA_SBB, UA_AND, UA_OR, UA_XOR, UA_INC, UA_DEC,
    UA_XADD, UA_NOT, UA_NEG, UA_XCHG, UA_CMPXCHG, UA_BTS, UA_BTR, UA_BTC,
};
struct ua_ctx { unsigned op, bits; dword_t operand, cin; };

static qword_t ua_mask(unsigned bits) { return bits == 32 ? 0xffffffffull : (1ull << bits) - 1; }

static qword_t ua_apply(qword_t old, void *p) {
    struct ua_ctx *c = p;
    qword_t x = old, y = c->operand, r;
    switch (c->op) {
    case UA_ADD: r = x + y; break;
    case UA_ADC: r = x + y + c->cin; break;
    case UA_SUB: r = x - y; break;
    case UA_SBB: r = x - y - c->cin; break;
    case UA_AND: r = x & y; break;
    case UA_OR: r = x | y; break;
    case UA_XOR: r = x ^ y; break;
    case UA_INC: r = x + 1; break;
    case UA_DEC: r = x - 1; break;
    case UA_XADD: r = x + y; break;
    case UA_NOT: r = ~x; break;
    case UA_NEG: r = 0 - x; break;
    case UA_XCHG: r = y; break;
    case UA_BTS: r = x | (1ull << (y & (c->bits - 1))); break;
    case UA_BTR: r = x & ~(1ull << (y & (c->bits - 1))); break;
    case UA_BTC: r = x ^ (1ull << (y & (c->bits - 1))); break;
    default: r = x; break;
    }
    return r & ua_mask(c->bits);
}

static int32_t ua_sext(qword_t v, unsigned bits) {
    return bits == 32 ? (int32_t) v : bits == 16 ? (int16_t) v : (int8_t) v;
}

// The add family's deposit: dst OP src (+ carry in), with op1 = src and
// op2 = dst as setf_a stores them.
static void ua_flags_addsub(struct cpu_state *cpu, bool sub, qword_t dst, qword_t src,
        qword_t cin, qword_t res, unsigned bits, bool set_cf) {
    qword_t m = ua_mask(bits), sign = 1ull << (bits - 1);
    dst &= m; src &= m;
    if (set_cf)
        cpu->cf = sub ? (src + cin > dst) : (dst + src + cin > m);
    cpu->of = sub ? (((dst ^ src) & (dst ^ res) & sign) != 0)
                  : (((dst ^ res) & (src ^ res) & sign) != 0);
    cpu->op1 = ua_sext(src, bits);
    cpu->op2 = ua_sext(dst, bits);
    cpu->res = ua_sext(res, bits);
    cpu->flags_res = ZF_RES | SF_RES | PF_RES | AF_OPS;
}

uint64_t helper_atomic_unaligned(struct cpu_state *cpu, struct tlb *tlb, dword_t addr,
        dword_t op_bits, dword_t operand) {
    struct ua_ctx c = { op_bits & 0xff, op_bits >> 8, operand, cpu->cf };
    unsigned bits = c.bits;
    qword_t old, neu;
    if (c.op == UA_CMPXCHG) {
        bool swapped;
        qword_t expected = cpu->eax & ua_mask(bits);
        if (x86_atomic_cas(cpu, tlb, addr, bits / 8, expected, operand & ua_mask(bits),
                    &old, &swapped) != 0)
            return 1ull << 32;
        // cmp eax, [mem]: eax - old, with op1 = old and op2 = eax.
        ua_flags_addsub(cpu, true, expected, old, 0, (expected - old) & ua_mask(bits), bits, true);
        if (!swapped)
            cpu->eax = (cpu->eax & ~(dword_t) ua_mask(bits)) | (dword_t) old;
        return operand;
    }
    if (c.op == UA_XCHG) {
        if (x86_atomic_xchg(cpu, tlb, addr, bits / 8, operand & ua_mask(bits), &old) != 0)
            return 1ull << 32;
        return (dword_t) old;
    }
    if (x86_atomic_rmw(cpu, tlb, addr, bits / 8, ua_apply, &c, &old, &neu) != 0)
        return 1ull << 32;
    switch (c.op) {
    case UA_ADD: case UA_SUB: case UA_ADC: case UA_SBB:
        ua_flags_addsub(cpu, c.op == UA_SUB || c.op == UA_SBB, old, operand,
                (c.op == UA_ADC || c.op == UA_SBB) ? c.cin : 0, neu, bits, true);
        return operand;
    case UA_XADD:
        // The gadget: exchange, then add -- op1 = old, op2 = the register.
        ua_flags_addsub(cpu, false, operand, old, 0, neu, bits, true);
        return (dword_t) old;
    case UA_INC: case UA_DEC:
        // CF is not touched; op1 = 1, op2 = the old value.
        ua_flags_addsub(cpu, c.op == UA_DEC, old, 1, 0, neu, bits, false);
        return operand;
    case UA_NEG:
        // 0 - old, with op1 = old and op2 = 0 as the gadget's setf_a has it.
        ua_flags_addsub(cpu, true, 0, old, 0, neu, bits, true);
        return operand;
    case UA_AND: case UA_OR: case UA_XOR:
        cpu->cf = cpu->of = 0;
        cpu->eflags &= ~AF_FLAG;
        cpu->res = ua_sext(neu, bits);
        cpu->flags_res = ZF_RES | SF_RES | PF_RES;
        return operand;
    case UA_BTS: case UA_BTR: case UA_BTC:
        cpu->cf = (old >> (operand & (bits - 1))) & 1;
        return operand;
    default: // UA_NOT: no flags
        return operand;
    }
}

// Unaligned i386 `lock cmpxchg8b [addr]`. The aarch64 fast path is an ARM
// exclusive load (ldaxr), which needs 8-byte alignment; x86 allows any, and the
// i386 ABI aligns 64-bit struct fields to only 4 bytes, so this is ordinary
// code (Python 3.14 does it at startup). x86_atomic_cas does it exactly -- see
// "Misaligned LOCK" in emu/tlb.c.
//
// The caller has spilled EAX/EBX/ECX/EDX to cpu->* and reloads EAX/EDX
// afterward; flags live in memory (cpu->eflags/flags_res), so ZF set here is
// what the next gadget sees. Only ZF is affected (CF/PF/AF/SF/OF are
// unchanged by CMPXCHG8B), matching the aligned gadget. Returns 0, or 1 on a
// fault with tlb->segfault_addr set for the gadget's segfault_write exit.
int helper_atomic_cmpxchg8b(struct cpu_state *cpu, struct tlb *tlb, dword_t addr) {
    qword_t expected = ((qword_t) cpu->edx << 32) | (dword_t) cpu->eax;
    qword_t desired  = ((qword_t) cpu->ecx << 32) | (dword_t) cpu->ebx;
    qword_t old;
    bool swapped;
    if (x86_atomic_cas(cpu, tlb, addr, 8, expected, desired, &old, &swapped) != 0)
        return 1;
    cpu->zf = swapped;
    cpu->zf_res = 0;
    if (!swapped) {
        cpu->eax = (dword_t) old;
        cpu->edx = (dword_t) (old >> 32);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Page-batched rep movs / rep stos fast path.
//
// The per-element string gadgets in jit/gadgets-aarch64/string.S re-run the
// full inline TLB hash (read_prep/write_prep) on EVERY element of a rep loop,
// so a `rep movsb` of N bytes pays ~2N translation sequences even though the
// host page base is invariant until the address crosses a 4 KB boundary.
//
// This helper resolves the host page once per page-run (via the same
// __tlb_read_ptr/__tlb_write_ptr the C TLB path uses, so it inherits all the
// miss / COW / cross-page / write-revalidate / staleness-flush handling) and
// bulk-copies the run with memmove/memset. It is invoked only for the forward
// (DF=0) direction with ecx >= a small threshold; the gadget keeps the existing
// per-element loop for backward, tiny, and data-dependent (cmps/scas) reps.
//
// Operates directly on cpu->{esi,edi,ecx,eax}; the gadget spills the
// host-register-cached copies before the call and reloads them after. On a
// fault it returns 1 (source/read) or 2 (dest/write) with cpu->{esi,edi,ecx}
// left pointing at the element about to be processed (x86 #PF restartability),
// and tlb->segfault_addr already set by the miss handler, so the gadget jumps
// to the existing segfault_read/segfault_write exit.
//
// It returns 3 when a poke arrived mid-rep, with that same restart state, and
// the gadget exits to jit_ret with eip set back to this instruction so the rep
// resumes after the interrupt has been handled.
//
// Overlapping forward movs (edi>esi within the run) is the x86 "smear" case
// that memmove would NOT reproduce, so it falls back to single-element copies
// through guest memory, which smears identically to the hardware.
// ---------------------------------------------------------------------------
int rep_string_fast(struct cpu_state *cpu, struct tlb *tlb, unsigned elem_size, int is_movs) {
    uint32_t ecx = cpu->ecx;
    uint32_t edi = cpu->edi;
    uint32_t esi = cpu->esi;
    uint32_t eax = cpu->eax;

    while (ecx != 0) {
        uint32_t run = (PAGE_SIZE - PGOFFSET(edi)) / elem_size; // whole elems left in dst page
        if (run > ecx)
            run = ecx;
        if (is_movs) {
            uint32_t src_room = (PAGE_SIZE - PGOFFSET(esi)) / elem_size;
            if (run > src_room)
                run = src_room;
        }

        // memmove only matches x86 ascending semantics when the run does not
        // forward-overlap (edi <= esi, or no overlap within the run).
        int overlap_smear = is_movs && edi > esi &&
            (uint64_t) (edi - esi) < (uint64_t) run * elem_size;

        if (run >= 1 && !overlap_smear) {
            char *dst = (char *) __tlb_write_ptr(tlb, edi);
            if (dst == NULL)
                return 2;
            if (is_movs) {
                char *src = (char *) __tlb_read_ptr(tlb, esi);
                if (src == NULL)
                    return 1;
                memmove(dst, src, (size_t) run * elem_size);
                esi += run * elem_size;
            } else if (elem_size == 1) {
                memset(dst, eax & 0xff, run);
            } else if (elem_size == 2) {
                uint16_t v = eax & 0xffff;
                for (uint32_t i = 0; i < run; i++)
                    ((uint16_t *) dst)[i] = v;
            } else {
                for (uint32_t i = 0; i < run; i++)
                    ((uint32_t *) dst)[i] = eax;
            }
            edi += run * elem_size;
            ecx -= run;
        } else {
            // One element via the cross-page-safe path: handles an element
            // straddling a page boundary (run == 0) and the overlap-smear case.
            if (is_movs) {
                char tmp[8];
                if (!tlb_read(tlb, esi, tmp, elem_size))
                    return 1;
                if (!tlb_write(tlb, edi, tmp, elem_size))
                    return 2;
                esi += elem_size;
            } else {
                if (!tlb_write(tlb, edi, &eax, elem_size))
                    return 2;
            }
            edi += elem_size;
            ecx -= 1;
        }

        cpu->ecx = ecx;
        cpu->edi = edi;
        cpu->esi = esi;

        // x86 makes REP interruptible BETWEEN iterations, and until now AOK
        // did not: this helper ran to ecx == 0 no matter how long that took,
        // and the JIT only tests the poke flag at block boundaries. A guest
        // memcpy compiled to `rep movsb` over a large buffer therefore held
        // the thread inside one gadget for the whole copy -- a signal could
        // not be delivered, and the swap pager's throttle poke (emu/tlb.c
        // sets mem_throttle_wanted and calls cpu_poke) could not reach the
        // thread it was aimed at, so reclaim waited on a copy that had
        // already faulted in everything it touched.
        //
        // Once per loop iteration, which is once per page-run on the bulk arm
        // above and once per ELEMENT on the single-element arm (an overlapping
        // forward movs, or an element straddling a page). That is the right
        // way round: the single-element arm is the slow one, so it is the one
        // that most needs to be interruptible, and it is rare enough that a
        // load per element does not matter.
        //
        // Only AFTER the registers above have been written back and progress
        // has been made. ecx, edi and esi are exactly the restart state x86
        // defines for #PF, so re-executing the instruction resumes where this
        // left off; returning before doing any work would livelock on a flag
        // nobody clears.
        //
        // Relaxed, and a load rather than an exchange. No ordering is needed
        // -- nothing here reads data published alongside the flag -- and the
        // authoritative read is cpu_take_poke's seq_cst exchange, which is
        // what actually consumes it. Clearing it here would swallow the poke
        // and yield for nothing. This matches jit_ret_chain, which reads the
        // same byte with a plain ldrb.
        if (ecx != 0 && __atomic_load_n(cpu->poked_ptr, __ATOMIC_RELAXED))
            return 3;
    }
    return 0;
}

// LOOP decrements ECX and, unlike DEC, leaves the flags alone. The branch
// itself reuses the jcxz gadget with its two ip slots swapped, since "jump if
// ECX != 0" is exactly the inverse of jcxz -- so this needs no new gadget on
// either host.
void helper_loop_dec_ecx(struct cpu_state *cpu) {
    cpu->ecx--;
}

// Packed/unpacked BCD adjust instructions. All of these read and write flags
// that the JIT normally keeps lazily (AF in particular), so each one collapses
// the lazy state first and then works with the materialized bits. Semantics
// follow the Intel SDM; flags the SDM leaves undefined are left alone.
// DAA/DAS set SF/ZF/PF from the result, so they also have to clear the lazy
// bits for those (collapse_flags does) and then write them directly.
static void bcd_set_result_flags(struct cpu_state *cpu, uint8_t al) {
    cpu->zf = al == 0;
    cpu->sf = (al & 0x80) != 0;
    cpu->pf = !__builtin_parity(al);
}

void helper_aaa(struct cpu_state *cpu) {
    collapse_flags(cpu);
    uint16_t ax = cpu->eax & 0xffff;
    if ((ax & 0x0f) > 9 || cpu->af) {
        // AX += 6 is a 16-bit add, so a carry out of AL lands in AH before the
        // separate AH increment -- doing this 8-bit on AL loses that carry.
        ax += 6;
        ax = (uint16_t) (ax + 0x100);
        cpu->af = cpu->cf = 1;
    } else {
        cpu->af = cpu->cf = 0;
    }
    ax &= 0xff0f;
    cpu->eax = (cpu->eax & 0xffff0000) | ax;
    // The SDM calls SF/ZF/PF undefined here, but real silicon sets them from
    // AL and software has been observed to rely on it, so match the hardware.
    bcd_set_result_flags(cpu, ax & 0xff);
}

void helper_aas(struct cpu_state *cpu) {
    collapse_flags(cpu);
    uint16_t ax = cpu->eax & 0xffff;
    if ((ax & 0x0f) > 9 || cpu->af) {
        ax -= 6;
        ax = (uint16_t) (ax - 0x100);
        cpu->af = cpu->cf = 1;
    } else {
        cpu->af = cpu->cf = 0;
    }
    ax &= 0xff0f;
    cpu->eax = (cpu->eax & 0xffff0000) | ax;
    bcd_set_result_flags(cpu, ax & 0xff);
}

void helper_daa(struct cpu_state *cpu) {
    collapse_flags(cpu);
    uint8_t al = cpu->eax & 0xff;
    uint8_t old_al = al;
    bool old_cf = cpu->cf;
    cpu->cf = 0;
    if ((al & 0x0f) > 9 || cpu->af) {
        cpu->cf = old_cf || (al > 0xff - 6);
        al += 6;
        cpu->af = 1;
    } else {
        cpu->af = 0;
    }
    if (old_al > 0x99 || old_cf) {
        al += 0x60;
        cpu->cf = 1;
    }
    cpu->eax = (cpu->eax & 0xffffff00) | al;
    bcd_set_result_flags(cpu, al);
}

void helper_das(struct cpu_state *cpu) {
    collapse_flags(cpu);
    uint8_t al = cpu->eax & 0xff;
    uint8_t old_al = al;
    bool old_cf = cpu->cf;
    cpu->cf = 0;
    if ((al & 0x0f) > 9 || cpu->af) {
        cpu->cf = old_cf || (al < 6);
        al -= 6;
        cpu->af = 1;
    } else {
        cpu->af = 0;
    }
    if (old_al > 0x99 || old_cf) {
        al -= 0x60;
        cpu->cf = 1;
    }
    cpu->eax = (cpu->eax & 0xffffff00) | al;
    bcd_set_result_flags(cpu, al);
}

// AAM's base-0 case is a divide error; gen.c catches that at translate time
// (the base is an immediate) and emits the interrupt instead of calling this.
void helper_aam(struct cpu_state *cpu, uint32_t base) {
    collapse_flags(cpu);
    uint8_t al = cpu->eax & 0xff;
    uint8_t ah = al / (uint8_t) base;
    al = al % (uint8_t) base;
    cpu->eax = (cpu->eax & 0xffff0000) | ((uint32_t) ah << 8) | al;
    bcd_set_result_flags(cpu, al);
}

void helper_aad(struct cpu_state *cpu, uint32_t base) {
    collapse_flags(cpu);
    uint8_t al = cpu->eax & 0xff;
    uint8_t ah = (cpu->eax >> 8) & 0xff;
    uint8_t prod = (uint8_t) (ah * (uint8_t) base);
    unsigned sum = al + prod;
    // The SDM calls CF/AF undefined, but hardware sets them from the implied
    // 8-bit ADD of AL and AH*base, and matching that is free.
    cpu->cf = sum > 0xff;
    cpu->af = ((al & 0xf) + (prod & 0xf)) > 0xf;
    al = (uint8_t) sum;
    cpu->eax = (cpu->eax & 0xffff0000) | al;
    bcd_set_result_flags(cpu, al);
}

// ---------------------------------------------------------------------------
// 66 0F 3A 63 pcmpistri, register form. Called from the JIT gadget rather than
// having it call vec_pcmpistri128 directly, because two things must happen
// afterwards and both are easy to miss in assembly:
//
//   - the index lands in cpu->ecx. vec.c's pcmp helpers are shared with the
//     i386 emulator and write the LEGACY register; for an amd64 guest it has
//     to be moved into amd64_regs[rcx], zero-extended to 64 bits.
//   - the flags it sets are the lazy kind, so they have to be collapsed before
//     anything reads them.
//
// emu/amd64_interp.c's bridge does exactly this pair after its own call. A
// gadget that skipped either would not fail loudly -- it would return a stale
// rcx or stale flags to glibc's strlen, which is the worst possible shape for
// a bug.
// ---------------------------------------------------------------------------
void amd64_jit_pcmpistri(struct cpu_state *cpu, const union xmm_reg *src,
        union xmm_reg *dst, uint8_t imm) {
    vec_pcmpistri128(cpu, src, dst, imm);
    cpu->amd64_regs[amd64_rcx] = (uint32_t) cpu->ecx;
    collapse_flags(cpu);
}

// 66 0F 3A 61 pcmpestri. The EXPLICIT-length form, and the one that actually
// dominates: measured over a gcc workload in the amd64 guest, op3=0x61 was
// 12956 of 20250 three-byte decodes (64%), against 6 for the register form of
// pcmpistri. It does not show up in a mnemonic grep of libc because glibc
// reaches it through strcmp/strncmp rather than by name.
//
// Unlike pcmpistri it reads the string lengths from EAX and EDX, and those are
// the LEGACY registers -- vec.c is shared with the i386 emulator -- so for an
// amd64 guest they have to be staged down from RAX/RDX first. The interpreter
// does exactly this before its own call; a gadget that skipped it would hand
// vec.c whatever the last i386-shaped write happened to leave behind.
void amd64_jit_pcmpestri(struct cpu_state *cpu, const union xmm_reg *src,
        union xmm_reg *dst, uint8_t imm) {
    cpu->eax = (dword_t) cpu->amd64_regs[amd64_rax];
    cpu->edx = (dword_t) cpu->amd64_regs[amd64_rdx];
    vec_pcmpestri128(cpu, src, dst, imm);
    cpu->amd64_regs[amd64_rcx] = (uint32_t) cpu->ecx;
    collapse_flags(cpu);
}
