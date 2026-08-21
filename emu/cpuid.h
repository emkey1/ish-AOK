#ifndef CPUID_H
#define CPUID_H

#include "misc.h"
#include "kernel/task.h"
#include "kernel/abi.h"
extern bool isGlibC;

static inline bool cpuid_guest_supports_long_mode(void) {
    return current != NULL && current->abi == GUEST_ABI_AMD64;
}

// Highest basic leaf we answer. 0x0D is the XSAVE state enumeration, which is
// the last one we implement and which software must be able to reach: glibc's
// __cpu_indicator_init refuses to look at leaf 7 (AVX2/BMI/AVX-512) unless the
// maximum basic leaf is at least 7, and sizes its state save area from 0x0D.
static inline dword_t cpuid_basic_max_leaf(void) {
    return 0x0D;
}

static inline dword_t cpuid_extended_max_leaf(void) {
    return cpuid_guest_supports_long_mode() ? 0x80000001u : 0x80000000u;
}

// ---- XSAVE state components ------------------------------------------------
//
// We support x87, SSE, AVX (YMM_Hi128) and the three AVX-512 components. MPX
// (states 3 and 4) is not implemented, so it is absent from XCR0 and the later
// components are packed up against the AVX one. Software is required to take
// component offsets from CPUID leaf 0x0D rather than assume a fixed map -- real
// silicon varies here too, since Ice Lake dropped MPX exactly like this -- so
// the only hard requirement is that this table and the offsets the XSAVE
// implementation uses agree.
//
// There is no XSAVE implementation yet, so this table is currently the only
// definition of the map and nothing derives from it. Leaf 0x0D answers
// unconditionally all the same, which is harmless only because
// CPUID_ADVERTISE_VECTOR_STATE keeps the leaf-1 XSAVE bit dark: software
// checks that bit before it goes looking here. Whoever writes XSAVE/XRSTOR
// must take its offsets from these constants rather than repeat them.
#define XCR0_X87_       (1u << 0)
#define XCR0_SSE_       (1u << 1)
#define XCR0_YMM_       (1u << 2)
#define XCR0_OPMASK_    (1u << 5)
#define XCR0_ZMM_HI256_ (1u << 6)
#define XCR0_HI16_ZMM_  (1u << 7)

#define XCR0_SUPPORTED_ (XCR0_X87_ | XCR0_SSE_ | XCR0_YMM_ | \
                         XCR0_OPMASK_ | XCR0_ZMM_HI256_ | XCR0_HI16_ZMM_)

// Legacy FXSAVE area, then the 64-byte XSAVE header, then the extended states.
#define XSAVE_LEGACY_SIZE_   512
#define XSAVE_HEADER_SIZE_   64
#define XSAVE_YMM_OFFSET_    576            // 512 + 64
#define XSAVE_YMM_SIZE_      256            // 16 regs * 16 bytes of ymm_hi
#define XSAVE_OPMASK_OFFSET_ 832            // 576 + 256
#define XSAVE_OPMASK_SIZE_   64             // k0-k7, 8 bytes each
#define XSAVE_ZMM_HI_OFFSET_ 896            // 832 + 64
#define XSAVE_ZMM_HI_SIZE_   1024           // 16 regs * 32 bytes (bits 256-511)
#define XSAVE_HI16_OFFSET_   1920           // 896 + 1024
#define XSAVE_HI16_SIZE_     1024           // regs 16-31, 64 bytes each
#define XSAVE_MAX_SIZE_      2944           // 1920 + 1024

// XCR0 as XGETBV(0) reports it. iSH is the OS, so every component we support
// is permanently enabled: there is no CR4.OSXSAVE to clear and no path for a
// guest to disable state it cannot save for itself. Constant by construction,
// which is also why leaf 0x0D's "size for enabled features" and "size for all
// supported features" are the same number.
static inline qword_t xcr0_value(void) {
    return XCR0_SUPPORTED_;
}

// Whether to tell guests the vector state exists.
//
// The machinery below (leaf 7, leaf 0x0D, XGETBV, XCR0) is complete and
// tested, but advertising AVX is only SAFE once the extended vector state
// survives a signal. Until then glibc would start selecting AVX string
// routines, those run inside signal handlers too, and a handler clobbering
// ymm_hi has nowhere to restore it from -- kernel/signal.c's amd64_fpstate_ is
// static_asserted to the 512-byte legacy FXSAVE layout, which holds only the
// low 128 bits of xmm[0-15]. That would trade today's dormant gap for silent
// register corruption in any program taking a signal mid-AVX, which is a
// strictly worse bug than the one we are fixing.
//
// So the enumeration lands dark and this flips to 1 in the same commit that
// teaches the signal frame (and ptrace's regsets) to carry ymm_hi, zmm_hi,
// xmm_ext and the opmask registers. XSAVE/XRSTOR must land with it too: bit 26
// below promises those instructions exist, and today nothing implements them.
//
// The signal frame is not the only debt. Flipping this to 1 and running
// tests/manual/x86/cpuid_xsave.c on an i386 guest reports thirteen features
// advertised that the guest cannot execute: every AVX-512 bit (the i386 front
// end reaches vector code only through gen_vex32, which is VEX-only -- there
// is no EVEX decoder), BMI1 and BMI2, and the legacy SSE encodings of AESNI
// and PCLMULQDQ. The amd64 guest is in better shape but shares the XSAVE gap.
// Run that test on BOTH guest ABIs before believing the switch is safe: these
// bits are advertised from ABI-independent functions, so i386 gets whatever
// amd64 is promised.
#define CPUID_ADVERTISE_VECTOR_STATE 0

static inline dword_t cpuid_leaf1_ecx_features(void) {
    dword_t features = 0;
    // SSE3, SSSE3, SSE4.1, and SSE4.2 are fully implemented in both the i386
    // and amd64 engines (interp + JIT) and validated bit-exact against real
    // Intel silicon (tests/remote/corpus/sse3.c, sse4.c, sse42.c). Advertise
    // them so feature-detecting software (glibc string ifuncs, codecs, hashers)
    // selects these paths instead of a slower SSE2/scalar fallback.
    features |= (1 << 0);   // sse3 (pni)
    features |= (1 << 9);   // ssse3
    features |= (1 << 19);  // sse4.1
    features |= (1 << 20);  // sse4.2
    // POPCNT is implemented for both i386 (emu/decode.h JIT decoder) and
    // amd64 (emu/amd64_interp.c), validated bit-exact against real Intel
    // silicon (tests/remote/corpus/popcnt_lzcnt_tzcnt.c), so it's safe to
    // advertise for either guest ABI (unlike cx16 below, this isn't
    // encoding-restricted to long mode).
    features |= (1 << 23);  // popcnt
#if CPUID_ADVERTISE_VECTOR_STATE
    // AESNI and PCLMULQDQ go with the vector switch rather than ahead of it:
    // avx_regress covers their VEX forms, but advertising them makes OpenSSL
    // and friends emit the LEGACY SSE encodings, which is a separate claim
    // needing its own evidence before it is made.
    features |= (1 << 1);   // pclmulqdq
    features |= (1 << 25);  // aesni
    // The XSAVE feature set (XSAVE/XRSTOR/XGETBV), and since we are the OS the
    // OS-enabled bit is simply always on. Both matter: glibc gates its whole
    // AVX path on OSXSAVE plus an XGETBV check of XCR0, so advertising AVX
    // without these two would be ignored by every ifunc.
    features |= (1 << 26);  // xsave
    features |= (1 << 27);  // osxsave
    features |= (1 << 28);  // avx
#endif
    // cmpxchg16b is implemented for the amd64 (long-mode) guest, so advertise it
    // -- feature-detecting software (glibc, C++ 128-bit lock-free CAS) checks
    // this bit before emitting the instruction. It is a long-mode-only op, so it
    // is not advertised to i386 guests (which cannot encode it).
    if (cpuid_guest_supports_long_mode())
        features |= (1 << 13); // cx16 (cmpxchg16b)
    return features;
}

static inline dword_t cpuid_leaf1_edx_features(void) {
    return (1 << 0)   // fpu
        | (1 << 4)    // tsc
        | (1 << 8)    // cx8
        | (1 << 15)   // cmov
        | (1 << 23)   // mmx
        | (1 << 24)   // fxsr
        | (1 << 25)   // sse
        | (1 << 26);  // sse2
}

// Leaf 7 subleaf 0: the structured extended feature flags. Everything here is
// implemented by emu/avx.c plus the amd64 interpreter's VEX/EVEX decoder and
// covered by tests/manual/x86/avx_regress.c. The i386 guest is NOT in the same
// position: gen_vex32 decodes VEX only, so the AVX-512 bits below have nothing
// behind them there -- see the CPUID_ADVERTISE_VECTOR_STATE comment above.
//
// tests/manual/x86/cpuid_xsave.c walks every bit this file advertises and runs
// a representative instruction for each one, so a bit turned on here without an
// implementation behind it fails the suite rather than SIGILLing a user. It
// also catches the quieter failure: an instruction the decoder accepts and then
// discards, which is how FXSAVE and STMXCSR spent a long time doing nothing at
// all on the i386 guest while fxsr and sse were advertised.
static inline dword_t cpuid_leaf7_ebx_features(void) {
#if !CPUID_ADVERTISE_VECTOR_STATE
    return 0;
#else
    return (1u << 3)    // bmi1
        | (1u << 5)     // avx2
        | (1u << 8)     // bmi2
        | (1u << 16)    // avx512f
        | (1u << 17)    // avx512dq
        | (1u << 30)    // avx512bw
        | (1u << 31);   // avx512vl
#endif
}

static inline dword_t cpuid_leaf7_ecx_features(void) {
#if !CPUID_ADVERTISE_VECTOR_STATE
    return 0;
#else
    return (1u << 1)    // avx512_vbmi
        | (1u << 6)     // avx512_vbmi2
        | (1u << 8)     // gfni
        | (1u << 9)     // vaes
        | (1u << 10)    // vpclmulqdq
        | (1u << 11)    // avx512_vnni
        | (1u << 14);   // avx512_vpopcntdq
#endif
}

static inline void cpuid_leaf_d(dword_t subleaf, dword_t *eax, dword_t *ebx,
                                dword_t *ecx, dword_t *edx) {
    *eax = *ebx = *ecx = *edx = 0;
    switch (subleaf) {
        case 0:
            // Which components XCR0 may enable, and how big the save area is.
            // We always report the maximum: XCR0 is fixed, so "size for the
            // currently enabled features" and "size for all supported
            // features" are the same number.
            *eax = XCR0_SUPPORTED_;
            *ebx = XSAVE_MAX_SIZE_;
            *ecx = XSAVE_MAX_SIZE_;
            break;
        case 1:
            // No XSAVEOPT/XSAVEC/XGETBV1/XSAVES: plain XSAVE and XRSTOR only,
            // so every bit here stays clear and software falls back to them.
            break;
        case 2:
            *eax = XSAVE_YMM_SIZE_;
            *ebx = XSAVE_YMM_OFFSET_;
            break;
        case 5:
            *eax = XSAVE_OPMASK_SIZE_;
            *ebx = XSAVE_OPMASK_OFFSET_;
            break;
        case 6:
            *eax = XSAVE_ZMM_HI_SIZE_;
            *ebx = XSAVE_ZMM_HI_OFFSET_;
            break;
        case 7:
            *eax = XSAVE_HI16_SIZE_;
            *ebx = XSAVE_HI16_OFFSET_;
            break;
        default:
            break;
    }
}

static inline dword_t cpuid_leaf80000001_ecx_features(void) {
    return 0;
}

static inline dword_t cpuid_leaf80000001_edx_features(void) {
    dword_t features = 0;
    if (cpuid_guest_supports_long_mode()) {
        features |= (1 << 11); // syscall/sysret
        features |= (1 << 29); // lm
    }
    return features;
}

// ecx is an in/out parameter: leaves 7 and 0x0D take it as a subleaf selector
// on the way in. Callers already hand us the guest's live ecx, so nothing else
// has to change for that.
static inline void do_cpuid(dword_t *eax, dword_t *ebx, dword_t *ecx, dword_t *edx) {
    dword_t leaf = *eax;
    dword_t subleaf = *ecx;
    switch (leaf) {
        case 0:
            *eax = cpuid_basic_max_leaf();
            *ebx = 0x756e6547; // Genu
            *edx = 0x49656e69; // ineI
            *ecx = 0x6c65746e; // ntel
            break;
        case 1:
            *eax = 0x0; // say nothing about cpu model number
            *ebx = 0x0; // processor number 0, flushes 0 bytes on clflush
            *ecx = cpuid_leaf1_ecx_features();
            *edx = cpuid_leaf1_edx_features();
            break;
        case 7:
            if (subleaf == 0) {
                *eax = 0; // highest subleaf
                *ebx = cpuid_leaf7_ebx_features();
                *ecx = cpuid_leaf7_ecx_features();
                *edx = 0;
            } else {
                *eax = *ebx = *ecx = *edx = 0;
            }
            break;
        case 0x0D:
            cpuid_leaf_d(subleaf, eax, ebx, ecx, edx);
            break;
        case 0x80000000:
            *eax = cpuid_extended_max_leaf();
            *ebx = 0;
            *ecx = 0;
            *edx = 0;
            break;
        case 0x80000001:
            *eax = 0;
            *ebx = 0;
            *ecx = cpuid_leaf80000001_ecx_features();
            *edx = cpuid_leaf80000001_edx_features();
            break;
        default: // if leaf is too high, use highest supported leaf
            if (leaf >= 0x80000000) {
                *eax = cpuid_extended_max_leaf();
                *ebx = 0;
                *ecx = 0;
                *edx = 0;
            } else {
                // Leaves we don't implement but which are below the maximum
                // (2, 3, 4, ...) read as zero rather than as leaf 1's data:
                // returning feature words in a leaf that doesn't define them
                // hands out nonsense (leaf 1's ECX bit 9 is SSSE3, but in leaf
                // 7's namespace bit 9 is VAES). Intel only specifies the
                // highest-leaf mirroring for input ABOVE the maximum, which is
                // the case handled below.
                *eax = 0;
                *ebx = 0;
                *ecx = 0;
                *edx = 0;
                if (leaf > cpuid_basic_max_leaf()) {
                    *ecx = cpuid_leaf1_ecx_features();
                    *edx = cpuid_leaf1_edx_features();
                }
            }
            break;
    }
}

#endif
