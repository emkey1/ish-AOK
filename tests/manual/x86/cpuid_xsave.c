// cpuid_xsave -- every CPUID feature bit AOK advertises must name an
// instruction the guest can actually execute.
//
// emu/cpuid.h has said for a while that "nothing is advertised that a guest
// cannot actually execute", and cited this file as the thing that enforced it.
// This file did not exist: no test in the tree executed CPUID at all, so the
// claim rested on nobody having made a mistake yet. That is a live risk rather
// than a theoretical one, because emu/cpuid.h's CPUID_ADVERTISE_VECTOR_STATE
// is a switch someone is expected to flip, and flipping it turns on AVX,
// AVX-512, AESNI, PCLMULQDQ and XSAVE in one edit.
//
// Over-advertising is the failure that matters, and it is not a graceful one.
// Feature-detecting software does not probe; it reads CPUID and commits.
// Compiler and stdlib function multiversioning picks a baseline/AVX2/AVX-512
// variant of a hot routine at startup and never revisits the choice, so a bit
// we advertise without an implementation behind it is not a slow path, it is a
// SIGILL somewhere deep inside memcpy. That is exactly how the AVX gap
// originally surfaced -- see x86/avx_regress.c's header on Bun -- and the
// diagnosis cost was entirely in the distance between the crash and the lie.
//
// So: read the bits, and for each one that is SET, execute a representative
// instruction under a SIGILL guard. A bit that is clear is skipped, not
// failed -- under-advertising a working instruction is conservative and legal,
// and several bits here are deliberately dark today. The table lists them
// anyway so that turning one on lights up its probe automatically instead of
// requiring someone to remember this file exists.
//
// The XSAVE half is a self-consistency check on leaf 0x0D rather than an
// instruction probe. AOK packs the extended state components itself (MPX is
// absent, so everything above AVX is packed up against it), which means the
// offsets and sizes in leaf 0x0D are hand-maintained numbers that software is
// entitled to believe. They are only checked when XSAVE is actually
// advertised: leaf 0x0D answers unconditionally today, describing state that
// the dark leaf-1 bits mean no guest will go looking for.
//
// Representative instructions are register-only wherever the encoding allows,
// so a probe that faults faults for the reason under test and not because it
// touched memory. Where a memory operand is unavoidable (FXSAVE, XSAVE,
// CMPXCHG8B/16B) the address goes in via a "D" constraint and the opcode via
// .byte, which sidesteps the (%edi) vs (%rdi) spelling difference and keeps
// one probe valid for both guest ABIs -- the same trick x86/atomic_cmpxchg8b.c
// uses.
//
// Inline asm goes straight to the assembler, so no __attribute__((target))
// annotations are needed to emit AVX-512 here; the guest toolchain's binutils
// is the only thing that has to know the mnemonics, and x86/avx_regress.c
// already requires that much of it.
//
// x86 only. Builds and runs on both the i386 and amd64 guests, which is the
// point for the leaf 7 bits: those are advertised from one ABI-independent
// function, while the i386 front end reaches vector code only through
// gen_vex32 (VEX, 128/256-bit, 8 registers) and has no EVEX decoder at all.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include "../test_common.h"

#if !defined(__i386__) && !defined(__x86_64__)
int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    printf("cpuid_xsave: SKIP (x86 only)\n");
    return 0;
}
#else

#include <cpuid.h>

// ---- fault guard -----------------------------------------------------------

static sigjmp_buf fault_env;
static volatile int fault_signo;

static void fault_handler(int sig, siginfo_t *info, void *uc) {
    (void) info;
    (void) uc;
    fault_signo = sig;
    siglongjmp(fault_env, 1);
}

// ---- representative instructions -------------------------------------------
//
// Each probe executes exactly one feature-defining instruction and returns 0.
//
// A probe returns 1 instead when the instruction executed without faulting but
// demonstrably did nothing. That case is not paranoia: emu/decode.h decodes the
// whole 0f ae group as "fence" for the i386 guest -- it reads the modrm byte
// and falls through -- so FXSAVE, FXRSTOR, LDMXCSR, STMXCSR, XSAVE and XRSTOR
// are all silently skipped there rather than raising SIGILL. An advertised
// instruction that quietly does nothing is strictly worse than one that
// faults: the guest gets stale state and no diagnostic, where a fault at least
// names the instruction. Only probes with a cheap, unambiguous observable
// effect check for this -- poisoning a buffer and requiring the write.
//
// None of these name a vector register in the clobber list, and none carries
// an __attribute__((target)). Both are deliberate. GCC only knows the names
// xmm0/ymm0/k1 when the corresponding ISA is enabled for the translation
// unit, so naming them would force a target attribute onto every probe -- and
// then the file would stop building the moment a guest toolchain predates one
// of the feature names (this is not hypothetical: an i386 Alpine gcc 9.3
// rejects every one of them without it). The assembler needs no such help,
// because inline asm text goes straight through to it.
//
// Dropping the names is safe rather than merely convenient. Every probe is
// reached through the function pointer in the table below, so it is a real
// call that GCC cannot inline into a caller holding live vector state, and
// xmm/ymm/zmm/k are all caller-saved in both the i386 and x86-64 SysV ABIs.
// The "memory" clobber keeps the asm from being reordered against the fault
// guard around it.

static int p_fpu(void) {
    __asm__ volatile("fninit");
    return 0;
}

static int p_tsc(void) {
    unsigned lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    (void) lo;
    (void) hi;
    return 0;
}

// cmpxchg8b (%edi/%rdi). No lock prefix: this probes decode, and the locked
// path has its own coverage in x86/atomic_cmpxchg8b.c.
static int p_cx8(void) {
    static volatile uint64_t slot;
    uint32_t eax = 0, edx = 0;
    __asm__ volatile(".byte 0x0f, 0xc7, 0x0f"
                     : "+a"(eax), "+d"(edx), "+m"(slot)
                     : "D"(&slot), "b"(0u), "c"(0u)
                     : "cc", "memory");
    return 0;
}

static int p_cmov(void) {
    unsigned x = 1;
    __asm__ volatile("testl %0,%0\n\tcmovnel %0,%0" : "+r"(x) : : "cc");
    return 0;
}

static int p_mmx(void) {
    __asm__ volatile("pxor %%mm0,%%mm0\n\temms" : : : "memory");
    return 0;
}

// FXSAVE (0f ae /0) into a 16-byte aligned 512-byte area. The area is poisoned
// first and required to change: FXSAVE always writes the control, status and
// tag words, so an unchanged area means the opcode was decoded and dropped.
static int p_fxsr(void) {
    static char area[512] __attribute__((aligned(16)));
    memset(area, 0xa5, sizeof area);
    __asm__ volatile("fninit\n\tfld1");
    __asm__ volatile(".byte 0x0f, 0xae, 0x07" : : "D"(area) : "memory");
    __asm__ volatile("fninit");
    for (size_t i = 0; i < sizeof area; i++)
        if ((unsigned char) area[i] != 0xa5)
            return 0;
    return 1;
}

// XORPS is the defining SSE operation, but the SSE bit also promises MXCSR,
// which software reaches only through STMXCSR/LDMXCSR (0f ae /3 and /2). Both
// are checked, because a guest that cannot read back its own control word gets
// a silently wrong rounding mode rather than a fault.
static int p_sse(void) {
    unsigned mxcsr = 0xdeadbeefu;
    __asm__ volatile("xorps %%xmm0,%%xmm0" : : : "memory");
    __asm__ volatile(".byte 0x0f, 0xae, 0x1f" : : "D"(&mxcsr) : "memory");
    return mxcsr == 0xdeadbeefu ? 1 : 0;
}

static int p_sse2(void) {
    __asm__ volatile("pxor %%xmm0,%%xmm0" : : : "memory");
    return 0;
}

static int p_sse3(void) {
    __asm__ volatile("haddps %%xmm0,%%xmm0" : : : "memory");
    return 0;
}

static int p_ssse3(void) {
    __asm__ volatile("pshufb %%xmm0,%%xmm0" : : : "memory");
    return 0;
}

static int p_sse41(void) {
    __asm__ volatile("pmulld %%xmm0,%%xmm0" : : : "memory");
    return 0;
}

static int p_sse42(void) {
    __asm__ volatile("pcmpgtq %%xmm0,%%xmm0" : : : "memory");
    return 0;
}

static int p_popcnt(void) {
    unsigned x = 0xffu, r;
    __asm__ volatile("popcntl %1,%0" : "=r"(r) : "r"(x) : "cc");
    (void) r;
    return 0;
}

static int p_pclmulqdq(void) {
    __asm__ volatile("pclmulqdq $0,%%xmm0,%%xmm0" : : : "memory");
    return 0;
}

static int p_aesni(void) {
    __asm__ volatile("aesimc %%xmm0,%%xmm0" : : : "memory");
    return 0;
}

// xgetbv, ecx=0. 0f 01 d0 -- spelled as bytes because it is the one 0f 01
// register form a user-mode guest may execute and the rest of the group is
// ring-0 (see emu/decode.h's 0x01 case).
static int p_xgetbv(void) {
    unsigned lo, hi;
    __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0u));
    (void) lo;
    (void) hi;
    return 0;
}

// XSAVE (0f ae /4) with RFBM = x87|SSE. RFBM must be non-zero for the header
// to be a usable witness: with RFBM = 0 the SDM leaves XSTATE_BV exactly as it
// found it, so a no-op and a correct execution look identical. The 64-byte
// header sits at offset 512; poison it and require the write.
static int p_xsave(void) {
    // Larger than emu/cpuid.h's XSAVE_MAX_SIZE_ (2944) so the area is valid
    // whatever leaf 0x0D reports.
    static char area[4096] __attribute__((aligned(64)));
    memset(area, 0xa5, sizeof area);
    __asm__ volatile(".byte 0x0f, 0xae, 0x27"
                     : : "D"(area), "a"(3u), "d"(0u) : "memory");
    for (size_t i = 512; i < 512 + 64; i++)
        if ((unsigned char) area[i] != 0xa5)
            return 0;
    return 1;
}

static int p_avx(void) {
    __asm__ volatile("vaddps %%xmm0,%%xmm0,%%xmm0" : : : "memory");
    return 0;
}

static int p_avx2(void) {
    __asm__ volatile("vpaddd %%ymm0,%%ymm0,%%ymm0\n\tvzeroupper" : : : "memory");
    return 0;
}

static int p_bmi1(void) {
    unsigned x = 1, r;
    __asm__ volatile("andnl %1,%1,%0" : "=r"(r) : "r"(x) : "cc");
    (void) r;
    return 0;
}

static int p_bmi2(void) {
    unsigned x = 1, r;
    __asm__ volatile("shlxl %1,%1,%0" : "=r"(r) : "r"(x));
    (void) r;
    return 0;
}

static int p_avx512f(void) {
    __asm__ volatile("vpaddd %%zmm0,%%zmm0,%%zmm0\n\tvzeroupper" : : : "memory");
    return 0;
}

static int p_avx512dq(void) {
    __asm__ volatile("vandpd %%zmm0,%%zmm0,%%zmm0\n\tvzeroupper" : : : "memory");
    return 0;
}

static int p_avx512bw(void) {
    __asm__ volatile("vpaddb %%zmm0,%%zmm0,%%zmm0\n\tvzeroupper" : : : "memory");
    return 0;
}

// AVX512VL is the claim that the 128/256-bit forms exist in EVEX encoding.
// A bare vpaddd on ymm assembles to VEX and would prove nothing, so the write
// is masked: {%k1} is encodable only under EVEX.
static int p_avx512vl(void) {
    __asm__ volatile("kxorw %%k1,%%k1,%%k1\n\t"
                     "vpaddd %%ymm0,%%ymm0,%%ymm0%{%%k1%}\n\t"
                     "vzeroupper"
                     : : : "memory");
    return 0;
}

static int p_avx512_vbmi(void) {
    __asm__ volatile("vpermb %%zmm0,%%zmm0,%%zmm0\n\tvzeroupper" : : : "memory");
    return 0;
}

static int p_avx512_vbmi2(void) {
    __asm__ volatile("kxorw %%k1,%%k1,%%k1\n\t"
                     "vpcompressw %%zmm0,%%zmm1%{%%k1%}\n\t"
                     "vzeroupper"
                     : : : "memory");
    return 0;
}

static int p_gfni(void) {
    __asm__ volatile("gf2p8mulb %%xmm0,%%xmm0" : : : "memory");
    return 0;
}

static int p_vaes(void) {
    __asm__ volatile("vaesenc %%ymm0,%%ymm0,%%ymm0\n\tvzeroupper" : : : "memory");
    return 0;
}

static int p_vpclmulqdq(void) {
    __asm__ volatile("vpclmulqdq $0,%%ymm0,%%ymm0,%%ymm0\n\tvzeroupper"
                     : : : "memory");
    return 0;
}

static int p_avx512_vnni(void) {
    __asm__ volatile("vpdpbusd %%zmm0,%%zmm0,%%zmm0\n\tvzeroupper" : : : "memory");
    return 0;
}

static int p_avx512_vpopcntdq(void) {
    __asm__ volatile("vpopcntd %%zmm0,%%zmm0\n\tvzeroupper" : : : "memory");
    return 0;
}

#ifdef __x86_64__
// cmpxchg16b (%rdi): 48 0f c7 /1, long mode only.
static int p_cx16(void) {
    static volatile __int128 slot __attribute__((aligned(16)));
    uint64_t rax = 0, rdx = 0;
    __asm__ volatile(".byte 0x48, 0x0f, 0xc7, 0x0f"
                     : "+a"(rax), "+d"(rdx), "+m"(slot)
                     : "D"(&slot), "b"(0ull), "c"(0ull)
                     : "cc", "memory");
    return 0;
}

// The syscall instruction itself, not the libc wrapper: getpid(39) is chosen
// because it cannot fail and has no side effects. Advertising bit 11 of
// leaf 0x80000001 promises this entry path works, and AOK routes it through a
// different interrupt (INT_AMD64_SYSCALL) than int 0x80.
static int p_syscall(void) {
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(39L)
                     : "rcx", "r11", "memory");
    (void) ret;
    return 0;
}
#endif

// ---- the table -------------------------------------------------------------

enum { F_AMD64_ONLY = 1 };

struct feature {
    const char *name;
    unsigned leaf;
    unsigned subleaf;
    char reg;               // which of eax/ebx/ecx/edx holds the bit
    unsigned bit;
    int (*run)(void);   // 0 = executed, 1 = decoded but had no effect
    unsigned flags;
};

static const struct feature features[] = {
    // leaf 1, edx
    { "fpu",                1, 0, 'd',  0, p_fpu,        0 },
    { "tsc",                1, 0, 'd',  4, p_tsc,        0 },
    { "cx8",                1, 0, 'd',  8, p_cx8,        0 },
    { "cmov",               1, 0, 'd', 15, p_cmov,       0 },
    { "mmx",                1, 0, 'd', 23, p_mmx,        0 },
    { "fxsr",               1, 0, 'd', 24, p_fxsr,       0 },
    { "sse",                1, 0, 'd', 25, p_sse,        0 },
    { "sse2",               1, 0, 'd', 26, p_sse2,       0 },
    // leaf 1, ecx
    { "sse3",               1, 0, 'c',  0, p_sse3,       0 },
    { "pclmulqdq",          1, 0, 'c',  1, p_pclmulqdq,  0 },
    { "ssse3",              1, 0, 'c',  9, p_ssse3,      0 },
#ifdef __x86_64__
    { "cx16",               1, 0, 'c', 13, p_cx16,       F_AMD64_ONLY },
#endif
    { "sse4_1",             1, 0, 'c', 19, p_sse41,      0 },
    { "sse4_2",             1, 0, 'c', 20, p_sse42,      0 },
    { "popcnt",             1, 0, 'c', 23, p_popcnt,     0 },
    { "aesni",              1, 0, 'c', 25, p_aesni,      0 },
    { "xsave",              1, 0, 'c', 26, p_xsave,      0 },
    { "osxsave",            1, 0, 'c', 27, p_xgetbv,     0 },
    { "avx",                1, 0, 'c', 28, p_avx,        0 },
    // leaf 7 subleaf 0, ebx
    { "bmi1",               7, 0, 'b',  3, p_bmi1,       0 },
    { "avx2",               7, 0, 'b',  5, p_avx2,       0 },
    { "bmi2",               7, 0, 'b',  8, p_bmi2,       0 },
    { "avx512f",            7, 0, 'b', 16, p_avx512f,    0 },
    { "avx512dq",           7, 0, 'b', 17, p_avx512dq,   0 },
    { "avx512bw",           7, 0, 'b', 30, p_avx512bw,   0 },
    { "avx512vl",           7, 0, 'b', 31, p_avx512vl,   0 },
    // leaf 7 subleaf 0, ecx
    { "avx512_vbmi",        7, 0, 'c',  1, p_avx512_vbmi,        0 },
    { "avx512_vbmi2",       7, 0, 'c',  6, p_avx512_vbmi2,       0 },
    { "gfni",               7, 0, 'c',  8, p_gfni,               0 },
    { "vaes",               7, 0, 'c',  9, p_vaes,               0 },
    { "vpclmulqdq",         7, 0, 'c', 10, p_vpclmulqdq,         0 },
    { "avx512_vnni",        7, 0, 'c', 11, p_avx512_vnni,        0 },
    { "avx512_vpopcntdq",   7, 0, 'c', 14, p_avx512_vpopcntdq,   0 },
#ifdef __x86_64__
    // leaf 0x80000001, edx
    { "syscall",   0x80000001, 0, 'd', 11, p_syscall,    F_AMD64_ONLY },
#endif
};

// ---- driver ----------------------------------------------------------------

static unsigned max_basic_leaf;
static unsigned max_extended_leaf;

static int leaf_reachable(unsigned leaf) {
    if (leaf >= 0x80000000u)
        return leaf <= max_extended_leaf;
    return leaf <= max_basic_leaf;
}

static unsigned read_bit_reg(unsigned leaf, unsigned subleaf, char reg) {
    unsigned a, b, c, d;
    __cpuid_count(leaf, subleaf, a, b, c, d);
    switch (reg) {
        case 'a': return a;
        case 'b': return b;
        case 'c': return c;
        default:  return d;
    }
}

// Execute one probe with SIGILL and SIGSEGV both caught, so a probe that goes
// wrong is diagnosed rather than killing the run with no message.
static int run_guarded(int (*run)(void), int *was_noop) {
    struct sigaction sa, old_ill, old_segv;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, &old_ill);
    sigaction(SIGSEGV, &sa, &old_segv);

    fault_signo = 0;
    *was_noop = 0;
    if (sigsetjmp(fault_env, 1) == 0)
        *was_noop = run();

    sigaction(SIGILL, &old_ill, NULL);
    sigaction(SIGSEGV, &old_segv, NULL);
    return fault_signo;
}

static void check_features(void) {
    for (size_t i = 0; i < sizeof features / sizeof features[0]; i++) {
        const struct feature *f = &features[i];

        if (!leaf_reachable(f->leaf)) {
            // A bit can only be advertised through a leaf software can read.
            // If the max-leaf report hides it, nothing downstream will ever
            // see the bit and there is nothing to probe.
            test_logf("  skip %-18s leaf %#x unreachable (max basic %#x, "
                      "max extended %#x)\n",
                      f->name, f->leaf, max_basic_leaf, max_extended_leaf);
            continue;
        }

        unsigned val = read_bit_reg(f->leaf, f->subleaf, f->reg);
        if ((val & (1u << f->bit)) == 0) {
            test_logf("  dark %-18s leaf %#x %cx bit %u clear\n",
                      f->name, f->leaf, f->reg, f->bit);
            continue;
        }

        int was_noop = 0;
        int sig = run_guarded(f->run, &was_noop);
        if (sig != 0) {
            printf("FAIL: %s advertised (leaf %#x %cx bit %u) but its "
                   "instruction raised %s\n",
                   f->name, f->leaf, f->reg, f->bit, strsignal(sig));
            failures_total++;
            continue;
        }
        if (was_noop) {
            printf("FAIL: %s advertised (leaf %#x %cx bit %u) but its "
                   "instruction was decoded and had no effect\n",
                   f->name, f->leaf, f->reg, f->bit);
            failures_total++;
            continue;
        }
        test_logf("  ok   %-18s advertised and executes\n", f->name);
    }
}

// ---- leaf 0x0D: the XSAVE state map ----------------------------------------

// Only meaningful once XSAVE is advertised. AOK answers leaf 0x0D
// unconditionally today, describing components that the dark leaf-1 bits mean
// no guest will act on, so checking it while XSAVE is off would fail the suite
// over a map nothing reads.
static void check_xsave_layout(void) {
    unsigned a, b, c, d;

    __cpuid_count(1, 0, a, b, c, d);
    int xsave_advertised = (c & (1u << 26)) != 0;
    int osxsave_advertised = (c & (1u << 27)) != 0;

    if (!xsave_advertised) {
        test_logf("  dark xsave not advertised; leaf 0x0D map not checked\n");
        return;
    }
    if (!leaf_reachable(0x0D)) {
        printf("FAIL: xsave advertised but leaf 0x0D is above the reported "
               "max basic leaf (%#x)\n", max_basic_leaf);
        failures_total++;
        return;
    }

    unsigned xcr0_lo, area_enabled, area_max, hi;
    __cpuid_count(0x0D, 0, xcr0_lo, area_enabled, area_max, hi);

    // x87 and SSE live in the 512-byte legacy area and must always be present.
    if ((xcr0_lo & 0x3u) != 0x3u) {
        printf("FAIL: leaf 0x0D XCR0 map %#x omits x87/SSE\n", xcr0_lo);
        failures_total++;
    }
    if (area_enabled < 576 || area_max < 576) {
        printf("FAIL: leaf 0x0D area size too small (enabled=%u max=%u, "
               "legacy+header is 576)\n", area_enabled, area_max);
        failures_total++;
    }
    if (area_enabled > area_max) {
        printf("FAIL: leaf 0x0D enabled-size %u exceeds max-size %u\n",
               area_enabled, area_max);
        failures_total++;
    }

    // XGETBV must agree with the map. AOK holds XCR0 fixed at everything it
    // supports -- there is no CR4.OSXSAVE to clear and no way for a guest to
    // disable state -- so the two are required to be the same number, which is
    // also why the two sizes above are.
    if (osxsave_advertised) {
        unsigned xcr0_a, xcr0_d;
        __asm__ volatile(".byte 0x0f, 0x01, 0xd0"
                         : "=a"(xcr0_a), "=d"(xcr0_d) : "c"(0u));
        if (xcr0_a != xcr0_lo) {
            printf("FAIL: XGETBV(0) reports XCR0 %#x, leaf 0x0D map says %#x\n",
                   xcr0_a, xcr0_lo);
            failures_total++;
        }
        if (xcr0_d != 0) {
            printf("FAIL: XGETBV(0) high half is %#x, want 0\n", xcr0_d);
            failures_total++;
        }
    }

    // Every component above the legacy pair must describe a real, non-empty,
    // non-overlapping extent inside the reported area. These offsets are
    // hand-packed in emu/cpuid.h (MPX is absent and the AVX-512 components sit
    // up against the AVX one), so an editing slip here is silent until a guest
    // saves state into the wrong place.
    unsigned prev_end = 576;
    for (unsigned bit = 2; bit < 32; bit++) {
        if ((xcr0_lo & (1u << bit)) == 0)
            continue;
        unsigned size, offset;
        __cpuid_count(0x0D, bit, size, offset, c, d);
        if (size == 0 || offset == 0) {
            printf("FAIL: leaf 0x0D component %u enumerated in XCR0 but "
                   "subleaf reports size=%u offset=%u\n", bit, size, offset);
            failures_total++;
            continue;
        }
        if (offset < prev_end) {
            printf("FAIL: leaf 0x0D component %u at offset %u overlaps the "
                   "extent ending at %u\n", bit, offset, prev_end);
            failures_total++;
        }
        if ((uint64_t) offset + size > area_max) {
            printf("FAIL: leaf 0x0D component %u (offset %u size %u) runs "
                   "past the reported area size %u\n",
                   bit, offset, size, area_max);
            failures_total++;
        }
        test_logf("  ok   xsave component %-2u offset %5u size %5u\n",
                  bit, offset, size);
        prev_end = offset + size;
    }
}

// ---- leaf reachability ------------------------------------------------------

// glibc's __cpu_indicator_init will not look at leaf 7 unless the maximum
// basic leaf is at least 7, and sizes its state save area from leaf 0x0D.
// Advertising a bit in a leaf the max-leaf report hides is the same lie in the
// other direction: harmless to run, but it means the feature is unreachable.
static void check_leaf_reach(void) {
    for (size_t i = 0; i < sizeof features / sizeof features[0]; i++) {
        const struct feature *f = &features[i];
        if (leaf_reachable(f->leaf))
            continue;
        unsigned a, b, c, d;
        __cpuid_count(f->leaf, f->subleaf, a, b, c, d);
        unsigned val = f->reg == 'a' ? a : f->reg == 'b' ? b
                     : f->reg == 'c' ? c : d;
        if (val & (1u << f->bit)) {
            printf("FAIL: %s advertised in leaf %#x, which is above the "
                   "reported maximum (%#x basic / %#x extended)\n",
                   f->name, f->leaf, max_basic_leaf, max_extended_leaf);
            failures_total++;
        }
    }
}

int main(int argc, char **argv) {
    unsigned a, b, c, d;

    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    __cpuid_count(0, 0, a, b, c, d);
    max_basic_leaf = a;
    char vendor[13];
    memcpy(vendor + 0, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = '\0';

    __cpuid_count(0x80000000u, 0, a, b, c, d);
    max_extended_leaf = a;

    test_logf("vendor %s, max basic leaf %#x, max extended leaf %#x\n",
              vendor, max_basic_leaf, max_extended_leaf);

    // Leaf 7 carries every vector feature bit above AVX; if it is not
    // reachable the whole enumeration below is dead and glibc will not look.
    if (max_basic_leaf < 7) {
        printf("FAIL: max basic leaf is %#x, need at least 7 for the "
               "extended feature enumeration\n", max_basic_leaf);
        failures_total++;
    }

    check_features();
    check_leaf_reach();
    check_xsave_layout();

    return finish_suite("cpuid_xsave");
}

#endif
