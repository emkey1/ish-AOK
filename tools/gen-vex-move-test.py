#!/usr/bin/env python3
# Generates tests/manual/x86/amd64_vex_move.c, or with --i386
# i386_vex_move.c (usage: this script [--i386] OUT.c).
#
# The VEX data movement (AVX/AVX2): the loads and stores (VMOVUPS .. VMOVDQU,
# VLDDQU, VMOVNTDQA, VMOVNT*), VMOVSS/SD (load, merge, store), VMOVLPS/HPS/
# LPD/HPD and VMOVHLPS/LHPS, VMOVD/VMOVQ to and from general registers and
# memory, VZEROUPPER/VZEROALL, the broadcasts, VINSERTF128/I128 and
# VEXTRACTF128/I128, VPERM2F128/I128, VPERMQ/PD, VPERMD/PS, VPERMILPS/PD by
# register, the variable shifts -- against byte models: the destination
# register's 256 bits, the memory, the general register. An aligned form on
# a misaligned address is #GP (SIGSEGV). camd runs this test too.
import sys
I386 = '--i386' in sys.argv
ARGS = [a for a in sys.argv[1:] if a != '--i386']
NAME = 'i386_vex_move' if I386 else 'amd64_vex_move'
GPR = '%%edx' if I386 else '%%rdx'
W = 4 if I386 else 8

# Each form: (name, asm, model, kind). The state: ymm2 (a) = s1, ymm3 (b) = s2,
# ymm1 (d) = the destination's old value, memory m (64 bytes at 128(%0)), the
# GPR g (rdx/edx). Result: ymm1 -> out, m -> mout, g -> gout. kind: 0 normal.
F = []
def f(name, asm, model):
    F.append((name, asm, model))
for L, x in ((256, 'ymm'), (128, 'xmm')):
    n = L // 8
    for op in ('vmovups', 'vmovupd', 'vmovdqu', 'vlddqu'):
        f(f'{op} {L} load', f'{op} 128(%0), %%{x}1', f'memcpy(o, m, {n});')
        if op != 'vlddqu':
            f(f'{op} {L} reg', f'{op} %%{x}3, %%{x}1', f'memcpy(o, b, {n});')
    for op in ('vmovaps', 'vmovapd', 'vmovdqa', 'vmovntdqa'):
        f(f'{op} {L} load', f'{op} 128(%0), %%{x}1', f'memcpy(o, m, {n});')
    for op in ('vmovups', 'vmovupd', 'vmovdqu', 'vmovaps', 'vmovapd', 'vmovdqa', 'vmovntps', 'vmovntpd', 'vmovntdq'):
        f(f'{op} {L} store', f'{op} %%{x}3, 128(%0)', f'memcpy(o, d, 32); memcpy(mo, b, {n});')
    for op in ('vmovups', 'vmovaps', 'vmovdqu'):
        f(f'{op} {L} reg store form', f'{op} %%{x}3, %%{x}1' if False else f'.byte 0xc5, {0xfc if L == 256 else 0xf8} ^ 0, 0x11, 0xd9',
          f'memcpy(o, b, {n});')
    # VZEROUPPER/VZEROALL
f('vmovss load', 'vmovss 128(%0), %%xmm1', 'memcpy(o, m, 4);')
f('vmovsd load', 'vmovsd 128(%0), %%xmm1', 'memcpy(o, m, 8);')
f('vmovss reg', 'vmovss %%xmm3, %%xmm2, %%xmm1', 'memcpy(o, a, 16); memcpy(o, b, 4);')
f('vmovsd reg', 'vmovsd %%xmm3, %%xmm2, %%xmm1', 'memcpy(o, a, 16); memcpy(o, b, 8);')
f('vmovss store', 'vmovss %%xmm3, 128(%0)', 'memcpy(o, d, 32); memcpy(mo, b, 4);')
f('vmovsd store', 'vmovsd %%xmm3, 128(%0)', 'memcpy(o, d, 32); memcpy(mo, b, 8);')
f('vmovss reg 11', '.byte 0xc5, 0xea, 0x11, 0xd9', 'memcpy(o, a, 16); memcpy(o, b, 4);')        # vmovss xmm1(rm), xmm2(vvvv), xmm3(reg)
f('vmovsd reg 11', '.byte 0xc5, 0xeb, 0x11, 0xd9', 'memcpy(o, a, 16); memcpy(o, b, 8);')
f('vmovlps load', 'vmovlps 128(%0), %%xmm2, %%xmm1', 'memcpy(o, a, 16); memcpy(o, m, 8);')
f('vmovlpd load', 'vmovlpd 128(%0), %%xmm2, %%xmm1', 'memcpy(o, a, 16); memcpy(o, m, 8);')
f('vmovhps load', 'vmovhps 128(%0), %%xmm2, %%xmm1', 'memcpy(o, a, 8); memcpy(o + 8, m, 8);')
f('vmovhpd load', 'vmovhpd 128(%0), %%xmm2, %%xmm1', 'memcpy(o, a, 8); memcpy(o + 8, m, 8);')
f('vmovhlps', 'vmovhlps %%xmm3, %%xmm2, %%xmm1', 'memcpy(o, b + 8, 8); memcpy(o + 8, a + 8, 8);')
f('vmovlhps', 'vmovlhps %%xmm3, %%xmm2, %%xmm1', 'memcpy(o, a, 8); memcpy(o + 8, b, 8);')
for op, off in (('vmovlps', 0), ('vmovlpd', 0), ('vmovhps', 8), ('vmovhpd', 8)):
    f(f'{op} store', f'{op} %%xmm3, 128(%0)', f'memcpy(o, d, 32); memcpy(mo, b + {off}, 8);')
f('vmovd xmm, r32', f'vmovd %%edx, %%xmm1', 'memcpy(o, &g, 4);')
f('vmovd xmm, m32', 'vmovd 128(%0), %%xmm1', 'memcpy(o, m, 4);')
f('vmovd r32, xmm', 'vmovd %%xmm3, %%edx', 'memcpy(o, d, 32); go = 0; memcpy(&go, b, 4);')
f('vmovd m32, xmm', 'vmovd %%xmm3, 128(%0)', 'memcpy(o, d, 32); memcpy(mo, b, 4);')
if not I386:
    f('vmovq xmm, r64', 'vmovq %%rdx, %%xmm1', 'memcpy(o, &g, 8);')
    f('vmovq r64, xmm', 'vmovq %%xmm3, %%rdx', 'memcpy(o, d, 32); memcpy(&go, b, 8);')
f('vmovq xmm, m64 (F3 7E)', 'vmovq 128(%0), %%xmm1', 'memcpy(o, m, 8);')
f('vmovq xmm, xmm (F3 7E)', 'vmovq %%xmm3, %%xmm1', 'memcpy(o, b, 8);')
f('vmovq m64, xmm (D6)', 'vmovq %%xmm3, 128(%0)', 'memcpy(o, d, 32); memcpy(mo, b, 8);')
f('vmovq xmm, xmm (D6)', '.byte 0xc5, 0xf9, 0xd6, 0xd9', 'memcpy(o, b, 8);')               # vmovq xmm1(rm), xmm3(reg)
f('vzeroupper', 'vzeroupper', 'memcpy(o, d, 16);')
f('vzeroall', 'vzeroall', '')
for op, n, reg in (('vbroadcastss', 4, 1), ('vbroadcastsd', 8, 1), ('vpbroadcastb', 1, 1), ('vpbroadcastw', 2, 1),
                   ('vpbroadcastd', 4, 1), ('vpbroadcastq', 8, 1), ('vbroadcastf128', 16, 0), ('vbroadcasti128', 16, 0)):
    for L, x in ((256, 'ymm'), (128, 'xmm')):
        if (op in ('vbroadcastsd', 'vbroadcastf128', 'vbroadcasti128')) and L == 128:
            continue
        mx = 'xmm'
        f(f'{op} {L} mem', f'{op} 128(%0), %%{x}1', f'for (int i = 0; i < {L // 8}; i += {n}) memcpy(o + i, m, {n});')
        if reg:
            f(f'{op} {L} reg', f'{op} %%xmm3, %%{x}1', f'for (int i = 0; i < {L // 8}; i += {n}) memcpy(o + i, b, {n});')
for op in ('vinsertf128', 'vinserti128'):
    for i in (0, 1):
        f(f'{op} {i} reg', f'{op} ${i}, %%xmm3, %%ymm2, %%ymm1', f'memcpy(o, a, 32); memcpy(o + 16 * {i}, b, 16);')
        f(f'{op} {i} mem', f'{op} ${i}, 128(%0), %%ymm2, %%ymm1', f'memcpy(o, a, 32); memcpy(o + 16 * {i}, m, 16);')
for op in ('vextractf128', 'vextracti128'):
    for i in (0, 1):
        f(f'{op} {i} reg', f'{op} ${i}, %%ymm3, %%xmm1', f'memcpy(o, b + 16 * {i}, 16);')
        f(f'{op} {i} mem', f'{op} ${i}, %%ymm3, 128(%0)', f'memcpy(o, d, 32); memcpy(mo, b + 16 * {i}, 16);')
for op in ('vperm2f128', 'vperm2i128'):
    for imm in (0x00, 0x01, 0x02, 0x03, 0x10, 0x20, 0x31, 0x13, 0x08, 0x80, 0x88, 0x23, 0x12):
        model = f'{{ const uint8_t *q[4] = {{a, a + 16, b, b + 16}}; for (int l = 0; l < 2; l++) {{ int s = ({imm} >> (4 * l)) & 0xf; if (s & 8) memset(o + 16 * l, 0, 16); else memcpy(o + 16 * l, q[s & 3], 16); }} }}'
        f(f'{op} {imm:#x} reg', f'{op} ${imm}, %%ymm3, %%ymm2, %%ymm1', model)
        f(f'{op} {imm:#x} mem', f'{op} ${imm}, 128(%0), %%ymm2, %%ymm1', model.replace('b, b + 16', 'm, m + 16'))
for op in ('vpermq', 'vpermpd'):
    for imm in (0x00, 0x1b, 0x4e, 0xb1, 0xe4, 0xff, 0x93, 0x6c):
        model = f'for (int q = 0; q < 4; q++) memcpy(o + 8 * q, S + 8 * (({imm} >> (2 * q)) & 3), 8);'
        f(f'{op} {imm:#x} reg', f'{op} ${imm}, %%ymm3, %%ymm1', model.replace('S', 'b'))
        f(f'{op} {imm:#x} mem', f'{op} ${imm}, 128(%0), %%ymm1', model.replace('S', 'm'))
for op in ('vpermd', 'vpermps'):
    model = 'for (int i = 0; i < 8; i++) memcpy(o + 4 * i, S + 4 * (a[4 * i] & 7), 4);'
    f(f'{op} reg', f'{op} %%ymm3, %%ymm2, %%ymm1', model.replace('S', 'b'))
    f(f'{op} mem', f'{op} 128(%0), %%ymm2, %%ymm1', model.replace('S', 'm'))
for L, x in ((256, 'ymm'), (128, 'xmm')):
    n = L // 8
    ps = f'for (int i = 0; i < {n}; i += 4) memcpy(o + i, a + (i & ~15) + 4 * (C[i] & 3), 4);'
    pd = f'for (int i = 0; i < {n}; i += 8) memcpy(o + i, a + (i & ~15) + 8 * ((C[i] >> 1) & 1), 8);'
    f(f'vpermilps var {L} reg', f'vpermilps %%{x}3, %%{x}2, %%{x}1', ps.replace('C', 'b'))
    f(f'vpermilps var {L} mem', f'vpermilps 128(%0), %%{x}2, %%{x}1', ps.replace('C', 'm'))
    f(f'vpermilpd var {L} reg', f'vpermilpd %%{x}3, %%{x}2, %%{x}1', pd.replace('C', 'b'))
    f(f'vpermilpd var {L} mem', f'vpermilpd 128(%0), %%{x}2, %%{x}1', pd.replace('C', 'm'))
    for op, lb, kind in (('vpsrlvd', 4, 'srl'), ('vpsrlvq', 8, 'srl'), ('vpsravd', 4, 'sra'), ('vpsllvd', 4, 'sll'), ('vpsllvq', 8, 'sll')):
        model = (f'for (int i = 0; i < {n}; i += {lb}) {{ uint64_t v = 0, c = 0; memcpy(&v, a + i, {lb}); memcpy(&c, C + i, {lb}); '
                 f'int bits = {8 * lb}; uint64_t r; '
                 + ('if (c >= (uint64_t) bits) r = 0; else r = v >> c; ' if kind == 'srl' else
                    'if (c >= (uint64_t) bits) r = 0; else r = v << c; ' if kind == 'sll' else
                    'int64_t sv = (int32_t) v; if (c > 31) c = 31; r = (uint64_t) (sv >> c); ')
                 + f'memcpy(o + i, &r, {lb}); }}')
        f(f'{op} {L} reg', f'{op} %%{x}3, %%{x}2, %%{x}1', model.replace('C', 'b'))
        f(f'{op} {L} mem', f'{op} 128(%0), %%{x}2, %%{x}1', model.replace('C', 'm'))

o = []; w = o.append
w(f'''// Generated by tools/gen-vex-move-test.py{" --i386" if I386 else ""} -- do not edit.
//
// The VEX data movement against byte models (see the generator).
#pragma GCC target("avx2")
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned long checks, bad;
static uint64_t rs = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) {{ rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }}
struct st {{ uint8_t a[32], b[32], d[32], out[32], m[64]; uint64_t g, gout; }} __attribute__((aligned(32)));
#define GMASK {"0xffffffffull" if I386 else "~0ull"}
''')
for i, (name, asm, model) in enumerate(F):
    w(f'''__attribute__((noinline)) static void f{i}(struct st *t) {{
    __asm__ volatile("vmovdqu (%0), %%ymm2\\n vmovdqu 32(%0), %%ymm3\\n vmovdqu 64(%0), %%ymm1\\n mov 192(%0), {GPR}\\n"
                     "{asm}\\n vmovdqu %%ymm1, 96(%0)\\n mov {GPR}, 200(%0)\\n vzeroupper"
                     :: "r"(t) : "memory", "{GPR[2:].replace('%','')}", "xmm1", "xmm2", "xmm3");
}}
static void m{i}(const uint8_t *a, const uint8_t *b, const uint8_t *d, const uint8_t *m, uint64_t g, uint8_t *o, uint8_t *mo, uint64_t *gop) {{
    uint64_t go = g; (void) a; (void) b; (void) d; (void) m; (void) mo;
    {model}
    *gop = go;
}}''')
w('static const struct { const char *name; void (*fn)(struct st *); void (*model)(const uint8_t *, const uint8_t *, const uint8_t *, const uint8_t *, uint64_t, uint8_t *, uint8_t *, uint64_t *); } forms[] = {')
for i, (name, asm, model) in enumerate(F):
    w(f'    {{"{name}", f{i}, m{i}}},')
w('};')
w(f'#define NFORMS {len(F)}')
w(r'''
static sigjmp_buf jb;
static volatile int sig;
static void on_segv(int s) { sig = s; siglongjmp(jb, 1); }

int main(void) {
    static struct st t;
    for (int k = 0; k < 300; k++) {
        for (int fi = 0; fi < NFORMS; fi++) {
            for (int i = 0; i < 32; i++) { t.a[i] = (uint8_t) rnd(); t.b[i] = (uint8_t) rnd(); t.d[i] = (uint8_t) rnd(); }
            for (int i = 0; i < 64; i++) t.m[i] = (uint8_t) rnd();
            if (k % 3 == 1) for (int i = 0; i < 32; i += 4) { t.b[i + 1] = t.b[i + 2] = t.b[i + 3] = 0; t.m[i + 1] = t.m[i + 2] = t.m[i + 3] = 0; t.b[i] &= 0x3f; t.m[i] &= 0x3f; }
            t.g = rnd() & GMASK;
            t.gout = 0;
            memset(t.out, 0, 32);
            struct st in = t;
            forms[fi].fn(&t);
            uint8_t o[32] = {0}, mo[64];
            uint64_t go;
            memcpy(mo, in.m, 64);
            forms[fi].model(in.a, in.b, in.d, in.m, in.g, o, mo, &go);
            go &= GMASK;
            checks++;
            if ((memcmp(t.out, o, 32) || memcmp(t.m, mo, 64) || t.gout != go) && bad++ < 40) {
                printf("FAIL %s (%d)\n  got ", forms[fi].name, k);
                for (int i = 31; i >= 0; i--) printf("%s%02x", i % 8 == 7 ? " " : "", t.out[i]);
                printf("\n  want");
                for (int i = 31; i >= 0; i--) printf("%s%02x", i % 8 == 7 ? " " : "", o[i]);
                printf("\n  mem %s, gpr %llx want %llx\n", memcmp(t.m, mo, 64) ? "DIFFERS" : "ok", (unsigned long long) t.gout, (unsigned long long) go);
            }
        }
    }
    // an aligned form at a misaligned address: #GP
    static const char *names[] = {"vmovaps ymm load", "vmovdqa xmm load", "vmovaps ymm store", "vmovntdq store"};
    static uint8_t buf[96] __attribute__((aligned(32)));
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_segv;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    for (int i = 0; i < 4; i++) {
        sig = 0;
        if (!sigsetjmp(jb, 1)) {
            uint8_t *p = buf + 16;   // 16-aligned, not 32
            if (i == 1) p = buf + 8;
            switch (i) {
            case 0: __asm__ volatile("vmovaps (%0), %%ymm1\n vzeroupper" :: "r"(p) : "memory", "xmm1"); break;
            case 1: __asm__ volatile("vmovdqa (%0), %%xmm1" :: "r"(p) : "memory", "xmm1"); break;
            case 2: __asm__ volatile("vmovaps %%ymm1, (%0)\n vzeroupper" :: "r"(p) : "memory"); break;
            case 3: __asm__ volatile("vmovntdq %%ymm1, (%0)\n vzeroupper" :: "r"(p) : "memory"); break;
            }
        }
        checks++;
        if (sig != SIGSEGV && bad++ < 40)
            printf("FAIL %s misaligned: signal %d, want SIGSEGV\n", names[i], sig);
    }
    printf("NAME: %s (%d forms, %lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", NFORMS, checks, bad);
    return bad != 0;
}''')
open(ARGS[0], 'w').write('\n'.join(o).replace('NAME: %s', NAME + ': %s'))
print(len(F), 'forms')
