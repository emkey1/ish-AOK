#!/usr/bin/env python3
# Writes a C program for a diff between an oracle (qemu-ppc64le, a POWER
# machine) and the AOK guest, as gen_alu_diff.py's, over what that and
# gen_vec_diff.py leave out: the OE forms (XER[OV] and SO), the extended
# divides, the VMX floating point (with VSCR[NJ] set, as Linux starts a
# program, and clear), the VSX conversions with NaN operands, and the
# negated multiply-adds on NaNs and on exact zeros.
#   tests/ppc64/gen_oe_fp_diff.py > oe_fp_diff.c
#   powerpc64le-linux-gnu-gcc -O1 -static -lm -o oe_fp_diff oe_fp_diff.c
# vexptefp and vlogefp are estimates: the program checks them against libm
# itself and prints "ok" or the operand and both values (qemu's exp2 is a
# Taylor series about 0 and fails this for large operands; AOK's should not).
# Expected differences from qemu 10: divde and divdeo where the quotient
# passes 2^63 in magnitude -- the ISA's overflow, which qemu misses (it gives
# the truncated quotient and no OV; AOK gives 0 and OV) -- and, after such a
# line, CR fields the compiler's own code set while SO was left on by it.

RR_O = ['addo', 'subfo', 'addco', 'subfco', 'addeo', 'subfeo', 'mulldo', 'mullwo',
        'divdo', 'divduo', 'divwo', 'divwuo', 'divweu', 'divwe', 'divdeu', 'divde',
        'divweuo', 'divweo', 'divdeuo', 'divdeo']
R1_O = ['nego', 'addzeo', 'addmeo', 'subfzeo', 'subfmeo']
RECORD = ['addo', 'subfo', 'mulldo', 'mullwo', 'divwo', 'divdeo']
VMX3 = ['vaddfp', 'vsubfp', 'vmaxfp', 'vminfp', 'vcmpeqfp', 'vcmpgefp', 'vcmpgtfp', 'vcmpbfp']
VMX2 = ['vrfin', 'vrfiz', 'vrfip', 'vrfim', 'vrefp', 'vrsqrtefp']
VMXU = ['vcfsx', 'vcfux', 'vctsxs', 'vctuxs']
X2 = ['xvcvdpuxws', 'xvcvspsxds', 'xvcvspuxds', 'xvcvsxdsp', 'xvcvuxdsp', 'xvcvspsxws',
      'xvcvdpsxds', 'xvcvdpsxws']
X3 = ['xvmaddadp', 'xvmsubadp', 'xvnmaddadp', 'xvnmsubadp', 'xvmaddasp', 'xvmsubasp',
      'xvnmaddasp', 'xvnmsubasp', 'xvmsubmdp', 'xvnmsubmsp']
F4 = ['fmadd', 'fmsub', 'fnmadd', 'fnmsub', 'fmsubs', 'fnmsubs']

out = []
w = out.append
w(r'''#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
static uint64_t s = 0x6a09e667f3bcc909ULL;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static uint64_t irnd(void) {
  uint64_t r = rnd();
  switch (r % 10) { case 0: return 0; case 1: return (uint64_t) -1; case 2: return 0x80000000;
    case 3: return 0x8000000000000000ULL; case 4: return 0x7fffffffffffffffULL; case 5: return r & 0xff;
    case 6: return (uint64_t) -(int64_t) (r & 0xffff); case 7: return 0x7fffffff; default: return r; }
}
// single-precision lanes: the specials and some ordinary values
static const uint32_t SP[] = {0x00000000, 0x80000000, 0x3fc00000, 0xc0100000, 0x0006ce3e /* denormal */,
  0x80000b2b /* -denormal */, 0x7f800000, 0xff800000, 0x7fc00001, 0xffc00002, 0x7f800005 /* sNaN */,
  0x7f61d7f4 /* 3e38 */, 0x006ce3ee /* 1e-38 */, 0x3dcccccd, 0x4640e6b6, 0xc0e00000, 0x4b800001,
  0x40000000, 0x3f800000, 0x00800000, 0x4f000000 /* 2^31 */, 0xcf000001, 0x4f800000 /* 2^32 */};
static const uint64_t DP[] = {0, 0x8000000000000000ULL, 0x3ff8000000000000ULL, 0xc002000000000000ULL,
  0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x7ff8000000000001ULL, 0xfff8000000000002ULL,
  0x7ff0000000000005ULL, 0x0000000000000123ULL, 0x41e0000000000000ULL /* 2^31 */,
  0xc3e0000000000001ULL, 0x43f0000000000000ULL /* 2^64 */, 0x4000000000000000ULL,
  0x3ff0000000000000ULL, 0xbff0000000000000ULL, 0x4024000000000000ULL};
#define NSP (sizeof SP / sizeof SP[0])
#define NDP (sizeof DP / sizeof DP[0])
static uint64_t sp2(void) { uint64_t r = rnd(); return (uint64_t) SP[r % NSP] << 32 | SP[(r >> 8) % NSP]; }
static uint64_t dp(void) { return DP[rnd() % NDP]; }
static uint64_t A[2] __attribute__((aligned(16))), B[2] __attribute__((aligned(16))),
  C[2] __attribute__((aligned(16))), R[2] __attribute__((aligned(16))), V[2] __attribute__((aligned(16)));
#define P(name) printf("%s %016llx%016llx %016llx%016llx %016llx%016llx -> %016llx%016llx\n", name, \
  (unsigned long long) A[1], (unsigned long long) A[0], (unsigned long long) B[1], (unsigned long long) B[0], \
  (unsigned long long) C[1], (unsigned long long) C[0], (unsigned long long) R[1], (unsigned long long) R[0])
#define LD "lvx 0,0,%0\n\tlvx 1,0,%1\n\tlvx 2,0,%2\n\tlvx 3,0,%3\n\t"
#define IN : : "r"(A), "r"(B), "r"(C), "r"(R) : "memory", "v0", "v1", "v2", "v3", "cr6"
#define SHOW(name) printf("%s %016llx %016llx -> %016llx xer %08llx cr %08llx\n", name, \
  (unsigned long long) a, (unsigned long long) b, (unsigned long long) r, \
  (unsigned long long) (xer & 0xe0000000), (unsigned long long) cr)
static void nj(int on) {
  V[0] = on ? 0x10000 : 0; V[1] = 0;
  __asm__ volatile("lvx 4,0,%0\n\tmtvscr 4" : : "r"(V) : "memory", "v4");
}
static float f32(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static uint32_t b32(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
int main(void) {
  for (int i = 0; i < 64; i++) {
    uint64_t a = irnd(), b = irnd(), r, xer, cr, x = (rnd() & 1) << 29 | (i & 4 ? 1ULL << 31 : 0);
''')

def body(asm, name):
    return ('    __asm__ volatile("mtxer %%3\\n\\t" "%s\\n\\t" "mfxer %%1\\n\\t" "mfcr %%2" '
            ': "=&r"(r), "=&r"(xer), "=&r"(cr) : "r"(x), "r"(a), "r"(b) : "xer", "cr0"); SHOW("%s");\n'
            % (asm, name))

for op in RR_O:
    w(body('%s %%0,%%4,%%5' % op, op))
for op in R1_O:
    w(body('%s %%0,%%4' % op, op))
for op in RECORD:
    w(body('%s. %%0,%%4,%%5' % op, op + '.'))
w(body('nego. %0,%4', 'nego.'))
w('  }\n')

w('  for (int it = 0; it < 48; it++) {\n')
w('    int njon = !(it & 1); nj(njon);\n')
w('    A[0] = sp2(); A[1] = sp2(); B[0] = sp2(); B[1] = sp2(); C[0] = sp2(); C[1] = sp2();\n')
w('    if (it % 6 == 0) { B[0] = A[0]; B[1] = A[1]; }\n')
w('    printf("nj %d\\n", njon);\n')
for op in VMX3:
    w('    __asm__ volatile(LD "%s 3,0,1\\n\\tstvx 3,0,%%3" IN); P("%s");\n' % (op, op))
    if op.startswith('vcmp'):
        w('    __asm__ volatile(LD "%s. 3,0,1\\n\\tmfcr 9\\n\\tstd 9,0(%%3)" : : "r"(A), "r"(B), "r"(C), "r"(R) '
          ': "memory", "v0", "v1", "v2", "v3", "cr6", "r9"); R[0] = (R[0] >> 4) & 15; R[1] = 0; P("%s.");\n'
          % (op, op))
for op in ['vmaddfp', 'vnmsubfp']:
    w('    __asm__ volatile(LD "%s 3,0,2,1\\n\\tstvx 3,0,%%3" IN); P("%s");\n' % (op, op))
w('    { uint64_t c0 = C[0], c1 = C[1]; C[0] = 0x3f8000003f800000ULL; C[1] = 0x4000000040000000ULL;\n')
w('      B[0] = A[0]; B[1] = A[1] ^ 0x0000000080000000ULL;\n')
w('      __asm__ volatile(LD "vnmsubfp 3,0,2,1\\n\\tstvx 3,0,%3" IN); P("vnmsubfp=");\n')
w('      C[0] = c0; C[1] = c1; }\n')
for op in VMX2:
    w('    __asm__ volatile(LD "%s 3,1\\n\\tstvx 3,0,%%3" IN); P("%s");\n' % (op, op))
for op in VMXU:
    for u in (0, 1, 5, 31):
        w('    __asm__ volatile(LD "%s 3,1,%d\\n\\tstvx 3,0,%%3" IN); P("%s,%d");\n' % (op, u, op, u))
    w('    { uint64_t b0 = B[0], b1 = B[1]; B[0] = irnd(); B[1] = irnd();\n')
    for u in (0, 3):
        w('      __asm__ volatile(LD "%s 3,1,%d\\n\\tstvx 3,0,%%3" IN); P("%s,%d,int");\n' % (op, u, op, u))
    w('      B[0] = b0; B[1] = b1; }\n')
# FPSCR untouched by VMX: clear it, raise everything VMX can, read it back
w('    { uint64_t f; double z = 0; __asm__ volatile("mtfsf 0xff,%5\\n\\t" LD "vaddfp 3,0,1\\n\\tvrefp 3,1\\n\\t'
  'vmaddfp 3,0,1,2\\n\\tvctsxs 3,1,31\\n\\tmffs 0\\n\\tstfd 0,0(%4)" : : "r"(A), "r"(B), "r"(C), "r"(R), "r"(&f), "d"(z) '
  ': "memory", "v0", "v1", "v2", "v3", "fr0"); R[0] = f; R[1] = 0; P("fpscr-after-vmx"); }\n')
w('  }\n  nj(1);\n')

# the estimates against libm
w(r'''  for (int i = 0; i < 400; i++) {
    float xs[4];
    for (int k = 0; k < 4; k++) {
      uint64_t r = rnd();
      xs[k] = i < 200 ? (float) ((int64_t) (r % 60000) - 30000) / 200.0f : f32(SP[r % NSP]);
      if (i >= 200 && (r & 0x10000)) xs[k] = (float) (r % 100000) / 7.0f;
    }
    memcpy(B, xs, 16);
    float e[4], l[4];
    __asm__ volatile(LD "vexptefp 3,1\n\tstvx 3,0,%3" IN); memcpy(e, R, 16);
    __asm__ volatile(LD "vlogefp 3,1\n\tstvx 3,0,%3" IN); memcpy(l, R, 16);
    for (int k = 0; k < 4; k++) {
      float x = xs[k];
      if (fpclassify(x) == FP_SUBNORMAL) continue;      // NJ flushes it
      float ee = exp2f(x), le = log2f(x);
      int eok = isnan(ee) ? isnan(e[k]) : isinf(ee) ? e[k] == ee : ee == 0 || fpclassify(ee) == FP_SUBNORMAL ? e[k] == 0 || fabsf(e[k] - ee) < 1e-38f : fabsf(e[k] - ee) <= fabsf(ee) * 2e-7f;
      int lok = isnan(le) || signbit(x) && x != 0 ? isnan(l[k]) : isinf(le) ? l[k] == le : fabsf(l[k] - le) <= 4e-7f * (fabsf(le) > 1 ? fabsf(le) : 1);
      if (!eok) printf("vexptefp %08x: %08x, libm %08x\n", b32(x), b32(e[k]), b32(ee));
      if (!lok) printf("vlogefp %08x: %08x, libm %08x\n", b32(x), b32(l[k]), b32(le));
    }
  }
  printf("estimates checked\n");
''')

# conversions and multiply-adds on doubles and singles, NaNs included
w('  for (int it = 0; it < 48; it++) {\n')
w('    if (it & 1) { A[0] = dp(); A[1] = dp(); B[0] = dp(); B[1] = dp(); C[0] = dp(); C[1] = dp(); }\n')
w('    else { A[0] = sp2(); A[1] = sp2(); B[0] = sp2(); B[1] = sp2(); C[0] = sp2(); C[1] = sp2(); }\n')
w('    if (it % 4 == 1) { A[0] = 0x3ff0000000000000ULL; B[0] = 0x4000000000000000ULL; C[0] = B[0]; }\n')
w('    if (it % 4 == 2) { A[0] = 0x3f8000003f800000ULL; B[0] = 0x4000000040000000ULL; C[0] = B[0]; }\n')
w('    if (it % 8 == 3) { B[0] = irnd(); B[1] = irnd(); }\n')
for op in X2:
    w('    __asm__ volatile(LD "%s 35,33\\n\\tstvx 3,0,%%3" IN); P("%s");\n' % (op, op))
for op in X3:
    w('    __asm__ volatile(LD "vor 3,2,2\\n\\t%s 35,32,33\\n\\tstvx 3,0,%%3" IN); P("%s");\n' % (op, op))
for op in F4:
    w('    { double fa, fb, fc, fr; memcpy(&fa, &A[0], 8); memcpy(&fb, &B[0], 8); memcpy(&fc, &C[0], 8);\n'
      '      __asm__ volatile("%s %%0,%%1,%%2,%%3" : "=d"(fr) : "d"(fa), "d"(fc), "d"(fb)); memcpy(&R[0], &fr, 8); R[1] = 0; P("%s"); }\n'
      % (op, op))
w('  }\n  return 0;\n}\n')
print(''.join(out))
