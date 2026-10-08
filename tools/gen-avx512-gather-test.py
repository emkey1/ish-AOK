#!/usr/bin/env python3
# Generates tests/manual/x86/amd64_avx512_gather.c, or with --i386
# i386_avx512_gather.c (usage: this script [--i386] [--answers FILE] OUT.c).
#
# AVX-512 gathers and scatters (VPGATHERDD/DQ/QD/QQ, VGATHERDPS/DPD/QPS/QPD,
# VPSCATTER*, VSCATTER*): VL 128/256/512, every scale, disp8*N and disp32,
# random opmasks and signed indices around a base in the middle of a random
# buffer (and, one case in sixteen, indices 0-7: overlapping scatters),
# plus (amd64) a form per op with the registers 16-31 (EVEX.V' as the
# index's top bit), and encodings whose #UD-or-not is part of the answer:
# the data register, k1, the buffer and the signal hashed per form and
# checked against Intel SDE's (`sde64 -ptr-raise -spr -- ./test hashes` on
# camd, passed back with --answers); the i386 test takes the amd64 answers.
#
# Then the faults, checked by the harness against the SDM: element 3 on an
# unmapped page. The SIGSEGV handler records the memory, changes every byte
# of the mapped page, maps the other and returns, so the instruction
# restarts. The SDM has every element below the faulting one completed,
# its opmask bit cleared, when the fault is delivered: a gather's keep the
# old bytes, a scatter's show in the record; AOK does none above it before
# the fault (the SDM leaves those open). SDE does not: it keeps nothing
# across the fault (its gathers re-read elements 0-2 after the restart,
# its scatters have written nothing at the fault), so "sde" as argv[1]
# skips these.
import sys
I386 = '--i386' in sys.argv
args = [a for a in sys.argv[1:] if a != '--i386']
answers = {}
if '--answers' in args:
    i = args.index('--answers')
    for line in open(args[i + 1]):
        k, v = line.rsplit(' ', 1)
        answers[k] = v.strip()
    del args[i:i + 2]
NAME = 'i386_avx512_gather' if I386 else 'amd64_avx512_gather'
VLS = {128: 'xmm', 256: 'ymm', 512: 'zmm'}
HALF = {128: 'xmm', 256: 'xmm', 512: 'ymm'}
OPS = [('vpgatherdd', 4, 4), ('vpgatherdq', 4, 8), ('vpgatherqd', 8, 4), ('vpgatherqq', 8, 8),
       ('vgatherdps', 4, 4), ('vgatherdpd', 4, 8), ('vgatherqps', 8, 4), ('vgatherqpd', 8, 8),
       ('vpscatterdd', 4, 4), ('vpscatterdq', 4, 8), ('vpscatterqd', 8, 4), ('vpscatterqq', 8, 8),
       ('vscatterdps', 4, 4), ('vscatterdpd', 4, 8), ('vscatterqps', 8, 4), ('vscatterqpd', 8, 8)]
F = []   # (name, asm, index size, 0)
def regs(L, isz, esz):
    """the data and index registers' names at VL L"""
    if isz == esz:
        return VLS[L], VLS[L]
    if isz == 4:                                # dword indices, qword data
        return VLS[L], HALF[L]
    return HALF[L], VLS[L]                      # qword indices, dword data
for op, isz, esz in OPS:
    scatter = 'scatter' in op
    for L in VLS:
        d, x = regs(L, isz, esz)
        for scale in (1, 2, 4, 8):
            for disp in (esz * 2, -1032):
                mem = f'{disp}(%1,%%{x}2,{scale})'
                asm = f'{op} %%{d}1, {mem}%{{%%k1%}}' if scatter else f'{op} {mem}, %%{d}1%{{%%k1%}}'
                F.append((f'{op} {L} {scale} {disp}', asm, isz, 0))
    if not I386:                                # data zmm25, index zmm18
        d, x = regs(512, isz, esz)
        mem = f'{esz}(%1,%%{x}18,8)'
        body = f'{op} %%{d}25, {mem}%{{%%k1%}}' if scatter else f'{op} {mem}, %%{d}25%{{%%k1%}}'
        F.append((f'{op} 512 hi', f'vmovdqa64 %%zmm1, %%zmm25\\n vmovdqa64 %%zmm2, %%zmm18\\n {body}\\n '
                  'vmovdqa64 %%zmm25, %%zmm1', isz, 0))
# the fault forms (checked by the harness, not hashed): base = the mapped
# page, element j at base + 8j but element 3 at base + 4096 + 16
FL = []
for op, isz, esz in (('vpgatherdd', 4, 4), ('vpgatherqq', 8, 8), ('vpgatherqd', 8, 4), ('vpgatherdq', 4, 8),
                     ('vpscatterdd', 4, 4), ('vpscatterqq', 8, 8), ('vpscatterqd', 8, 4), ('vpscatterdq', 4, 8)):
    for L in (128, 256, 512):
        d, x = regs(L, isz, esz)
        mem = f'(%1,%%{x}2,1)'
        asm = f'{op} %%{d}1, {mem}%{{%%k1%}}' if 'scatter' in op else f'{op} {mem}, %%{d}1%{{%%k1%}}'
        FL.append((f'{op} {L}', asm, isz, esz, L, int('scatter' in op)))
# 62 P0 P1 P2 op ModRM SIB: vpgatherdd zmm1{k1}, [rax + zmm2*4] is 62 f2 7d 49 90 0c 90
UD = {'vpgatherdd ok': '62 f2 7d 49 90 0c 90', 'vpgatherdd k0': '62 f2 7d 48 90 0c 90',
      'vpgatherdd z': '62 f2 7d c9 90 0c 90', 'vpgatherdd dst=index': '62 f2 7d 49 90 14 90',
      'vpgatherdd no sib': '62 f2 7d 49 90 08', 'vpgatherdd reg': '62 f2 7d 49 90 ca',
      'vpgatherdd vvvv': '62 f2 75 49 90 0c 90', 'vpgatherdd b': '62 f2 7d 59 90 0c 90',
      'vpgatherdd LL3': '62 f2 7d 69 90 0c 90', 'vpscatterdd ok': '62 f2 7d 49 a0 0c 90',
      'vpscatterdd k0': '62 f2 7d 48 a0 0c 90', 'vpscatterdd src=index': '62 f2 7d 49 a0 14 90',
      'vpscatterdd z': '62 f2 7d c9 a0 0c 90', 'vpgatherqq ok': '62 f2 fd 49 91 0c 90'}
for k, b in UD.items():
    F.append((f'ud {k}', 'ud:' + ', '.join('0x' + x for x in b.split()), 8 if 'qq' in k else 4, 0))
R = 'e' if I386 else 'r'
o = []; w = o.append
w(f'''// Generated by tools/gen-avx512-gather-test.py{" --i386" if I386 else ""} -- do not edit.
//
// AVX-512 gathers and scatters against Intel SDE (see the generator). (No
// target pragma: the harness itself must not use AVX-512.)
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static uint64_t rs;
static uint64_t rnd(void) {{ rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }}
struct st {{ uint8_t d[64], x[64], out[64]; uint64_t k, kout; }} __attribute__((aligned(64)));
static uint8_t buf[24576] __attribute__((aligned(64)));
static uint8_t rec[128];                        /* a faulting scatter's writes at the fault */
static uint8_t *pg;                             /* the fault forms' pages: pg mapped, pg + 4096 not */
static volatile int restart, faults;
static sigjmp_buf jb;
static volatile int sig;
static void handler(int s) {{
    if (s == SIGSEGV && restart && faults++ < 4) {{
        memcpy(rec, pg, sizeof rec);
        for (int i = 0; i < 4096; i++) pg[i] ^= 0xff;
        mprotect(pg + 4096, 4096, PROT_READ | PROT_WRITE);
        for (int i = 0; i < 64; i++) pg[4096 + i] = (uint8_t) (0xc0 + i);
        return;                                 /* the instruction restarts */
    }}
    sig = s;
    siglongjmp(jb, 1);
}}
#define LOAD "vmovdqu64 (%0), %%zmm1\\n vmovdqu64 64(%0), %%zmm2\\n kmovq 192(%0), %%k1\\n"
#define SAVE "\\n vmovdqu64 %%zmm1, 128(%0)\\n kmovq %%k1, 200(%0)"
''')
for i, (name, asm, isz, esz, L, sc) in enumerate(FL):
    w(f'''__attribute__((noinline)) static void g{i}(struct st *t, uint8_t *base) {{
    __asm__ volatile(LOAD "{asm}" SAVE :: "r"(t), "r"(base) : "memory", "xmm1", "xmm2");
}}''')
w('static const struct { const char *name; void (*fn)(struct st *, uint8_t *); int isz, dsz, vl, scatter; } fl[] = {')
for i, (name, asm, isz, esz, L, sc) in enumerate(FL):
    w(f'    {{"{name}", g{i}, {isz}, {esz}, {L}, {sc}}},')
w('};')
w(f'#define NFL {len(FL)}')
for i, (name, asm, isz, fault) in enumerate(F):
    if asm.startswith('ud:'):
        w(f'''__attribute__((noinline)) static void f{i}(struct st *t, uint8_t *base) {{
    __asm__ volatile(LOAD "mov %1, %%{R}ax\\n .byte {asm[3:]}" SAVE :: "r"(t), "r"(base) : "memory", "{R}ax", "xmm1", "xmm2");
}}''')
    else:
        w(f'''__attribute__((noinline)) static void f{i}(struct st *t, uint8_t *base) {{
    __asm__ volatile(LOAD "{asm}" SAVE :: "r"(t), "r"(base) : "memory", "xmm1", "xmm2");
}}''')
w('static const struct { const char *name; void (*fn)(struct st *, uint8_t *); int isz, unused; } forms[] = {')
for i, (name, asm, isz, fault) in enumerate(F):
    w(f'    {{"{name}", f{i}, {isz}, {fault}}},')
w('};')
w(f'#define NF {len(F)}')
if answers:
    w('static const uint64_t want[NF] = {')
    for f in F:
        w(f'    {answers[f[0]]}ull,')
    w('};')
else:
    w('static const uint64_t want[NF];   // (run with "hashes" under SDE)')
w(r'''
static int run(void (*fn)(struct st *, uint8_t *), struct st *t, uint8_t *base) {
    sig = 0;
    if (!sigsetjmp(jb, 1))
        fn(t, base);
    return sig;
}
static void put_index(struct st *t, int isz, int j, int64_t v) {
    if (isz == 4) { int32_t d = (int32_t) v; memcpy(t->x + 4 * j, &d, 4); }
    else memcpy(t->x + 8 * j, &v, 8);
}

static int under_sde;
static unsigned long fault_checks(unsigned long *checks) {
    static struct st t;
    static uint8_t orig[128], emem[128], erec[128], eb[64], eout[64];
    unsigned long bad = 0;
    if (under_sde)
        return 0;
    for (int fi = 0; fi < NFL; fi++) {
        int isz = fl[fi].isz, dsz = fl[fi].dsz, sc = fl[fi].scatter;
        int n = fl[fi].vl / 8 / (isz > dsz ? isz : dsz);
        rs = 0x9e3779b97f4a7c15ull;
        for (const char *c = fl[fi].name; *c; c++) rs = (rs ^ (uint8_t) *c) * 0x100000001b3ull;
        for (int k = 0; k < 16; k++) {
            for (int i = 0; i < 64; i += 8) {
                uint64_t r = rnd();
                memcpy(t.d + i, &r, 8);
            }
            for (int j = 0; j < 64 / isz; j++)
                put_index(&t, isz, j, j == 3 ? 4096 + 16 : 8 * j);
            t.k = rnd();
            if (k == 2) t.k = 0;
            if (k == 3) t.k = ~0ull;
            t.kout = 0;
            memset(t.out, 0, 64);
            for (int i = 0; i < 4096; i += 8) {
                uint64_t r = rnd();
                memcpy(pg + i, &r, 8);
            }
            mprotect(pg + 4096, 4096, PROT_READ | PROT_WRITE);
            for (int i = 0; i < 64; i++) pg[4096 + i] = (uint8_t) (0xc0 + i);
            mprotect(pg + 4096, 4096, PROT_NONE);
            memcpy(orig, pg, 128);
            memset(rec, 0, sizeof rec);
            faults = 0;
            restart = 1;
            int sg = run(fl[fi].fn, &t, pg);
            restart = 0;
            mprotect(pg + 4096, 4096, PROT_READ | PROT_WRITE);
            /* the SDM's answer, AOK's order: element by element from the lowest */
            uint64_t en = t.k & ((1ull << n) - 1);
            int f = n > 3 && (en >> 3 & 1);
            for (int i = 0; i < 64; i++) eb[i] = (uint8_t) (0xc0 + i);
            memcpy(emem, orig, 128);
            memset(erec, 0, sizeof erec);
            memcpy(eout, t.d, 64);
            if (sc) {
                for (int j = 0; j < n; j++)
                    if ((en >> j & 1) && (!f || j < 3))
                        memcpy(emem + 8 * j, t.d + dsz * j, dsz);
                if (f) {
                    memcpy(erec, emem, 128);
                    for (int i = 0; i < 128; i++) emem[i] ^= 0xff;
                    for (int j = 3; j < n; j++)
                        if (en >> j & 1)
                            memcpy(j == 3 ? eb + 16 : emem + 8 * j, t.d + dsz * j, dsz);
                }
            } else {
                for (int j = 0; j < n; j++) {
                    if (!(en >> j & 1))
                        continue;
                    uint8_t v[8];
                    if (!f || j < 3) memcpy(v, orig + 8 * j, 8);
                    else if (j == 3) memcpy(v, eb + 16, 8);
                    else for (int i = 0; i < 8; i++) v[i] = orig[8 * j + i] ^ 0xff;
                    memcpy(eout + dsz * j, v, dsz);
                }
                memset(eout + n * dsz, 0, 64 - n * dsz);
                if (f) {
                    memcpy(erec, orig, 128);
                    for (int i = 0; i < 128; i++) emem[i] ^= 0xff;
                }
            }
            (*checks)++;
            const char *what = sg ? "a signal" : faults != f ? "the fault count" : t.kout ? "k1 left set" :
                    memcmp(t.out, eout, 64) ? "the data register" : memcmp(rec, erec, 128) ? "the memory at the fault" :
                    memcmp(pg, emem, 128) ? "the memory" : memcmp(pg + 4096, eb, 64) ? "the second page" : NULL;
            if (what && bad++ < 40)
                printf("FAIL fault %s %d (k %#llx): %s wrong (signal %d, faults %d)\n", fl[fi].name, k,
                       (unsigned long long) t.k, what, sg, faults);
        }
    }
    return bad;
}

int main(int argc, char **argv) {
    int print = argc > 1 && !strcmp(argv[1], "hashes"), dump = argc > 1 && !strcmp(argv[1], "dump");
    under_sde = argc > 1 && !strcmp(argv[1], "sde");
    unsigned long checks = 0, bad = 0;
    static struct st t;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGILL, &sa, 0);
    sigaction(SIGSEGV, &sa, 0);
    pg = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (int fi = 0; fi < NF; fi++) {
        uint64_t h = 0xcbf29ce484222325ull;
        rs = 0x9e3779b97f4a7c15ull;
        for (const char *c = forms[fi].name; *c; c++) rs = (rs ^ (uint8_t) *c) * 0x100000001b3ull;
        int ud = !strncmp(forms[fi].name, "ud ", 3), isz = forms[fi].isz;
        for (int k = 0; k < (ud ? 1 : 16); k++) {
            for (int i = 0; i < 64; i += 8) {
                uint64_t r = rnd();
                memcpy(t.d + i, &r, 8);
            }
            for (int j = 0; j < 64 / isz; j++)       /* indices -1024..1023, or 0..7 */
                put_index(&t, isz, j, k == 5 ? (int64_t) (rnd() & 7) : (int64_t) (rnd() & 2047) - 1024);
            for (unsigned i = 0; i < sizeof buf; i += 8) {
                uint64_t r = rnd();
                memcpy(buf + i, &r, 8);
            }
            t.k = rnd();
            if (k == 2) t.k = 0;
            if (k == 3) t.k = ~0ull;
            t.kout = 0;
            memset(t.out, 0, 64);
            memset(rec, 0, sizeof rec);
            int sg = run(forms[fi].fn, &t, buf + 12288);
            for (int i = 0; i < 64; i++) h = (h ^ t.out[i]) * 0x100000001b3ull;
            for (int b = 0; b < 8; b++) h = (h ^ ((t.kout >> (8 * b)) & 0xff)) * 0x100000001b3ull;
            for (unsigned i = 0; i < sizeof buf; i++) h = (h ^ buf[i]) * 0x100000001b3ull;
            h = (h ^ (unsigned) sg) * 0x100000001b3ull;
            if (dump) {
                uint64_t bh = 0xcbf29ce484222325ull;
                for (unsigned i = 0; i < sizeof buf; i++) bh = (bh ^ buf[i]) * 0x100000001b3ull;
                printf("%s %d: sig %d k1 %016llx buf %016llx out ", forms[fi].name, k, sg,
                       (unsigned long long) t.kout, (unsigned long long) bh);
                for (int i = 63; i >= 0; i--) printf("%02x", t.out[i]);
                printf("\n");
            }
        }
        if (print) printf("%s %#llx\n", forms[fi].name, (unsigned long long) h);
        checks++;
        if (!print && !dump && h != want[fi] && bad++ < 40)
            printf("FAIL %s\n", forms[fi].name);
    }
    if (!print && !dump)
        bad += fault_checks(&checks);
    if (!print && !dump)
        printf("NAME: %s (%d forms, %lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", NF, checks, bad);
    return bad != 0;
}''')
open(args[0], 'w').write('\n'.join(o).replace('NAME: %s', NAME + ': %s'))
print(len(F), 'forms')
