#include "emu/cpu.h"
#include "emu/tlb.h"
#include "emu/interrupt.h"
#include "kernel/signal.h"
#include "kernel/task.h"

static void arm64_watch_scan_value(guest_addr_t addr, const void *value, unsigned size);

// AdvSIMD LD1/ST1 (load/store multiple single-element structures,
// contiguous form): transfer `count` consecutive V registers (wrapping
// mod 32) of `regbytes` (8 for the .8b/.4h/.2s arrangements, 16 for the
// .16b/... Q=1 arrangements) each, starting at `addr`. Done in C because
// the whole transfer can span page boundaries per register — reusing the
// crosspage-capable tlb_read/tlb_write is far simpler and safer than
// hand-rolling the multi-register crosspage assembly. Returns INT_NONE on
// success, or INT_PF (with cpu->segfault_addr/was_write set from the tlb)
// on the first faulting access; the calling gadget rewinds PC and exits.
// The scalar-write zero-extension (Q=0 clears the upper 64 bits) falls out
// of copying through a zero-initialized union.
//
// Success returns 0, NOT INT_NONE (which is -1) — the gadget branches to
// its fault path on a nonzero result, so INT_NONE would take the fault
// path on every success. (Real bug: it did exactly that — every
// successful ld1 exited INT_PF and the block re-ran forever.)
int arm64_vldst_multi(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
                      unsigned rt, unsigned count, unsigned regbytes, int is_load) {
    for (unsigned r = 0; r < count; r++) {
        unsigned v = (rt + r) & 31;
        if (is_load) {
            union xmm_reg tmp = {};
            if (!tlb_read(tlb, addr, &tmp, regbytes)) {
                cpu->segfault_addr = tlb->segfault_addr;
                cpu->segfault_was_write = false;
                return INT_PF;
            }
            cpu->arm64_v[v] = tmp;
        } else {
            if (!tlb_write(tlb, addr, &cpu->arm64_v[v], regbytes)) {
                cpu->segfault_addr = tlb->segfault_addr;
                cpu->segfault_was_write = true;
                return INT_PF;
            }
            arm64_watch_scan_value(addr, &cpu->arm64_v[v], regbytes);
        }
        addr += regbytes;
    }
    return 0;
}

// AdvSIMD structured load/store forms beyond the contiguous LD1/ST1
// above (same crosspage-safety reasoning; OpenMinis splits these across
// per-element micro-gadgets instead). spec packs the shape:
//   [3:0] count  [5:4] esize_log2  [6] q  [11:8] lane
//   [13:12] kind (1=interleaved multiple, 2=single lane, 3=replicate)
//   [14] is_load
// Returns 0 on success (see the INT_NONE note above), INT_PF on fault.
int arm64_vldst_struct(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
                       unsigned rt, unsigned spec) {
    unsigned count = spec & 0xf;
    unsigned esize = 1u << ((spec >> 4) & 3);
    unsigned q = (spec >> 6) & 1;
    unsigned lane = (spec >> 8) & 0xf;
    unsigned kind = (spec >> 12) & 3;
    int is_load = (spec >> 14) & 1;
    unsigned regbytes = q ? 16 : 8;

    if (kind == 1) {
        // LD2/LD3/LD4 (multiple structures): de-interleave count registers'
        // worth of elements; ST2-4 interleave. Whole transfer buffered so a
        // fault mid-way never leaves half-updated guest registers.
        unsigned lanes = regbytes / esize;
        unsigned total = count * regbytes;
        uint8_t buf[64];
        if (is_load) {
            if (!tlb_read(tlb, addr, buf, total)) {
                cpu->segfault_addr = tlb->segfault_addr;
                cpu->segfault_was_write = false;
                return INT_PF;
            }
            for (unsigned r = 0; r < count; r++) {
                union xmm_reg tmp = {};
                for (unsigned e = 0; e < lanes; e++)
                    memcpy(&tmp.u8[e * esize], &buf[(e * count + r) * esize], esize);
                cpu->arm64_v[(rt + r) & 31] = tmp;
            }
        } else {
            for (unsigned r = 0; r < count; r++)
                for (unsigned e = 0; e < lanes; e++)
                    memcpy(&buf[(e * count + r) * esize],
                           &cpu->arm64_v[(rt + r) & 31].u8[e * esize], esize);
            if (!tlb_write(tlb, addr, buf, total)) {
                cpu->segfault_addr = tlb->segfault_addr;
                cpu->segfault_was_write = true;
                return INT_PF;
            }
            arm64_watch_scan_value(addr, buf, total);
        }
        return 0;
    }

    if (kind == 2) {
        // LD1-4/ST1-4 (single structure): one element per register at a
        // fixed lane; loads leave the register's other lanes intact.
        for (unsigned r = 0; r < count; r++) {
            unsigned v = (rt + r) & 31;
            if (is_load) {
                uint8_t tmp[8];
                if (!tlb_read(tlb, addr, tmp, esize)) {
                    cpu->segfault_addr = tlb->segfault_addr;
                    cpu->segfault_was_write = false;
                    return INT_PF;
                }
                memcpy(&cpu->arm64_v[v].u8[lane * esize], tmp, esize);
            } else {
                if (!tlb_write(tlb, addr, &cpu->arm64_v[v].u8[lane * esize], esize)) {
                    cpu->segfault_addr = tlb->segfault_addr;
                    cpu->segfault_was_write = true;
                    return INT_PF;
                }
            }
            addr += esize;
        }
        return 0;
    }

    // kind == 3: LD1R-LD4R — load one element per register and replicate
    // it across the register's arrangement (upper 64 bits zero if Q=0).
    for (unsigned r = 0; r < count; r++) {
        unsigned v = (rt + r) & 31;
        uint8_t tmp[8];
        if (!tlb_read(tlb, addr, tmp, esize)) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            return INT_PF;
        }
        union xmm_reg rep = {};
        for (unsigned e = 0; e < regbytes / esize; e++)
            memcpy(&rep.u8[e * esize], tmp, esize);
        cpu->arm64_v[v] = rep;
        addr += esize;
    }
    return 0;
}

// LSE atomic read-modify-write (LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/
// LDUMAX/LDUMIN and SWP). GENUINELY host-atomic: it resolves the guest
// address to its backing host pointer and runs a real host __atomic RMW
// there, so concurrent guest threads (each on its own host pthread) don't
// lose updates. LSE requires natural alignment, so a valid access never
// crosses a page — one host pointer suffices. size_bytes is 1/2/4/8;
// op 0-7 are the arithmetic forms, op 8 is SWP. Returns 0 / INT_PF.
//
// The min/max variants and the sub-64-bit widths are done with a
// compare-exchange loop (there's no direct __atomic_fetch_max, and byte/
// half atomics still lower to LL/SC on the host anyway), which is itself
// lock-free and race-free.
// Misaligned LSE atomics take an alignment fault on real hardware (they
// are architecturally required to be naturally aligned). Enforcing that
// here is also a host-memory-safety requirement, not just conformance:
// the helpers below resolve ONE host page and then run a host atomic of
// up to 16 bytes at the resolved pointer, so a misaligned guest atomic
// straddling a page boundary would read/write host memory beyond the
// page that was actually resolved. Natural alignment guarantees the
// access can never cross a page. The kernel's interrupt plumbing has no
// SIGBUS/BUS_ADRALN path for guest faults (see handle_interrupt in
// kernel/calls.c: INT_* maps to SIGSEGV/SIGILL/SIGFPE only), so this
// reports INT_GPF -> SIGSEGV rather than Linux's SIGBUS; a misaligned
// atomic is a hard programming error and the fatal signal is what
// matters. cpu->segfault_addr carries the misaligned address.
static int arm64_atomic_alignment_fault(struct cpu_state *cpu, guest_addr_t addr) {
    cpu->segfault_addr = addr;
    cpu->segfault_was_write = true;
    return INT_GPF;
}

int arm64_lse_rmw(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
                  unsigned size_bytes, unsigned op, uint64_t operand,
                  uint64_t *old_out) {
    if (addr & (size_bytes - 1))
        return arm64_atomic_alignment_fault(cpu, addr);
    void *ptr = tlb_write_ptr_slow(tlb, addr);
    if (ptr == NULL) {
        cpu->segfault_addr = tlb->segfault_addr;
        cpu->segfault_was_write = true;
        return INT_PF;
    }
    unsigned bits = size_bytes * 8;
    uint64_t smask = bits < 64 ? (1ull << (bits - 1)) : 0;

#define LSE_RMW_AT(TYPE) do {                                             \
        TYPE *p = ptr;                                                    \
        TYPE arg = (TYPE) operand;                                        \
        TYPE old = __atomic_load_n(p, __ATOMIC_RELAXED), neu;            \
        do {                                                             \
            switch (op) {                                                \
                case 0: neu = (TYPE) (old + arg); break;   /* LDADD */   \
                case 1: neu = (TYPE) (old & ~arg); break;  /* LDCLR */   \
                case 2: neu = (TYPE) (old ^ arg); break;   /* LDEOR */   \
                case 3: neu = (TYPE) (old | arg); break;   /* LDSET */   \
                case 4: neu = (int64_t) ((old ^ smask) - smask) >         \
                              (int64_t) ((arg ^ smask) - smask)           \
                              ? old : arg; break;          /* LDSMAX */  \
                case 5: neu = (int64_t) ((old ^ smask) - smask) <         \
                              (int64_t) ((arg ^ smask) - smask)           \
                              ? old : arg; break;          /* LDSMIN */  \
                case 6: neu = old > arg ? old : arg; break; /* LDUMAX */ \
                case 7: neu = old < arg ? old : arg; break; /* LDUMIN */ \
                default: neu = arg; break;                 /* SWP */     \
            }                                                            \
        } while (!__atomic_compare_exchange_n(p, &old, neu, true,        \
                     __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));               \
        *old_out = (uint64_t) old;                                       \
    } while (0)

    switch (size_bytes) {
        case 1: LSE_RMW_AT(uint8_t); break;
        case 2: LSE_RMW_AT(uint16_t); break;
        case 4: LSE_RMW_AT(uint32_t); break;
        default: LSE_RMW_AT(uint64_t); break;
    }
#undef LSE_RMW_AT
    return 0;
}

// Host-atomic compare-and-swap for the LSE CAS gadget and the STXR
// store-conditional. Compares memory at size_bytes against `expected`; if
// equal, atomically stores `desired` and sets *swapped=1, else leaves it
// and sets *swapped=0. Always returns the observed old value (zero-
// extended) in *old_out. Genuinely atomic against concurrent guest
// threads — replaces the old load-compare-store, which had an ABA race
// (two threads could both pass the compare and both store). Returns
// 0 / INT_PF.
int arm64_cas(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
              unsigned size_bytes, uint64_t expected, uint64_t desired,
              uint64_t *old_out, uint32_t *swapped) {
    if (addr & (size_bytes - 1))
        return arm64_atomic_alignment_fault(cpu, addr);
    void *ptr = tlb_write_ptr_slow(tlb, addr);
    if (ptr == NULL) {
        cpu->segfault_addr = tlb->segfault_addr;
        cpu->segfault_was_write = true;
        return INT_PF;
    }
#define LSE_CAS_AT(TYPE) do {                                            \
        TYPE exp = (TYPE) expected;                                      \
        bool ok = __atomic_compare_exchange_n((TYPE *) ptr, &exp,        \
                     (TYPE) desired, false, __ATOMIC_SEQ_CST,            \
                     __ATOMIC_SEQ_CST);                                  \
        *swapped = ok ? 1 : 0;                                           \
        *old_out = (uint64_t) exp; /* CAS writes the observed value on fail */ \
    } while (0)
    switch (size_bytes) {
        case 1: LSE_CAS_AT(uint8_t); break;
        case 2: LSE_CAS_AT(uint16_t); break;
        case 4: LSE_CAS_AT(uint32_t); break;
        default: LSE_CAS_AT(uint64_t); break;
    }
#undef LSE_CAS_AT
    return 0;
}

// Host-atomic compare-and-swap PAIR (CASP/CASPA/CASPL/CASPAL) for the
// casp gadgets (jit/guest-arm64/atomics.S) and the interpreter. sz is the
// per-register width (4 for the W-pair form, 8 for the X-pair form); the
// register pair maps to guest memory little-endian as [addr] = lo (Rs/Rt)
// and [addr+sz] = hi (Rs+1/Rt+1), so the whole pair is one 2*sz-byte
// little-endian value. expected/desired/old_out are {lo, hi} arrays
// (arrays rather than four scalars keep the C ABI at 8 register args, so
// the gadget's marshalling stays register-only like arm64_cas's).
// old_out always receives the observed memory value, zero-extended per
// half for the 32-bit form. Returns 0 / INT_PF / INT_GPF.
//
// The 64-bit pair uses a 16-byte __atomic_compare_exchange on an
// unsigned __int128 — on an ARMv8.0 baseline host clang lowers that to an
// LDXP/STXP loop (or an outline-atomics libcall), never a v8.1+ CASP
// instruction, keeping the v8.0-only gadget invariant (verified by
// disassembly; see the hostcaps note in jit/gen.c's crypto probing).
int arm64_casp(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
               unsigned sz, const uint64_t expected[2], const uint64_t desired[2],
               uint64_t old_out[2], uint32_t *swapped) {
    // Alignment requirement is the TOTAL pair size (2*sz), per the ARM ARM
    // — which also guarantees the access never crosses a page, so the
    // single resolved host pointer below covers the whole pair.
    if (addr & (2 * sz - 1))
        return arm64_atomic_alignment_fault(cpu, addr);
    void *ptr = tlb_write_ptr_slow(tlb, addr);
    if (ptr == NULL) {
        cpu->segfault_addr = tlb->segfault_addr;
        cpu->segfault_was_write = true;
        return INT_PF;
    }
    if (sz == 4) {
        uint64_t exp = (uint32_t) expected[0] | ((uint64_t) (uint32_t) expected[1] << 32);
        uint64_t des = (uint32_t) desired[0] | ((uint64_t) (uint32_t) desired[1] << 32);
        bool ok = __atomic_compare_exchange_n((uint64_t *) ptr, &exp, des, false,
                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        *swapped = ok ? 1 : 0;
        old_out[0] = (uint32_t) exp;
        old_out[1] = (uint32_t) (exp >> 32);
    } else {
        unsigned __int128 exp = ((unsigned __int128) expected[1] << 64) | expected[0];
        unsigned __int128 des = ((unsigned __int128) desired[1] << 64) | desired[0];
        bool ok = __atomic_compare_exchange_n((unsigned __int128 *) ptr, &exp, des, false,
                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        *swapped = ok ? 1 : 0;
        old_out[0] = (uint64_t) exp;
        old_out[1] = (uint64_t) (exp >> 64);
    }
    return 0;
}

// LDXP (load exclusive pair) for the ldxp gadgets (jit/guest-arm64/
// atomics.S) and the interpreter. sz is the per-register width (4 for the
// W-pair form, 8 for the X-pair form); memory maps little-endian as
// [addr] = lo (Rt) and [addr+sz] = hi (Rt2), same layout as arm64_casp.
// Loads the pair, writes it to val_out (each half zero-extended for the
// 32-bit form), and arms the exclusive monitor (excl_addr/excl_val/
// excl_val_hi) with the address and observed pair. Returns 0 / INT_PF /
// INT_GPF (alignment is the TOTAL pair size, per the ARM ARM — which also
// guarantees the single resolved host pointer covers the whole pair).
//
// The 64-bit pair is read as two 8-byte atomic loads, NOT one 16-byte
// __atomic_load: clang lowers a 16-byte atomic load to an LDXP/STXP loop
// that STORES the value back, which host-faults on guest-read-only pages
// where a real LDXP is architecturally fine. A cross-half torn read under
// concurrent modification is caught downstream: STXP's 16-byte CAS
// compares against the armed (torn) pair, fails, and the guest's LL/SC
// loop retries — same value-based-monitor caveat as the STXR path.
int arm64_ldxp(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
               unsigned sz, uint64_t val_out[2]) {
    if (addr & (2 * sz - 1))
        return arm64_atomic_alignment_fault(cpu, addr);
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        cpu->segfault_addr = tlb->segfault_addr;
        cpu->segfault_was_write = false;
        return INT_PF;
    }
    if (sz == 4) {
        uint64_t v = __atomic_load_n((uint64_t *) ptr, __ATOMIC_SEQ_CST);
        val_out[0] = (uint32_t) v;
        val_out[1] = (uint32_t) (v >> 32);
    } else {
        val_out[0] = __atomic_load_n((uint64_t *) ptr, __ATOMIC_SEQ_CST);
        val_out[1] = __atomic_load_n((uint64_t *) ptr + 1, __ATOMIC_SEQ_CST);
    }
    cpu->arm64_excl_addr = addr;
    cpu->arm64_excl_val = val_out[0];
    cpu->arm64_excl_val_hi = val_out[1];
    return 0;
}

// STXP (store exclusive pair): fails fast (status 1, monitor cleared)
// if the monitor isn't armed for this guest address; otherwise attempts
// a host-atomic pair CAS(addr: {excl_val, excl_val_hi} -> desired) via
// arm64_casp, so the whole check-and-store is one atomic operation — a
// won CAS is STXP success (status 0), a lost CAS (another agent changed
// memory since the LDXP) is failure and the guest LL/SC loop retries.
// The monitor clears on every completed STXP, success or not
// (architected), but is PRESERVED on a fault return so the restarted
// instruction can succeed once the fault (e.g. COW break) is resolved —
// same contract as the stxr gadget's fault path. Returns 0 / INT_PF /
// INT_GPF.
int arm64_stxp(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
               unsigned sz, uint64_t desired_lo, uint64_t desired_hi,
               uint32_t *status_out) {
    if (cpu->arm64_excl_addr != addr) {
        cpu->arm64_excl_addr = UINT64_MAX;
        *status_out = 1;
        return 0;
    }
    uint64_t expected[2] = { cpu->arm64_excl_val, cpu->arm64_excl_val_hi };
    uint64_t desired[2] = { desired_lo, desired_hi };
    uint64_t old[2];
    uint32_t swapped;
    int err = arm64_casp(cpu, tlb, addr, sz, expected, desired, old, &swapped);
    if (err != 0)
        return err;
    cpu->arm64_excl_addr = UINT64_MAX;
    *status_out = swapped ? 0 : 1;
    return 0;
}

// ---------------------------------------------------------------------------
// x86 LOCK-prefixed read-modify-write, done with REAL host atomics.
//
// Every locked instruction an x86 guest executes has to be atomic against two
// different things, and until build 553 the amd64 path was wrong about both:
//
//   * other guest threads -- each of which runs on its own host pthread, so
//     "atomic" here means the host CPU must see one indivisible RMW; and
//   * the kernel's own read-modify-writes on guest memory (FUTEX_WAKE_OP is
//     the live example), which are plain C11 atomics on the host pointer.
//
// The amd64 interpreter used to serialise locked instructions on the global
// `atomic_l_lock` instead. That is slow (one global mutex for the whole
// emulator), it does not interlock with a host atomic at all, and several
// forms -- `lock add [mem], reg` and its OR/AND/XOR/SUB/ADC/SBB siblings --
// never took the lock in the first place, so they lost updates even against
// other guest threads (measured: 3876 of 200000 increments dropped with four
// threads on `lock addl %reg, (mem)`).
//
// The rule these helpers implement:
//
//   aligned  -> one host atomic on the resolved host pointer. A naturally
//               aligned access of 1/2/4/8/16 bytes cannot cross a page, so a
//               single resolved page always covers it.
//   unaligned-> fall back to atomic_l_lock around a read/compute/write pair.
//               x86 permits a misaligned LOCK (unlike arm64's LSE atomics, so
//               unlike arm64_lse_rmw above we cannot just fault), but such an
//               access can straddle two pages and no host atomic spans that.
//               It stays as weak as it was -- it does not interlock with a
//               host atomic -- but it is vanishingly rare in real code and it
//               is at least correct against other guest threads.
//
// `fn` computes the new value from the observed old one and MUST BE PURE: on
// the aligned path it is re-run for every compare-exchange retry. Flags are
// the caller's job, computed once from the *old_out / *new_out this returns.
//
// Returns 0, or INT_PF with cpu->segfault_addr/was_write set.

typedef qword_t (*x86_atomic_fn)(qword_t old, void *ctx);

static int x86_atomic_fault(struct cpu_state *cpu, struct tlb *tlb) {
    cpu->segfault_addr = tlb->segfault_addr;
    cpu->segfault_was_write = true;
    return INT_PF;
}

// The unaligned path, shared by every helper below: one global-lock-guarded
// read/compute/write. Split out so the fast paths stay readable.
static int x86_atomic_rmw_locked(struct cpu_state *cpu, struct tlb *tlb,
        guest_addr_t addr, unsigned size_bytes, x86_atomic_fn fn, void *ctx,
        qword_t *old_out, qword_t *new_out) {
    uint64_t old = 0, neu;
    int err = 0;
    lock(&atomic_l_lock, 0);
    if (!tlb_read(tlb, addr, &old, size_bytes)) {
        cpu->segfault_addr = tlb->segfault_addr;
        cpu->segfault_was_write = false;
        err = INT_PF;
        goto out;
    }
    neu = fn(old, ctx);
    if (!tlb_write(tlb, addr, &neu, size_bytes)) {
        err = x86_atomic_fault(cpu, tlb);
        goto out;
    }
    *old_out = old;
    *new_out = neu;
out:
    unlock(&atomic_l_lock);
    return err;
}

int x86_atomic_rmw(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
                   unsigned size_bytes, x86_atomic_fn fn, void *ctx,
                   qword_t *old_out, qword_t *new_out) {
    if (addr & (size_bytes - 1))
        return x86_atomic_rmw_locked(cpu, tlb, addr, size_bytes, fn, ctx,
                                     old_out, new_out);
    void *ptr = tlb_write_ptr_slow(tlb, addr);
    if (ptr == NULL)
        return x86_atomic_fault(cpu, tlb);

#define X86_RMW_AT(TYPE) do {                                            \
        TYPE *p = ptr;                                                   \
        TYPE old = __atomic_load_n(p, __ATOMIC_RELAXED), neu;            \
        do {                                                             \
            neu = (TYPE) fn((qword_t) old, ctx);                         \
        } while (!__atomic_compare_exchange_n(p, &old, neu, true,        \
                     __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));               \
        *old_out = (qword_t) old;                                        \
        *new_out = (qword_t) neu;                                        \
    } while (0)
    switch (size_bytes) {
        case 1: X86_RMW_AT(uint8_t); break;
        case 2: X86_RMW_AT(uint16_t); break;
        case 4: X86_RMW_AT(uint32_t); break;
        default: X86_RMW_AT(uint64_t); break;
    }
#undef X86_RMW_AT
    return 0;
}

// LOCK CMPXCHG / CMPXCHG8B. Compares [addr] against `expected`; on equal
// stores `desired`. *old_out always receives the observed value -- which is
// what CMPXCHG loads into the accumulator when the compare fails, and what
// its flags are computed from either way.
int x86_atomic_cas(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
                   unsigned size_bytes, qword_t expected, qword_t desired,
                   qword_t *old_out, bool *swapped) {
    if (addr & (size_bytes - 1)) {
        // No pure-function shape here: whether we store depends on the value
        // we read, so the locked fallback is written out rather than reusing
        // x86_atomic_rmw_locked.
        uint64_t old = 0;
        int err = 0;
        lock(&atomic_l_lock, 0);
        if (!tlb_read(tlb, addr, &old, size_bytes)) {
            cpu->segfault_addr = tlb->segfault_addr;
            cpu->segfault_was_write = false;
            err = INT_PF;
            goto out;
        }
        *old_out = old;
        *swapped = old == expected;
        if (*swapped && !tlb_write(tlb, addr, &desired, size_bytes))
            err = x86_atomic_fault(cpu, tlb);
out:
        unlock(&atomic_l_lock);
        return err;
    }
    void *ptr = tlb_write_ptr_slow(tlb, addr);
    if (ptr == NULL)
        return x86_atomic_fault(cpu, tlb);
#define X86_CAS_AT(TYPE) do {                                            \
        TYPE exp = (TYPE) expected;                                      \
        bool ok = __atomic_compare_exchange_n((TYPE *) ptr, &exp,        \
                     (TYPE) desired, false, __ATOMIC_SEQ_CST,            \
                     __ATOMIC_SEQ_CST);                                  \
        *swapped = ok;                                                   \
        *old_out = (qword_t) exp; /* CAS writes the observed value back */ \
    } while (0)
    switch (size_bytes) {
        case 1: X86_CAS_AT(uint8_t); break;
        case 2: X86_CAS_AT(uint16_t); break;
        case 4: X86_CAS_AT(uint32_t); break;
        default: X86_CAS_AT(uint64_t); break;
    }
#undef X86_CAS_AT
    return 0;
}

// LOCK CMPXCHG16B. The instruction already requires 16-byte alignment (the
// caller raises #GP otherwise), so there is no unaligned path -- and that
// alignment is also what guarantees the single resolved page covers all 16
// bytes. expected/desired/old_out are {low qword, high qword}.
int x86_atomic_cas16b(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
                      const qword_t expected[2], const qword_t desired[2],
                      qword_t old_out[2], bool *swapped) {
    void *ptr = tlb_write_ptr_slow(tlb, addr);
    if (ptr == NULL)
        return x86_atomic_fault(cpu, tlb);
    unsigned __int128 exp = ((unsigned __int128) expected[1] << 64) | expected[0];
    unsigned __int128 des = ((unsigned __int128) desired[1] << 64) | desired[0];
    bool ok = __atomic_compare_exchange_n((unsigned __int128 *) ptr, &exp, des,
                 false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    *swapped = ok;
    old_out[0] = (qword_t) exp;
    old_out[1] = (qword_t) (exp >> 64);
    return 0;
}

// XCHG with a memory operand -- implicitly locked on x86, prefix or not.
// A direct host exchange rather than a compare-exchange loop, because this
// one is hot: it is the store half of every spinlock acquire.
static qword_t x86_atomic_swap_fn(qword_t old, void *ctx) {
    (void) old;
    return *(qword_t *) ctx;
}
int x86_atomic_xchg(struct cpu_state *cpu, struct tlb *tlb, guest_addr_t addr,
                    unsigned size_bytes, qword_t value, qword_t *old_out) {
    if (addr & (size_bytes - 1)) {
        qword_t neu;
        return x86_atomic_rmw_locked(cpu, tlb, addr, size_bytes,
                x86_atomic_swap_fn, &value, old_out, &neu);
    }
    void *ptr = tlb_write_ptr_slow(tlb, addr);
    if (ptr == NULL)
        return x86_atomic_fault(cpu, tlb);
    switch (size_bytes) {
        case 1: *old_out = __atomic_exchange_n((uint8_t *) ptr, (uint8_t) value, __ATOMIC_SEQ_CST); break;
        case 2: *old_out = __atomic_exchange_n((uint16_t *) ptr, (uint16_t) value, __ATOMIC_SEQ_CST); break;
        case 4: *old_out = __atomic_exchange_n((uint32_t *) ptr, (uint32_t) value, __ATOMIC_SEQ_CST); break;
        default: *old_out = __atomic_exchange_n((uint64_t *) ptr, (uint64_t) value, __ATOMIC_SEQ_CST); break;
    }
    return 0;
}

// Soft fallbacks for arm64-guest gadgets whose native instructions are
// optional host extensions: FEAT_SHA512 arrived with A13 and FEAT_CRC32
// with A10, but we support devices back to A7-class hardware. gen.c
// probes the host and emits the sha512_soft/crc32_soft gadgets (crypto.S,
// dpextra.S) on older chips, so AT_HWCAP and ID_AA64ISAR0 can advertise
// the same feature set on every device. Formulas verified bit-exact
// against the native instructions on an M-series host (random vectors).

static inline uint64_t ror64(uint64_t x, unsigned n) {
    return x >> n | x << (64 - n);
}
static inline uint64_t sha512_cho(uint64_t x, uint64_t y, uint64_t z) {
    return (x & (y ^ z)) ^ z;
}
static inline uint64_t sha512_maj(uint64_t x, uint64_t y, uint64_t z) {
    return (x & y) | ((x | y) & z);
}

// packed = rd | rn<<8 | rm<<16 | op<<24; op: 0=H 1=H2 2=SU1 3=SU0.
void arm64_sha512_soft(struct cpu_state *cpu, uint64_t packed) {
    uint64_t *d = cpu->arm64_v[packed & 0x1f].qw;
    // Snapshot operands: rd may alias rn/rm, and the instruction reads
    // everything before writing.
    uint64_t d0 = d[0], d1 = d[1];
    uint64_t n0 = cpu->arm64_v[(packed >> 8) & 0x1f].qw[0];
    uint64_t n1 = cpu->arm64_v[(packed >> 8) & 0x1f].qw[1];
    uint64_t m0 = cpu->arm64_v[(packed >> 16) & 0x1f].qw[0];
    uint64_t m1 = cpu->arm64_v[(packed >> 16) & 0x1f].qw[1];
    switch ((packed >> 24) & 3) {
        case 0: { // SHA512H
            d1 += (ror64(m1, 14) ^ ror64(m1, 18) ^ ror64(m1, 41))
                + sha512_cho(m1, n0, n1);
            uint64_t t = d1 + m0;
            d0 += (ror64(t, 14) ^ ror64(t, 18) ^ ror64(t, 41))
                + sha512_cho(t, m1, n0);
            break;
        }
        case 1: // SHA512H2
            d1 += (ror64(m0, 28) ^ ror64(m0, 34) ^ ror64(m0, 39))
                + sha512_maj(m0, m1, n0);
            d0 += (ror64(d1, 28) ^ ror64(d1, 34) ^ ror64(d1, 39))
                + sha512_maj(d1, m0, m1);
            break;
        case 2: // SHA512SU1
            d0 += (ror64(n0, 19) ^ ror64(n0, 61) ^ (n0 >> 6)) + m0;
            d1 += (ror64(n1, 19) ^ ror64(n1, 61) ^ (n1 >> 6)) + m1;
            break;
        case 3: // SHA512SU0 (two-reg; rm unused)
            d0 += ror64(d1, 1) ^ ror64(d1, 8) ^ (d1 >> 7);
            d1 += ror64(n0, 1) ^ ror64(n0, 8) ^ (n0 >> 7);
            break;
    }
    d[0] = d0;
    d[1] = d1;
}

uint32_t arm64_crc32_soft(uint32_t acc, uint64_t val, uint64_t size_log, uint64_t is_c) {
    uint32_t poly = is_c ? 0x82F63B78u : 0xEDB88320u; // reflected CRC32C / CRC32
    for (unsigned i = 0; i < (1u << size_log); i++) {
        acc ^= (uint8_t) (val >> (8 * i));
        for (int bit = 0; bit < 8; bit++)
            acc = (acc >> 1) ^ (poly & -(acc & 1));
    }
    return acc;
}

void tlb_refresh(struct tlb *tlb, struct mmu *mmu) {
    if (tlb->mmu == mmu &&
            tlb->mem_changes == atomic_load_explicit(&mmu->changes, memory_order_relaxed)) {
        return;
    }
    tlb->mmu = mmu;
    tlb->dirty_page = TLB_PAGE_EMPTY;
    tlb->mem_changes = atomic_load_explicit(&mmu->changes, memory_order_relaxed);
    tlb_flush(tlb);
}

void tlb_flush(struct tlb *tlb) {
    tlb->mem_changes = atomic_load_explicit(&tlb->mmu->changes, memory_order_relaxed);
    for (unsigned i = 0; i < TLB_SIZE; i++)
        tlb->entries[i] = (struct tlb_entry) {.page = 1, .page_if_writable = 1};
}

void tlb_free(struct tlb *tlb) {
    free(tlb);
}

bool __tlb_read_cross_page(struct tlb *tlb, guest_addr_t addr, char *value, unsigned size) {
    char *ptr1 = __tlb_read_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return false;
    }
    char *ptr2 = __tlb_read_ptr(tlb, (PAGE(addr) + 1) << PAGE_BITS);
    if (ptr2 == NULL) {
        return false;
    }
    size_t part1 = PAGE_SIZE - PGOFFSET(addr);
    assert(part1 < size);
    memcpy(value, ptr1, part1);
    memcpy(value + part1, ptr2, size - part1);
    return true;
}

bool __tlb_write_cross_page(struct tlb *tlb, guest_addr_t addr, const char *value, unsigned size) {
    char *ptr1 = __tlb_write_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return false;
    }
    char *ptr2 = __tlb_write_ptr(tlb, (PAGE(addr) + 1) << PAGE_BITS);
    if (ptr2 == NULL) {
        return false;
    }
    size_t part1 = PAGE_SIZE - PGOFFSET(addr);
    assert(part1 < size);
    memcpy(ptr1, value, part1);
    memcpy(ptr2, value + part1, size - part1);
    arm64_watch_scan_value(addr, value, size);
    return true;
}

__no_instrument void *tlb_handle_miss(struct tlb *tlb, guest_addr_t addr, int type) {
    char *ptr = mmu_translate(tlb->mmu, TLB_PAGE(addr), type);
    if (atomic_load_explicit(&tlb->mmu->changes, memory_order_relaxed) != tlb->mem_changes)
        tlb_flush(tlb);
    if (ptr == NULL) {
        tlb->segfault_addr = addr;
        return NULL;
    }
    tlb->dirty_page = TLB_PAGE(addr);

    struct tlb_entry *tlb_ent = &tlb->entries[TLB_INDEX(addr)];
    tlb_ent->page = TLB_PAGE(addr);
    if (type == MEM_WRITE)
        tlb_ent->page_if_writable = tlb_ent->page;
    else
        // 1 is not a valid page so this won't look like a hit
        tlb_ent->page_if_writable = TLB_PAGE_EMPTY;
    tlb_ent->data_minus_addr = (uintptr_t) ptr - TLB_PAGE(addr);
    return (void *) (tlb_ent->data_minus_addr + addr);
}

// ---- ISH_ARM64_WATCH_LO16 store watchpoint ------------------------------
// The V8-heap layout inside a page is stable across runs even though the
// allocation base moves, so the watch key is the low 16 bits of the target
// address. Every recorded entry keeps the storing instruction's guest pc
// (stashed in tlb->watch_ip by arm64_resolve_write_ptr), the full target
// address, and the 8 bytes about to be overwritten.

struct arm64_watch_rec {
    uint64_t ip;
    uint64_t addr;
    uint64_t oldval;
    uint32_t thread;
};
#define ARM64_WATCH_RING (1 << 20)
static struct arm64_watch_rec *arm64_watch_ring;
static _Atomic uint32_t arm64_watch_idx;
static uint32_t arm64_watch_lo16;
static bool arm64_watch_lo16_on;
static uint64_t arm64_watch_val;
static bool arm64_watch_val_on;
static int arm64_watch_state; // 0 = unchecked, 1 = off, 2 = on

bool arm64_watch_enabled(void) {
    if (arm64_watch_state == 0) {
        const char *spec = getenv("ISH_ARM64_WATCH_LO16");
        if (spec != NULL && spec[0] != '\0') {
            arm64_watch_lo16 = (uint32_t) strtoul(spec, NULL, 16) & 0xffff;
            arm64_watch_lo16_on = true;
        }
        const char *vspec = getenv("ISH_ARM64_WATCH_VAL");
        if (vspec != NULL && vspec[0] != '\0') {
            arm64_watch_val = strtoull(vspec, NULL, 16);
            arm64_watch_val_on = true;
        }
        if (arm64_watch_lo16_on || arm64_watch_val_on) {
            arm64_watch_ring = calloc(ARM64_WATCH_RING, sizeof(*arm64_watch_ring));
            arm64_watch_state = arm64_watch_ring != NULL ? 2 : 1;
        } else {
            arm64_watch_state = 1;
        }
    }
    return arm64_watch_state == 2;
}

static __no_instrument void arm64_watch_push(uint64_t ip, uint64_t addr, uint64_t val) {
    uint32_t i = atomic_fetch_add_explicit(&arm64_watch_idx, 1, memory_order_relaxed)
        & (ARM64_WATCH_RING - 1);
    struct arm64_watch_rec *rec = &arm64_watch_ring[i];
    rec->ip = ip;
    rec->addr = addr;
    rec->oldval = val;
    rec->thread = (uint32_t) (uintptr_t) pthread_self();
}

static __no_instrument void arm64_watch_record(struct tlb *tlb, guest_addr_t addr, void *ptr) {
    if (arm64_watch_val_on) {
        // Read back the previous store resolved through this (per-thread)
        // tlb: if it deposited the poison value, record it with its ip.
        // Skip when a mapping change happened in between (the old host
        // page may be gone) or when reading 16 bytes would leave the
        // guest page the store was on.
        if (tlb->prev_write_ptr != NULL &&
                tlb->prev_write_changes == tlb->mem_changes &&
                PGOFFSET(tlb->prev_write_addr) <= PAGE_SIZE - 16) {
            uint64_t half[2];
            memcpy(half, tlb->prev_write_ptr, sizeof(half));
            if (half[0] == arm64_watch_val)
                arm64_watch_push(tlb->prev_write_ip, tlb->prev_write_addr, half[0]);
            else if (half[1] == arm64_watch_val)
                arm64_watch_push(tlb->prev_write_ip, tlb->prev_write_addr + 8, half[1]);
        }
        tlb->prev_write_ptr = (char *) ptr - (addr & 7);
        tlb->prev_write_addr = addr & ~(guest_addr_t) 7;
        tlb->prev_write_ip = tlb->watch_ip;
        tlb->prev_write_changes = tlb->mem_changes;
    }
    if (arm64_watch_lo16_on) {
        // window: any store starting up to 15 bytes before the watched
        // 8-byte slot through its end can touch it (largest guest store
        // is 16 bytes)
        uint32_t lo = addr & 0xffff;
        if (lo + 16 > arm64_watch_lo16 && lo < arm64_watch_lo16 + 8) {
            uint64_t oldval;
            memcpy(&oldval, ptr, sizeof(oldval));
            arm64_watch_push(tlb->watch_ip, addr, oldval);
        }
    }
}

// Direct value check for C-side bulk stores that see the data (the ld1/st1
// helper and the crosspage flush): record any poison value in the buffer.
static __no_instrument void arm64_watch_scan_value(guest_addr_t addr, const void *value, unsigned size) {
    if (arm64_watch_state != 2 || !arm64_watch_val_on)
        return;
    for (unsigned off = 0; off + 8 <= size; off += 4) {
        uint64_t v;
        memcpy(&v, (const char *) value + off, sizeof(v));
        if (v == arm64_watch_val)
            arm64_watch_push((uint64_t) -1, addr + off, v);
    }
}

void arm64_watch_dump(void) {
    if (arm64_watch_state != 2)
        return;
    uint32_t end = atomic_load_explicit(&arm64_watch_idx, memory_order_relaxed);
    uint32_t count = end < ARM64_WATCH_RING ? end : ARM64_WATCH_RING;
    uint32_t shown = count < 65536 ? count : 65536;
    printk("arm64 watch ring: %u total records, dumping last %u\n", end, shown);
    for (uint32_t n = shown; n > 0; n--) {
        struct arm64_watch_rec *rec = &arm64_watch_ring[(end - n) & (ARM64_WATCH_RING - 1)];
        printk("  watch[-%u] ip=%#llx addr=%#llx old=%#llx thread=%#x\n",
               n, (unsigned long long) rec->ip, (unsigned long long) rec->addr,
               (unsigned long long) rec->oldval, rec->thread);
    }
}

// ISH_ARM64_TRACE_IP=<hex guest pc> + ISH_ARM64_TRACE_LDR="rn,rm": before
// the instruction at the traced pc executes, recompute its register-offset
// load address from the committed guest state and, when the value it is
// about to load matches ISH_ARM64_WATCH_VAL, dump the full context. Used to
// decide whether a poison value genuinely exists at the load's source or
// the executing gadget diverges from the architectural computation.
static uint64_t arm64_trace_rn = 5, arm64_trace_rm = 1;

uint64_t arm64_trace_ip_target(void) {
    static uint64_t target = (uint64_t) -1;
    if (target == (uint64_t) -1) {
        const char *spec = getenv("ISH_ARM64_TRACE_IP");
        target = (spec != NULL && spec[0] != '\0') ? strtoull(spec, NULL, 16) : 0;
        const char *regs = getenv("ISH_ARM64_TRACE_LDR");
        if (regs != NULL)
            sscanf(regs, "%llu,%llu",
                   (unsigned long long *) &arm64_trace_rn,
                   (unsigned long long *) &arm64_trace_rm);
    }
    return target;
}

void dump_mem(guest_addr_t start, uint_t len);

void arm64_trace_probe(struct cpu_state *cpu, struct tlb *tlb, uint64_t ip) {
    static _Atomic int hits;
    // ISH_ARM64_TRACE_LOOKUP=1 (paired with ISH_ARM64_TRACE_IP=<hex guest
    // pc>, see arm64_trace_ip_target above): call-site tracer for a specific
    // function whose calling convention is "(void *table, const char
    // *name, ...)" -- x1 is read as a NUL-terminated guest C string, x2/x3
    // and the return address (x30/LR) are logged alongside it. Written to
    // watch bfd_hash_lookup()/bfd_link_hash_lookup() calls (symbol name in
    // x1, create/copy flags in x2/x3) while diagnosing a GNU ld hang traced
    // to heap corruption in bfd's symbol hash table -- see
    // project_brk_reserve_shrink_regrow_corruption in the project's Claude
    // memory for the actual bug and fix (kernel/mmap.c, brk_reserve_start
    // shrink handling). General-purpose beyond that one investigation: any
    // "log every call to guest function at address X, with a C-string
    // second argument" question reuses this by pointing ISH_ARM64_TRACE_IP
    // at the new function's entry PC. Caps at 20000 logged calls so a hot
    // function traced by mistake doesn't flood the log.
    static int lookup_mode = -1;
    if (lookup_mode == -1)
        lookup_mode = getenv("ISH_ARM64_TRACE_LOOKUP") != NULL;
    if (lookup_mode) {
        static _Atomic int lhits;
        int n = atomic_fetch_add(&lhits, 1);
        if (n < 20000) {
            char namebuf[128];
            namebuf[0] = '\0';
            uint64_t strp = cpu->arm64_regs[1];
            for (int i = 0; i < 127; i++) {
                char c = 0;
                if (!tlb_read(tlb, strp + i, &c, 1))
                    break;
                namebuf[i] = c;
                namebuf[i + 1] = '\0';
                if (c == 0)
                    break;
            }
            printk("lookup#%d name=\"%s\" create=%llu copy=%llu caller=%llx\n", n,
                   namebuf,
                   (unsigned long long) cpu->arm64_regs[2],
                   (unsigned long long) cpu->arm64_regs[3],
                   (unsigned long long) cpu->arm64_regs[30]);
        }
        return;
    }
    uint64_t base = cpu->arm64_regs[arm64_trace_rn];
    uint64_t idx = cpu->arm64_regs[arm64_trace_rm];
    uint64_t addr = base + idx;
    uint64_t val = 0;
    if (!tlb_read(tlb, addr, &val, sizeof(val)))
        return;
    if (val != arm64_watch_val)
        return;
    if (atomic_fetch_add(&hits, 1) >= 100)
        return;
    printk("traceprobe ip=%#llx rn(x%llu)=%#llx rm(x%llu)=%#llx addr=%#llx val=%#llx\n",
           (unsigned long long) ip,
           (unsigned long long) arm64_trace_rn, (unsigned long long) base,
           (unsigned long long) arm64_trace_rm, (unsigned long long) idx,
           (unsigned long long) addr, (unsigned long long) val);
    for (int i = 0; i < 31; i += 4)
        printk("  x%d=%#llx x%d=%#llx x%d=%#llx x%d=%#llx\n",
               i, (unsigned long long) cpu->arm64_regs[i],
               i + 1, i + 1 < 31 ? (unsigned long long) cpu->arm64_regs[i + 1] : 0,
               i + 2, i + 2 < 31 ? (unsigned long long) cpu->arm64_regs[i + 2] : 0,
               i + 3, i + 3 < 31 ? (unsigned long long) cpu->arm64_regs[i + 3] : 0);
    printk("traceprobe mem around load addr %#llx:\n", (unsigned long long) addr);
    dump_mem((addr & ~7ULL) - 0x60, 0xc0);
    printk("traceprobe mem around x22=%#llx:\n", (unsigned long long) cpu->arm64_regs[22]);
    dump_mem((cpu->arm64_regs[22] & ~7ULL) - 0x20, 0x60);
    printk("traceprobe mem around dest x3=%#llx:\n", (unsigned long long) cpu->arm64_regs[3]);
    dump_mem((cpu->arm64_regs[3] & ~7ULL) - 0x40, 0x80);
}

__no_instrument void *tlb_write_ptr_slow(struct tlb *tlb, guest_addr_t addr) {
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (unlikely(arm64_watch_state == 2) && ptr != NULL)
        arm64_watch_record(tlb, addr, ptr);
    return ptr;
}
