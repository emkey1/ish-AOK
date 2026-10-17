#!/usr/bin/env python3
# Every shift amount and mask of the rotate and shift-immediate instructions,
# with their record forms, on a few operands: a diff between an oracle and the
# AOK guest (see gen_alu_diff.py). Big (some 10,000 asm statements); build it
# with -O0.
out = []
w = out.append
w('#include <stdio.h>\n#include <stdint.h>\n')
w('static const uint64_t V[4] = {0x0123456789abcdefULL, 0xfedcba9876543210ULL, 0x80000001ffff7fffULL, 0x00000000deadbeefULL};\n')
w('#define T(name, asm_) for (int i = 0; i < 4; i++) { uint64_t a = V[i], b = V[(i + 1) & 3], r, cr, xer; '
  '__asm__ volatile("mtxer %4\\n\\tmr %0,%3\\n\\t" asm_ "\\n\\tmfcr %1\\n\\tmfxer %2" : "=&r"(r), "=&r"(cr), "=&r"(xer) : "r"(b), "r"((uint64_t) 0), "r"(a) : "cr0", "xer"); '
  'printf("%s %d %016llx %08llx %llx\\n", name, i, (unsigned long long) r, (unsigned long long) (cr >> 28), (unsigned long long) (xer >> 29)); }\n')
w('int main(void) {\n')
for sh in range(32):
    for mb in range(32):
        for me in range(0, 32, 3):
            for op in ('rlwinm', 'rlwimi'):
                w('  T("%s %d %d %d", "%s. %%0,%%5,%d,%d,%d");\n' % (op, sh, mb, me, op, sh, mb, me))
for sh in range(64):
    for mb in range(0, 64, 3):
        for op in ('rldicl', 'rldicr', 'rldic', 'rldimi'):
            w('  T("%s %d %d", "%s. %%0,%%5,%d,%d");\n' % (op, sh, mb, op, sh, mb))
    w('  T("sradi %d", "sradi. %%0,%%5,%d");\n' % (sh, sh))
for sh in range(32):
    w('  T("srawi %d", "srawi. %%0,%%5,%d");\n' % (sh, sh))
for mb in range(32):
    w('  T("rlwnm %d", "rlwnm. %%0,%%5,%%3,%d,31");\n' % (mb, mb))
for mb in range(64):
    w('  T("rldcl %d", "rldcl. %%0,%%5,%%3,%d");\n' % (mb, mb))
    w('  T("rldcr %d", "rldcr. %%0,%%5,%%3,%d");\n' % (mb, mb))
w('  return 0;\n}\n')
print(''.join(out))
