#!/usr/bin/env python3
# Writes a C program that runs ppc64 integer instructions on fixed
# pseudo-random operands and prints each result with XER and CR after it, for
# a diff between an oracle (qemu-ppc64le, a POWER machine) and the AOK guest.
#   tests/ppc64/gen_alu_diff.py > alu_diff.c
#   powerpc64le-linux-gnu-gcc -O1 -static -o alu_diff alu_diff.c
import itertools

RR = ['add', 'subf', 'addc', 'adde', 'subfc', 'subfe', 'mulld', 'mulhd', 'mulhdu',
      'mullw', 'mulhw', 'mulhwu', 'divd', 'divdu', 'divw', 'divwu', 'and', 'or', 'xor',
      'nand', 'nor', 'eqv', 'andc', 'orc', 'sld', 'srd', 'slw', 'srw', 'srad', 'sraw',
      'cmpb']
R1 = ['neg', 'addze', 'addme', 'subfze', 'subfme', 'extsb', 'extsh', 'extsw',
      'cntlzw', 'cntlzd', 'popcntb', 'popcntw', 'popcntd']
RECORD = ['add', 'subf', 'and', 'or', 'xor', 'nor', 'neg', 'extsw', 'mulld', 'srad', 'sraw']
IMM = [('addic', [-1, 1, 0x7fff, -0x8000]), ('subfic', [-1, 0, 0x7fff, -0x8000]),
       ('mulli', [3, -7, 0x7fff]), ('sradi', [0, 1, 31, 32, 63]), ('srawi', [0, 1, 15, 31])]
ROTW = [(sh, mb, me) for sh in (0, 1, 7, 31) for mb in (0, 5, 31) for me in (0, 26, 31)]
ROTD = [(sh, mb) for sh in (0, 1, 31, 32, 63) for mb in (0, 1, 32, 63)]

out = []
out.append('#include <stdio.h>\n#include <stdint.h>\n')
out.append('static uint64_t s = 0x9e3779b97f4a7c15ULL;\n'
           'static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; '
           'switch (s % 8) { case 0: return 0; case 1: return (uint64_t) -1; '
           'case 2: return 0x80000000; case 3: return 0x7fffffffffffffffULL; '
           'case 4: return s & 0xff; default: return s; } }\n')
out.append('#define SHOW(name) printf("%s %016llx %016llx -> %016llx xer %08llx cr %08llx\\n", name, '
           '(unsigned long long) a, (unsigned long long) b, (unsigned long long) r, '
           '(unsigned long long) (xer & 0xe0000000), (unsigned long long) cr)\n')
out.append('int main(void) {\n  for (int i = 0; i < 64; i++) {\n'
           '    uint64_t a = rnd(), b = rnd(), r, xer, cr, ca = rnd() & 1;\n')

def body(asm, name):
    # set CA from ca, clear SO/OV, run, read XER and CR
    return ('    { uint64_t x = ca << 29; __asm__ volatile("mtxer %%3\\n\\t" "%s\\n\\t" '
            '"mfxer %%1\\n\\t" "mfcr %%2" : "=&r"(r), "=&r"(xer), "=&r"(cr) : "r"(x), "r"(a), "r"(b) '
            ': "xer", "cr0", "cr1", "cr7"); SHOW("%s"); }\n' % (asm, name))

for op in RR:
    out.append(body('%s %%0,%%4,%%5' % op, op))
for op in RECORD:
    if op in R1:
        out.append(body('%s. %%0,%%4' % op, op + '.'))
    else:
        out.append(body('%s. %%0,%%4,%%5' % op, op + '.'))
for op in R1:
    out.append(body('%s %%0,%%4' % op, op))
for op, imms in IMM:
    for i in imms:
        out.append(body('%s %%0,%%4,%d' % (op, i), '%s,%d' % (op, i)))
for sh, mb, me in ROTW:
    out.append(body('rlwinm %%0,%%4,%d,%d,%d' % (sh, mb, me), 'rlwinm,%d,%d,%d' % (sh, mb, me)))
    out.append(body('mr %%0,%%5\\n\\trlwimi %%0,%%4,%d,%d,%d' % (sh, mb, me), 'rlwimi,%d,%d,%d' % (sh, mb, me)))
    out.append(body('rlwnm %%0,%%4,%%5,%d,%d' % (mb, me), 'rlwnm,%d,%d' % (mb, me)))
for sh, mb in ROTD:
    out.append(body('rldicl %%0,%%4,%d,%d' % (sh, mb), 'rldicl,%d,%d' % (sh, mb)))
    out.append(body('rldicr %%0,%%4,%d,%d' % (sh, mb), 'rldicr,%d,%d' % (sh, mb)))
    out.append(body('rldic %%0,%%4,%d,%d' % (sh, mb), 'rldic,%d,%d' % (sh, mb)))
    out.append(body('mr %%0,%%5\\n\\trldimi %%0,%%4,%d,%d' % (sh, mb), 'rldimi,%d,%d' % (sh, mb)))
    out.append(body('rldcl %%0,%%4,%%5,%d' % mb, 'rldcl,%d' % mb))
    out.append(body('rldcr %%0,%%4,%%5,%d' % mb, 'rldcr,%d' % mb))
for cmp in ['cmpd', 'cmpw', 'cmpld', 'cmplw']:
    out.append(body('li %%0,0\\n\\t%s 7,%%4,%%5' % cmp, cmp))
for cmp, imm in [('cmpdi', -5), ('cmpwi', 0), ('cmpldi', 0x8000), ('cmplwi', 255)]:
    out.append(body('li %%0,0\\n\\t%s 7,%%4,%d' % (cmp, imm), '%s,%d' % (cmp, imm)))
out.append(body('cmpd 7,%4,%5\\n\\tisel %0,%4,%5,29', 'isel'))
out.append(body('cmpd 1,%4,%5\\n\\tcrxor 5,4,5\\n\\tmfcr %0', 'crxor'))
out.append('  }\n  return 0;\n}\n')
print(''.join(out))
