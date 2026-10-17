#!/usr/bin/env python3
# Every VMX and VSX instruction AOK's ppc64 guest has a gadget for, on fixed
# pseudo-random vectors: a diff between an oracle and the AOK guest (see
# gen_alu_diff.py). Vectors in and out through memory as two doublewords.
V3 = '''vand vandc vor vxor vnor vorc vnand veqv vaddubm vadduhm vadduwm vaddudm vsububm vsubuhm vsubuwm vsubudm
vmaxub vmaxuh vmaxuw vmaxsb vmaxsh vmaxsw vminub vminuh vminuw vminsb vminsh vminsw vaddubs vadduhs vadduws vaddsbs
vaddshs vaddsws vsububs vsubuhs vsubuws vsubsbs vsubshs vsubsws vavgub vavguh vavguw vavgsb vavgsh vavgsw vmuluwm
vslb vslh vslw vsld vsrb vsrh vsrw vsrd vsrab vsrah vsraw vsrad vrlb vrlh vrlw vrld vmrghb vmrghh vmrghw vmrglb vmrglh
vmrglw vcmpequb vcmpequh vcmpequw vcmpequd vcmpgtub vcmpgtuh vcmpgtuw vcmpgtud vcmpgtsb vcmpgtsh vcmpgtsw vcmpgtsd
vslo vsro vsl vsr vbpermq vmuleub vmuloub vmuleuh vmulouh vmuleuw vmulouw vmulesb vmulosb vmulesh vmulosh vmulesw
vmulosw vpkuhum vpkuwum vpkudum vpkshss vpkswss vpksdss vpkshus vpkswus vpksdus vpkuhus vpkuwus vpkudus vsumsws
vsum2sws vsum4ubs vsum4sbs vsum4shs vadduqm vsubuqm vaddcuw vsubcuw vmrgew vmrgow vmaxsd vmaxud vminsd vminud'''.split()
V2 = 'vupkhsb vupklsb vupkhsh vupklsh vupkhsw vupklsw vpopcntb vpopcnth vpopcntw vpopcntd vclzb vclzh vclzw vclzd vgbbd'.split()
X3 = '''xxland xxlandc xxlor xxlxor xxlnor xxlorc xxlnand xxleqv xxmrghw xxmrglw xvadddp xvaddsp xvsubdp xvsubsp
xvmuldp xvmulsp xvdivdp xvdivsp xvmaxdp xvmaxsp xvmindp xvminsp xvcmpeqdp xvcmpeqsp xvcmpgtdp xvcmpgtsp xvcmpgedp
xvcmpgesp xvcpsgndp xvcpsgnsp xvmaddadp xvmaddmdp xvmaddasp xvmaddmsp xvmsubadp xvmsubmdp xvnmaddadp xvnmsubadp'''.split()
X2 = '''xvsqrtdp xvsqrtsp xvabssp xvnegsp xvnabssp xvnegdp xvabsdp xvnabsdp xvrdpi xvrdpiz xvrdpip xvrdpim xvrspiz
xvcvsxwsp xvcvuxwsp xvcvspsxws xvcvspuxws xvcvsxddp xvcvuxddp xvcvdpsxds xvcvdpuxds xvcvspdp xvcvsxwdp xvcvuxwdp
xvcvdpsp xvcvdpsxws xxswapd'''.split()
out = []
w = out.append
w('''#include <stdio.h>
#include <stdint.h>
static uint64_t s = 0x2545f4914f6cdd1dULL;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static uint64_t fpv(void) { uint64_t r = rnd(); int k = r % 6; double d = k == 0 ? (double) (int32_t) r : k == 1 ? (double) (int64_t) r / 3e9 : k == 2 ? -0.5 : k == 3 ? 1e10 : (double) (r & 0xffff) / 7.0;
  if (r & 0x100) { float f = (float) d; uint32_t b; __builtin_memcpy(&b, &f, 4); return (uint64_t) b << 32 | b; }
  uint64_t b; __builtin_memcpy(&b, &d, 8); return b; }
static uint64_t A[2] __attribute__((aligned(16))), B[2] __attribute__((aligned(16))), C[2] __attribute__((aligned(16))), R[2] __attribute__((aligned(16)));
#define P(name) printf("%s %016llx%016llx %016llx%016llx %016llx%016llx -> %016llx%016llx\\n", name, \\
  (unsigned long long) A[1], (unsigned long long) A[0], (unsigned long long) B[1], (unsigned long long) B[0], \\
  (unsigned long long) C[1], (unsigned long long) C[0], (unsigned long long) R[1], (unsigned long long) R[0])
#define LD "lvx 0,0,%0\\n\\tlvx 1,0,%1\\n\\tlvx 2,0,%2\\n\\tlvx 3,0,%3\\n\\t"
#define IN : : "r"(A), "r"(B), "r"(C), "r"(R) : "memory", "v0", "v1", "v2", "v3", "cr6"
int main(void) {
  for (int it = 0; it < 40; it++) {
    int fp = it & 1;
    A[0] = fp ? fpv() : rnd(); A[1] = fp ? fpv() : rnd(); B[0] = fp ? fpv() : rnd(); B[1] = fp ? fpv() : rnd(); C[0] = rnd(); C[1] = rnd();
    if (it % 5 == 0) { B[0] = A[0]; B[1] ^= 0x1ULL << 40; }
''')
for op in V3:
    w('    __asm__ volatile(LD "%s 3,0,1\\n\\tstvx 3,0,%%3" IN); P("%s");\n' % (op, op))
    if op.startswith('vcmp'):
        w('    __asm__ volatile(LD "%s. 3,0,1\\n\\tmfcr 9\\n\\tstd 9,0(%%3)" : : "r"(A), "r"(B), "r"(C), "r"(R) : "memory", "v0", "v1", "v2", "v3", "cr6", "r9"); R[0] = (R[0] >> 4) & 15; R[1] = 0; P("%s.");\n' % (op, op))
for op in V2:
    w('    __asm__ volatile(LD "%s 3,1\\n\\tstvx 3,0,%%3" IN); P("%s");\n' % (op, op))
for op in ['vperm', 'vsel']:
    w('    __asm__ volatile(LD "%s 3,0,1,2\\n\\tstvx 3,0,%%3" IN); P("%s");\n' % (op, op))
for sh in range(16):
    w('    __asm__ volatile(LD "vsldoi 3,0,1,%d\\n\\tstvx 3,0,%%3" IN); P("vsldoi%d");\n' % (sh, sh))
for u in range(16):
    w('    __asm__ volatile(LD "vspltb 3,1,%d\\n\\tstvx 3,0,%%3" IN); P("vspltb%d");\n' % (u, u))
for u in range(8):
    w('    __asm__ volatile(LD "vsplth 3,1,%d\\n\\tstvx 3,0,%%3" IN); P("vsplth%d");\n' % (u, u))
for u in range(4):
    w('    __asm__ volatile(LD "vspltw 3,1,%d\\n\\tstvx 3,0,%%3" IN); P("vspltw%d");\n' % (u, u))
    w('    __asm__ volatile(LD "xxspltw 35,33,%d\\n\\tstvx 3,0,%%3" IN); P("xxspltw%d");\n' % (u, u))
    w('    __asm__ volatile(LD "xxsldwi 35,32,33,%d\\n\\tstvx 3,0,%%3" IN); P("xxsldwi%d");\n' % (u, u))
    w('    __asm__ volatile(LD "xxpermdi 35,32,33,%d\\n\\tstvx 3,0,%%3" IN); P("xxpermdi%d");\n' % (u, u))
for v in [-16, -1, 0, 5, 15]:
    for op in ['vspltisb', 'vspltish', 'vspltisw']:
        w('    __asm__ volatile(LD "%s 3,%d\\n\\tstvx 3,0,%%3" IN); P("%s%d");\n' % (op, v, op, v))
for op in X3:
    w('    __asm__ volatile(LD "vor 3,2,2\\n\\t%s 35,32,33\\n\\tstvx 3,0,%%3" IN); P("%s");\n' % (op, op))
for op in X2:
    w('    __asm__ volatile(LD "%s 35,33\\n\\tstvx 3,0,%%3" IN); P("%s");\n' % (op, op))
w('    __asm__ volatile(LD "xxsel 35,32,33,34\\n\\tstvx 3,0,%3" IN); P("xxsel");\n')
w('    __asm__ volatile(LD "lvsl 3,0,%0\\n\\tstvx 3,0,%3" IN); P("lvsl");\n')
w('    { uint64_t a = (uint64_t) A + (it & 15); __asm__ volatile("lvsl 3,0,%0\\n\\tstvx 3,0,%1\\n\\tlvsr 4,0,%0\\n\\tstvx 4,0,%2" : : "r"(a), "r"(R), "r"(C) : "memory", "v3", "v4"); P("lvsl/lvsr"); }\n')
w('  }\n  return 0;\n}\n')
print(''.join(out))
