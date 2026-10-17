#!/usr/bin/env python3
# Writes a C program that runs ppc64 loads and stores in every form, the
# condition register and branch instructions, and scalar floating point on
# fixed pseudo-random operands, printing what each leaves, for a diff between
# an oracle (qemu-ppc64le, a POWER machine) and the AOK guest (see
# gen_alu_diff.py for the integer ALU).
#   tests/ppc64/gen_isa_diff.py > isa_diff.c
#   powerpc64le-linux-gnu-gcc -O1 -static -mcpu=power8 -o isa_diff isa_diff.c
out = []
w = out.append
w('''#include <stdio.h>
#include <stdint.h>
#include <string.h>
static uint64_t s = 0x9e3779b97f4a7c15ULL;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static unsigned char buf[256] __attribute__((aligned(16)));
static uint64_t h(void) { uint64_t x = 1469598103934665603ULL; for (int i = 0; i < 256; i++) { x ^= buf[i]; x *= 1099511628211ULL; } return x; }
#define P(name, r, b) printf("%s %016llx %016llx %016llx\\n", name, (unsigned long long) (r), (unsigned long long) (b), (unsigned long long) h())
int main(void) {
  for (int it = 0; it < 32; it++) {
    for (int i = 0; i < 256; i++) buf[i] = (unsigned char) rnd();
    uint64_t v = rnd(), r, base;
    long off = (long) (rnd() % 200) & ~0L;
''')
loads = [('lbz', 'D'), ('lhz', 'D'), ('lha', 'D'), ('lwz', 'D'), ('lwa', 'DS'), ('ld', 'DS')]
for op, form in loads:
    d = 8 if form == 'DS' else 7
    w('    base = (uint64_t) buf + (off & ~7); __asm__ volatile("%s %%0,%d(%%1)" : "=r"(r) : "b"(base) : "memory"); P("%s", r, 0);\n' % (op, d, op))
    if op != 'lwa':
        w('    base = (uint64_t) buf + (off & ~7); __asm__ volatile("%su %%0,%d(%%1)" : "=r"(r), "+b"(base) : : "memory"); P("%su", r, base - (uint64_t) buf);\n' % (op, d, op))
for op in ['lbzx', 'lhzx', 'lhax', 'lwzx', 'lwax', 'ldx', 'lhbrx', 'lwbrx', 'ldbrx']:
    w('    base = (uint64_t) buf; __asm__ volatile("%s %%0,%%1,%%2" : "=r"(r) : "b"(base), "r"(off) : "memory"); P("%s", r, 0);\n' % (op, op))
for op in ['lbzux', 'lhzux', 'lhaux', 'lwzux', 'lwaux', 'ldux']:
    w('    base = (uint64_t) buf; __asm__ volatile("%s %%0,%%1,%%2" : "=r"(r), "+b"(base) : "r"(off) : "memory"); P("%s", r, base - (uint64_t) buf);\n' % (op, op))
for op, d in [('stb', 3), ('sth', 5), ('stw', 6), ('std', 8)]:
    w('    base = (uint64_t) buf + (off & ~7); __asm__ volatile("%s %%1,%d(%%0)" : : "b"(base), "r"(v) : "memory"); P("%s", 0, 0);\n' % (op, d, op))
    w('    base = (uint64_t) buf + (off & ~7); __asm__ volatile("%su %%1,%d(%%0)" : "+b"(base) : "r"(v) : "memory"); P("%su", 0, base - (uint64_t) buf);\n' % (op, d, op))
for op in ['stbx', 'sthx', 'stwx', 'stdx', 'sthbrx', 'stwbrx', 'stdbrx']:
    w('    base = (uint64_t) buf; __asm__ volatile("%s %%1,%%0,%%2" : : "b"(base), "r"(v), "r"(off) : "memory"); P("%s", 0, 0);\n' % (op, op))
for op in ['stbux', 'sthux', 'stwux', 'stdux']:
    w('    base = (uint64_t) buf; __asm__ volatile("%s %%1,%%0,%%2" : "+b"(base) : "r"(v), "r"(off) : "memory"); P("%s", 0, base - (uint64_t) buf);\n' % (op, op))
# larx/stcx
for l, s, al in [('lwarx', 'stwcx.', 3), ('ldarx', 'stdcx.', 7), ('lbarx', 'stbcx.', 0), ('lharx', 'sthcx.', 1)]:
    w('    { uint64_t a = (uint64_t) buf + (off & ~%d), cr; __asm__ volatile("1: %s %%0,0,%%2\\n\\tadd %%0,%%0,%%3\\n\\t%s %%0,0,%%2\\n\\tbne- 1b\\n\\tmfcr %%1" : "=&r"(r), "=&r"(cr) : "b"(a), "r"(v) : "memory", "cr0"); P("%s", r, cr >> 28); }\n' % (al, l, s, l))
# CR logic, mtcrf, mfocrf
for op in ['crand', 'cror', 'crxor', 'crnand', 'crnor', 'creqv', 'crandc', 'crorc']:
    w('    { uint64_t cr = v; __asm__ volatile("mtcrf 0xff,%%1\\n\\t%s 9,14,31\\n\\t%s 0,5,6\\n\\tmfcr %%0" : "=r"(r) : "r"(cr) : "cr0", "cr2", "cr3", "cr7"); P("%s", r, 0); }\n' % (op, op, op))
w('    { __asm__ volatile("mtcrf 0x30,%1\\n\\tmcrf 2,3\\n\\tmfcr %0" : "=r"(r) : "r"(v) : "cr2", "cr3"); P("mtcrf+mcrf", r, 0); }\n')
w('    { __asm__ volatile("mtocrf 0x08,%1\\n\\tmfocrf %0,0x08" : "=r"(r) : "r"(v) : "cr4"); P("mtocrf", r & 0x0000f000, 0); }\n')
# branches: bdnz, bc on cr bits, bclr/bcctr conditional
w('    { uint64_t n = (v & 63) + 1, c = 0; __asm__ volatile("mtctr %2\\n1: addi %0,%0,3\\n\\tbdnz 1b" : "+r"(c) : "0"(c), "r"(n) : "ctr"); P("bdnz", c, 0); }\n')
w('    { uint64_t c = 0, n = (v & 7) + 1; __asm__ volatile("mtctr %2\\n\\tcmpdi 6,%2,4\\n1: addi %0,%0,1\\n\\tbdnzf 26,1b" : "+r"(c) : "0"(c), "r"(n) : "ctr", "cr6"); P("bdnzf", c, 0); }\n')
w('    { uint64_t c; __asm__ volatile("cmpd 7,%1,%2\\n\\tli %0,1\\n\\tbgt 7,1f\\n\\tli %0,2\\n\\tblt 7,1f\\n\\tli %0,3\\n1:" : "=&r"(c) : "r"(v), "r"(rnd()) : "cr7"); P("bc", c, 0); }\n')
w('    { uint64_t c; __asm__ volatile("bl 2f\\n2: mflr %0\\n\\tcmpdi 7,%1,0\\n\\taddi %0,%0,24\\n\\tmtctr %0\\n\\tli %0,5\\n\\tbltctr 7\\n\\tli %0,6\\n\\tnop\\n" : "=&r"(c) : "r"(v) : "cr7", "ctr", "lr"); P("bltctr", c, 0); }\n')
# FP
FP3 = ['fadd', 'fsub', 'fmul', 'fdiv', 'fadds', 'fsubs', 'fmuls', 'fdivs', 'fcpsgn']
FP1 = ['fsqrt', 'fsqrts', 'frsp', 'fctiw', 'fctiwz', 'fctid', 'fctidz', 'fctiwu', 'fctiwuz', 'fctidu', 'fctiduz',
       'fcfid', 'fcfidu', 'fcfids', 'fcfidus', 'fneg', 'fabs', 'fnabs', 'fmr', 'frin', 'friz', 'frip', 'frim']
FMA = ['fmadd', 'fmsub', 'fnmadd', 'fnmsub', 'fmadds', 'fmsubs', 'fnmadds', 'fnmsubs', 'fsel']
w('''    { double a, b, c, d; uint64_t ra = rnd(), rb = rnd(), rc = rnd(), fs;
      int k = it % 4;
      a = k == 0 ? (double) (int64_t) ra / 7.0 : k == 1 ? (double) (int32_t) ra : k == 2 ? -1.5 : 1e300;
      b = k == 0 ? (double) (int64_t) rb / 3.0 : k == 1 ? 0.5 : k == 2 ? (double) (int16_t) rb : 1e-300;
      c = (double) (int32_t) rc / 11.0;
      uint64_t rn = it & 3;
      __asm__ volatile("mtfsfi 7,0\\n\\tmtfsf 0xff,%0" : : "d"(*(double *) &(uint64_t){rn}));
''')
for op in FP3:
    w('      __asm__ volatile("%s %%0,%%1,%%2" : "=d"(d) : "d"(a), "d"(b)); P("%s", *(uint64_t *) &d, 0);\n' % (op, op))
for op in FP1:
    w('      __asm__ volatile("%s %%0,%%1" : "=d"(d) : "d"(a)); P("%s", *(uint64_t *) &d, 0);\n' % (op, op))
    w('      __asm__ volatile("%s %%0,%%1" : "=d"(d) : "d"(c)); P("%s.c", *(uint64_t *) &d, 0);\n' % (op, op))
for op in FMA:
    w('      __asm__ volatile("%s %%0,%%1,%%2,%%3" : "=d"(d) : "d"(a), "d"(c), "d"(b)); P("%s", *(uint64_t *) &d, 0);\n' % (op, op))
w('''      { uint64_t cr; __asm__ volatile("fcmpu 3,%1,%2\\n\\tmfcr %0" : "=r"(cr) : "d"(a), "d"(b) : "cr3"); P("fcmpu", (cr >> 16) & 15, 0); }
      { uint64_t cr; __asm__ volatile("fcmpu 3,%1,%1\\n\\tmfcr %0" : "=r"(cr) : "d"(0.0 / 0.0) : "cr3"); P("fcmpu-nan", (cr >> 16) & 15, 0); }
      __asm__ volatile("mffs %0" : "=d"(d)); fs = *(uint64_t *) &d; P("mffs", fs & 0xfe0000ff, 0);
      __asm__ volatile("mtfsb1 31\\n\\tmtfsb0 30\\n\\tmffs %0" : "=d"(d)); fs = *(uint64_t *) &d; P("mtfsb", fs & 0xff, 0);
      __asm__ volatile("mtfsfi 7,0");
    }
  }
  return 0;
}
''')
print(''.join(out))
