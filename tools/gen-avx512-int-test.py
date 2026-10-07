#!/usr/bin/env python3
# Generates tests/manual/x86/amd64_avx512_int.c, or with --i386
# i386_avx512_int.c (usage: this script [--i386] [--answers FILE] OUT.c).
#
# AVX-512 (EVEX) moves and integer ops: every op at VL 128/256/512, register
# and memory (and {1toN} broadcast where the op has one) sources, unmasked,
# merge-masked and zero-masked, low registers and (amd64) zmm16-31 through
# EVEX.R'/V'/X; then fixed cases: a masked load and store whose masked-off
# elements lie on an unmapped page (no fault, memory past them untouched)
# with an enabled-element control (a fault, nothing written), and encodings
# whose #UD-or-not is part of the answer. The destination's 64 bytes, the
# memory a store wrote and the signal are hashed per form and checked
# against Intel SDE's (`sde64 -spr -- ./test hashes` on camd, passed back
# with --answers). Forms are keyed and seeded by name, so the i386 test
# (zmm0-7, no 16-31 forms) takes the amd64 answers. "dump" prints every case.
# The i386 test also checks 32-bit mode's own EVEX rules, as SDE gives them
# for a freestanding 32-bit probe on camd: EVEX.R', B and vvvv's bit 3 are
# ignored as register bits (the result is the normally encoded op's), the
# bit still counts where vvvv must be 1111b (#UD), and EVEX.V' is #UD.
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
NAME = 'i386_avx512_int' if I386 else 'amd64_avx512_int'
# op, bcast element bytes (0: none), unary
OPS = [('vpaddb', 0), ('vpaddw', 0), ('vpaddd', 4), ('vpaddq', 8), ('vpsubb', 0), ('vpsubw', 0), ('vpsubd', 4),
       ('vpsubq', 8), ('vpaddsb', 0), ('vpaddsw', 0), ('vpaddusb', 0), ('vpaddusw', 0), ('vpsubsb', 0), ('vpsubsw', 0),
       ('vpsubusb', 0), ('vpsubusw', 0), ('vpandd', 4), ('vpandq', 8), ('vpandnd', 4), ('vpandnq', 8), ('vpord', 4),
       ('vporq', 8), ('vpxord', 4), ('vpxorq', 8), ('vpminub', 0), ('vpmaxub', 0), ('vpminsw', 0), ('vpmaxsw', 0),
       ('vpavgb', 0), ('vpavgw', 0), ('vpmullw', 0), ('vpmulhw', 0), ('vpmulhuw', 0), ('vpmuludq', 8), ('vpmaddwd', 0),
       ('vpunpcklbw', 0), ('vpunpcklwd', 0), ('vpunpckldq', 4), ('vpunpcklqdq', 8), ('vpunpckhbw', 0),
       ('vpunpckhwd', 0), ('vpunpckhdq', 4), ('vpunpckhqdq', 8), ('vpacksswb', 0), ('vpackuswb', 0),
       ('vpackssdw', 4), ('vpshufb', 0), ('vpmaddubsw', 0), ('vpmulhrsw', 0), ('vpmuldq', 8), ('vpackusdw', 4),
       ('vpminsb', 0), ('vpminsd', 4), ('vpminuw', 0), ('vpminud', 4), ('vpmaxsb', 0), ('vpmaxsd', 4),
       ('vpmaxuw', 0), ('vpmaxud', 4), ('vpmulld', 4)]
UNARY = [('vpabsb', 0), ('vpabsw', 0), ('vpabsd', 4)]
MOVES = ['vmovdqa32', 'vmovdqa64', 'vmovdqu8', 'vmovdqu16', 'vmovdqu32', 'vmovdqu64', 'vmovups', 'vmovupd',
         'vmovaps', 'vmovapd']
VL = [(128, 'xmm'), (256, 'ymm'), (512, 'zmm')]
MASK = [('', ''), ('%{%%k1%}', 'm'), ('%{%%k1%}%{z%}', 'z')]
F = []  # (name, kind, asm); kind: op, st (store), ud
REGS = [('1', '2', '3')] + ([] if I386 else [('17', '25', '30')])
for op, bc in OPS + UNARY:
    un = (op, bc) in UNARY
    for L, x in VL:
        for mk, mn in MASK:
            for d, a, b in REGS:
                hi = '' if d == '1' else ' hi'
                src1 = '' if un else f', %%{x}{a}'
                F.append((f'{op} {L} reg{mn}{hi}', 'op', f'{op} %%{x}{b}{src1}, %%{x}{d}{mk}'))
            F.append((f'{op} {L} mem{mn}', 'op', f'{op} (%1){"" if un else f", %%{x}2"}, %%{x}1{mk}'))
            if bc:
                n = L // (8 * bc)
                F.append((f'{op} {L} bcst{mn}', 'op', f'{op} (%1)%{{1to{n}%}}{"" if un else f", %%{x}2"}, %%{x}1{mk}'))
for op in MOVES:
    for L, x in VL:
        for mk, mn in MASK:
            F.append((f'{op} {L} ld{mn}', 'op', f'{op} (%1), %%{x}1{mk}'))
            for d, a, b in REGS:
                hi = '' if d == '1' else ' hi'
                F.append((f'{op} {L} reg{mn}{hi}', 'op', f'{op} %%{x}{b}, %%{x}{d}{mk}'))
            if mn != 'z':
                F.append((f'{op} {L} st{mn}', 'st', f'{op} %%{x}3, (%1){mk}'))
UD = {   # 62 P0 P1 P2 op modrm (vpaddd zmm1, zmm2, zmm3 is 62 f1 6d 48 fe cb)
    'vpaddd ok': '62 f1 6d 48 fe cb', 'vpaddd W1': '62 f1 ed 48 fe cb', 'vpaddq W0': '62 f1 6d 48 d4 cb',
    'vpaddb W1': '62 f1 ed 48 fc cb', 'z no mask': '62 f1 6d c8 fe cb', 'LL 3': '62 f1 6d 68 fe cb',
    'b reg': '62 f1 6d 58 fe cb', 'b mem bytes': '62 f1 6d 58 fc 08', 'P1 bit2': '62 f1 69 48 fe cb',
    'P0 bit3': '62 f9 6d 48 fe cb', 'map0': '62 f0 6d 48 fe cb', 'vpsadbw mask': '62 f1 6d 49 f6 cb',
    'vmovdqu32 vvvv': '62 f1 7e 48 6f cb' .replace('7e', '76'), 'vmovdqu32 st z': '62 f1 7e c9 7f 08',
    'vmovdqa32 ok': '62 f1 7d 48 6f cb', 'vmovdqu8 W': '62 f1 7f 48 6f cb', 'vpabsd vvvv': '62 f2 75 48 1e cb',
}
if not I386:
    UD['V2 mem'] = '62 f1 35 40 fe 08'      # s1 = zmm25 (EVEX.V' set; no VSIB)
for k, b in UD.items():
    F.append((f'ud {k}', 'ud', ', '.join('0x' + x for x in b.split())))
o = []; w = o.append
R = 'e' if I386 else 'r'
w(f'''// Generated by tools/gen-avx512-int-test.py{" --i386" if I386 else ""} -- do not edit.
//
// AVX-512 moves and integer ops against Intel SDE (see the generator).
// (No target pragma: the harness itself must not use AVX-512.)
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static uint64_t rs;
static uint64_t rnd(void) {{ rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }}
// d: the destination's old value, a s1, b s2, m the memory operand (and a
// store's target, with guards around it), k the opmask, out the result
struct st {{ uint8_t d[64], a[64], b[64], out[64]; uint8_t pre[64], m[64], post[64]; uint64_t k; }} __attribute__((aligned(64)));
static sigjmp_buf jb;
static volatile int sig;
static void handler(int s) {{ sig = s; siglongjmp(jb, 1); }}
#define LOAD "vmovdqu64 (%0), %%zmm1\\n vmovdqu64 64(%0), %%zmm2\\n vmovdqu64 128(%0), %%zmm3\\n" \\
    "vmovdqu64 (%0), %%zmm17\\n vmovdqu64 64(%0), %%zmm25\\n vmovdqu64 128(%0), %%zmm30\\n kmovq 448(%0), %%k1\\n"
''' if not I386 else f'''// Generated by tools/gen-avx512-int-test.py --i386 -- do not edit.
//
// AVX-512 moves and integer ops against Intel SDE (see the generator).
// (No target pragma: the harness itself must not use AVX-512.)
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static uint64_t rs;
static uint64_t rnd(void) {{ rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }}
struct st {{ uint8_t d[64], a[64], b[64], out[64]; uint8_t pre[64], m[64], post[64]; uint64_t k; }} __attribute__((aligned(64)));
static sigjmp_buf jb;
static volatile int sig;
static void handler(int s) {{ sig = s; siglongjmp(jb, 1); }}
#define LOAD "vmovdqu64 (%0), %%zmm1\\n vmovdqu64 64(%0), %%zmm2\\n vmovdqu64 128(%0), %%zmm3\\n kmovq 448(%0), %%k1\\n"
''')
for i, (name, kind, asm) in enumerate(F):
    if kind == 'ud':
        w(f'''__attribute__((noinline)) static void f{i}(struct st *t) {{
    __asm__ volatile(LOAD "mov %1, %%{R}ax\\n .byte {asm}\\n vmovdqu64 %%zmm1, 192(%0)" :: "r"(t), "r"(t->m) : "memory", "{R}ax", "xmm1", "xmm2", "xmm3");
}}''')
        continue
    hi = ' hi' in name
    out = 'zmm17' if hi else 'zmm1'
    w(f'''__attribute__((noinline)) static void f{i}(struct st *t) {{
    __asm__ volatile(LOAD "{asm}\\n vmovdqu64 %%{out}, 192(%0)" :: "r"(t), "r"(t->m) : "memory", "xmm1", "xmm2", "xmm3");   /* (zmm16-31: gcc does not use them here) */
}}''')
w('static const struct { const char *name; void (*fn)(struct st *); } forms[] = {')
for i, (name, kind, asm) in enumerate(F):
    w(f'    {{"{name}", f{i}}},')
w('};')
w(f'#define NF {len(F)}')
if answers:
    w('static const uint64_t want[NF] = {')
    for name, kind, asm in F:
        w(f'    {answers[name]}ull,')
    w('};')
else:
    w('static const uint64_t want[NF];   // (run with "hashes" under SDE)')
w(r'''
static int run(void (*fn)(struct st *), struct st *t) {
    sig = 0;
    if (!sigsetjmp(jb, 1))
        fn(t);
    return sig;
}
// a masked load/store at the end of a mapped page, the next one unmapped:
// masked-off elements there must not fault; an enabled one must, with
// nothing written
static int masked_ld(const uint8_t *p, const uint64_t *k, uint8_t *out) {
    sig = 0;
    if (!sigsetjmp(jb, 1))
        __asm__ volatile("kmovq %1, %%k2\n vpxord %%zmm4, %%zmm4, %%zmm4\n vmovdqu32 (%0), %%zmm4%{%%k2%}%{z%}\n"
                         "vmovdqu64 %%zmm4, (%2)" :: "r"(p), "m"(*k), "r"(out) : "memory", "xmm4");
    return sig;
}
static int masked_st(uint8_t *p, const uint64_t *k, const uint8_t *src) {
    sig = 0;
    if (!sigsetjmp(jb, 1))
        __asm__ volatile("kmovq %1, %%k2\n vmovdqu64 (%2), %%zmm4\n vmovdqu32 %%zmm4, (%0)%{%%k2%}"
                         :: "r"(p), "m"(*k), "r"(src) : "memory", "xmm4");
    return sig;
}
#if defined(__i386__)
// 32-bit mode's EVEX register bits: (raw, the signal SDE gives, the same op
// encoded normally when it runs)
static int raw32(int which, uint8_t *out) {
    sig = 0;
    if (!sigsetjmp(jb, 1)) {
        __asm__ volatile("vmovdqu64 (%0), %%zmm1\n vmovdqu64 64(%0), %%zmm2\n vmovdqu64 128(%0), %%zmm3" :: "r"(out + 64) : "memory");
        switch (which) {
        case 0: __asm__ volatile(".byte 0x62, 0xe1, 0x6d, 0x48, 0xfe, 0xcb" ::: "memory"); break;   /* R' */
        case 1: __asm__ volatile(".byte 0x62, 0xd1, 0x6d, 0x48, 0xfe, 0xcb" ::: "memory"); break;   /* B clear */
        case 2: __asm__ volatile(".byte 0x62, 0xf1, 0x2d, 0x48, 0xfe, 0xcb" ::: "memory"); break;   /* vvvv 1010 */
        case 3: __asm__ volatile(".byte 0x62, 0xf1, 0x6d, 0x40, 0xfe, 0xcb" ::: "memory"); break;   /* V' */
        case 4: __asm__ volatile(".byte 0x62, 0xf1, 0x3e, 0x48, 0x6f, 0xcb" ::: "memory"); break;   /* vmovdqu32 vvvv 1000 */
        case 5: __asm__ volatile(".byte 0x62, 0xf2, 0x3d, 0x48, 0x1e, 0xcb" ::: "memory"); break;   /* vpabsd vvvv 1000 */
        case 6: __asm__ volatile(".byte 0x62, 0xe1, 0x7e, 0x48, 0x6f, 0xcb" ::: "memory"); break;   /* vmovdqu32 R' */
        default: __asm__ volatile("vpaddd %%zmm3, %%zmm2, %%zmm1\n" ::: "memory"); break;           /* the reference */
        }
        __asm__ volatile("vmovdqu64 %%zmm1, (%0)" :: "r"(out) : "memory");
    }
    return sig;
}
static unsigned long fixed32(unsigned long *checks) {
    static const int want[] = {0, 0, 0, SIGILL, SIGILL, SIGILL, 0};
    static const char *const names[] = {"R'", "B clear", "vvvv bit 3", "V'", "vmovdqu32 vvvv bit 3",
                                        "vpabsd vvvv bit 3", "vmovdqu32 R'"};
    static uint8_t buf[256] __attribute__((aligned(64))), ref[64];
    unsigned long bad = 0;
    for (int i = 0; i < 256; i++) buf[i] = (uint8_t) (i * 29 + 7);
    raw32(99, buf);
    memcpy(ref, buf, 64);
    for (int c = 0; c < 7; c++) {
        memset(buf, 0, 64);
        int sg = raw32(c, buf);
        (*checks)++;
        int same = c == 6 ? !memcmp(buf, buf + 192, 64) : !memcmp(buf, ref, 64);   /* (a move: zmm3) */
        if ((sg != want[c] || (!sg && !same)) && bad++ < 40)
            printf("FAIL 32-bit EVEX %s: signal %d (want %d)%s\n", names[c], sg, want[c], sg || same ? "" : ", a different result");
    }
    return bad;
}
#endif
static unsigned long fixed_checks(unsigned long *checks) {
    unsigned long bad = 0;
    uint8_t *pg = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(pg + 4096, 4096);
    uint8_t *p = pg + 4096 - 24;                 /* 24 bytes mapped: dwords 0-5 */
    for (int i = 0; i < 24; i++) p[i] = (uint8_t) (0x40 + i);
    static uint8_t out[64], src[64];
    for (int i = 0; i < 64; i++) src[i] = (uint8_t) (0x80 + i);
    static const uint64_t ks[2] = {0x003f, 0x007f};   /* dwords 0-5 (all mapped), then 0-6 (7th unmapped) */
    for (int c = 0; c < 2; c++) {
        uint64_t k = ks[c];
        memset(out, 0, 64);
        int sg = masked_ld(p, &k, out);
        (*checks)++;
        int want_sig = c ? SIGSEGV : 0;
        int okv = 1;
        if (!c) for (int i = 0; i < 64; i++) okv &= out[i] == (i < 24 ? (uint8_t) (0x40 + i) : 0);
        if ((sg != want_sig || !okv) && bad++ < 40)
            printf("FAIL masked load at a page end, mask %#llx: signal %d (want %d)%s\n", (unsigned long long) k, sg,
                   want_sig, okv ? "" : ", wrong data");
        for (int i = 0; i < 24; i++) p[i] = (uint8_t) (0x40 + i);
        sg = masked_st(p, &k, src);
        (*checks)++;
        okv = 1;
        for (int i = 0; i < 24; i++) okv &= p[i] == (c ? (uint8_t) (0x40 + i) : (uint8_t) (0x80 + i));
        if ((sg != want_sig || !okv) && bad++ < 40)
            printf("FAIL masked store at a page end, mask %#llx: signal %d (want %d)%s\n", (unsigned long long) k, sg,
                   want_sig, okv ? "" : ", memory wrong");
    }
    return bad;
}

int main(int argc, char **argv) {
    int print = argc > 1 && !strcmp(argv[1], "hashes"), dump = argc > 1 && !strcmp(argv[1], "dump");
    unsigned long checks = 0, bad = 0;
    static struct st t;
    signal(SIGILL, handler);
    signal(SIGSEGV, handler);
    for (int fi = 0; fi < NF; fi++) {
        uint64_t h = 0xcbf29ce484222325ull;
        rs = 0x9e3779b97f4a7c15ull;
        for (const char *c = forms[fi].name; *c; c++) rs = (rs ^ (uint8_t) *c) * 0x100000001b3ull;
        int ud = !strncmp(forms[fi].name, "ud ", 3);
        for (int k = 0; k < (ud ? 1 : 24); k++) {
            for (int i = 0; i < 64; i += 8) {
                uint64_t r;
                r = rnd(); memcpy(t.d + i, &r, 8);
                r = rnd(); memcpy(t.a + i, &r, 8);
                r = rnd(); memcpy(t.b + i, &r, 8);
                r = rnd(); memcpy(t.m + i, &r, 8);
                r = rnd(); memcpy(t.pre + i, &r, 8);
                r = rnd(); memcpy(t.post + i, &r, 8);
            }
            if ((k & 3) == 1) for (int i = 0; i < 64; i++) t.b[i] = t.m[i] = (uint8_t) (i & 1 ? 0x80 : 0x7f);  /* saturation */
            t.k = rnd();
            if (k == 2) t.k = 0;
            if (k == 3) t.k = ~0ull;
            memset(t.out, 0, 64);
            int sg = run(forms[fi].fn, &t);
            for (int i = 0; i < 64; i++) h = (h ^ t.out[i]) * 0x100000001b3ull;
            for (int i = 0; i < 64; i++) h = (h ^ t.m[i]) * 0x100000001b3ull;
            h = (h ^ (unsigned) sg) * 0x100000001b3ull;
            if (dump) {
                printf("%s %d: sig %d out ", forms[fi].name, k, sg);
                for (int i = 63; i >= 0; i--) printf("%02x", t.out[i]);
                printf(" m ");
                for (int i = 63; i >= 0; i--) printf("%02x", t.m[i]);
                printf("\n");
            }
        }
        if (print) printf("%s %#llx\n", forms[fi].name, (unsigned long long) h);
        checks++;
        if (!print && !dump && h != want[fi] && bad++ < 40)
            printf("FAIL %s\n", forms[fi].name);
    }
    if (!print && !dump)
        bad += fixed_checks(&checks);
#if defined(__i386__)
    if (!print && !dump)
        bad += fixed32(&checks);
#endif
    if (!print && !dump)
        printf("NAME: %s (%d forms, %lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", NF, checks, bad);
    return bad != 0;
}''')
open(args[0], 'w').write('\n'.join(o).replace('NAME: %s', NAME + ': %s'))
print(len(F), 'forms')
