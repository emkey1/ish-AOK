#!/usr/bin/env python3
# Generates tests/manual/x86/{amd64,i386}_evex_ud.c: every EVEX encoding of
# maps 1-3, 5 and 6 -- each opcode under each pp, EVEX.L'L 0-2 and W, as a
# register form and a memory form ([rsi]/[esi]), unmasked, vvvv 1111b --
# run under a SIGILL/SIGSEGV/SIGFPE/SIGBUS catcher; the signal each raises
# (or none) is checked against an Intel CPU's. This is the gate for the
# EVEX cutover: AOK runs every EVEX op as gadgets, so an encoding missing
# from gen.c's EVEX table is #UD and anything Intel runs must be in it.
#
#   tools/gen-evex-ud-test.py probe PROBE.c                # run PROBE under SDE
#   tools/gen-evex-ud-test.py test ANSWERS OUT.c [--i386]  # ANSWERS = its output
#
# The answers are Intel SDE's (`sde64 -spr -chip_check_die 0
# -chip_check_emit_file 1 -- PROBE sde [START]`, "index signal" per line;
# START resumes after a form that stops SDE): the encodings its chip check
# flags as not Sapphire Rapids' are #UD, and "sde" reports AMX, which SDE
# cannot run, as the #UD it is without the OS's tile state. No AMD part
# here has AVX-512. Where AOK's answer is not SDE's, aok() below says so:
#   - AVX512-FP16 (maps 5 and 6, and its rows of map 3): Sapphire Rapids
#     has it, AOK neither implements nor advertises it: #UD.
#   - 3.07 pp1 (TILEMOVROW), 3.08 and 3.26 pp3 (VRNDSCALEBF16,
#     VGETMANTBF16, AVX10.2): later parts' encodings that SDE's chip check
#     lets through: #UD on Sapphire Rapids.
# The i386 answers are the same (SDE cannot run 32-bit code here) but for
# the forms whose W the SDM marks N.E. in 32-bit mode -- VCVT(U)SI2SS/SD,
# VCVT(T)SS/SD2(U)SI, VMOVD/Q with a GPR, VPEXTRD/Q, VPINSRD/Q, VPBROADCASTD/Q
# from a GPR -- which run with W1 as their W0 forms (as the WIG VPINSRB/W
# and VPEXTRB/W do in both modes).
#
# The instructions are laid out in an executable buffer, one 32-byte slot
# each ending in RET, and called with rsi and rdi at a zeroed 64-byte
# buffer; MXCSR is put back after each (VLDMXCSR loads the buffer).
import sys

I386 = '--i386' in sys.argv
args = [a for a in sys.argv[1:] if a != '--i386']
IMM1 = {0x70, 0x71, 0x72, 0x73, 0xc2, 0xc4, 0xc5, 0xc6}
forms = []   # (name, bytes)
for m in (1, 2, 3, 5, 6):
    for op in range(256):
        imm = m == 3 or (m == 1 and op in IMM1)
        for pp in range(4):
            for L in (0, 1, 2):
                for W in (0, 1):
                    for kind in ('reg', 'mem'):
                        # 62 P0 P1 P2: R X B R' clear (inverted 1111), vvvv 1111b, V' clear, no masking
                        p0 = 0xf0 | m
                        p1 = W << 7 | 0xf << 3 | 4 | pp
                        p2 = L << 5 | 8
                        modrm = 0x06 if kind == 'mem' else 0xc1
                        b = [0x62, p0, p1, p2, op, modrm] + ([0x03] if imm else [])
                        forms.append((f'{m}.{op:02x} pp{pp} L{L} W{W} {kind}', b))

FP16_MAP3 = {0x08, 0x0a, 0x26, 0x27, 0x56, 0x57, 0x66, 0x67, 0xc2}
NE32 = {(1, 2, 0x2a), (1, 3, 0x2a), (1, 2, 0x2c), (1, 3, 0x2c), (1, 2, 0x2d), (1, 3, 0x2d),
        (1, 2, 0x7b), (1, 3, 0x7b), (1, 2, 0x78), (1, 3, 0x78), (1, 2, 0x79), (1, 3, 0x79),
        (1, 1, 0x6e), (1, 1, 0x7e), (3, 1, 0x16), (3, 1, 0x22), (2, 1, 0x7c)}
def aok(answers):
    """SDE's answers made AOK's (see the header), for this build's forms."""
    want = list(answers)
    index = {}
    for i, (name, b) in enumerate(forms):
        m, op, pp, W = b[1] & 7, b[4], b[2] & 3, b[2] >> 7
        index[m, op, pp, (b[3] >> 5) & 3, W, name.split()[-1]] = i
        if m in (5, 6) or (m == 3 and ((op in FP16_MAP3 and pp == 0) or (op == 0xc2 and pp == 2))):
            want[i] = 4                         # AVX512-FP16
        if (m, pp, op) in ((3, 1, 0x07), (3, 3, 0x08), (3, 3, 0x26)):
            want[i] = 4                         # not Sapphire Rapids'
    if I386:
        for (m, op, pp, L, W, kind), i in index.items():
            if W == 1 and (m, pp, op) in NE32:
                want[i] = want[index[m, op, pp, L, 0, kind]]
    return want

SI, DI = ('esi', 'edi') if I386 else ('rsi', 'rdi')
CLOB = '"eax", "ecx", "edx", "esi", "edi"' if I386 else '"rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"'
HEAD = r'''#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
static sigjmp_buf jb;
static volatile int last;
static void h(int s) { last = s; siglongjmp(jb, 1); }
static uint8_t buf[64] __attribute__((aligned(64)));
static uint8_t *code;
'''
TABLE = ['static const unsigned char insn[][8] = {']
for name, b in forms:
    TABLE.append('    {' + ', '.join('0x%02x' % x for x in b) + '},')
TABLE.append('};')
TABLE.append('static const unsigned char ilen[] = {' + ','.join(str(len(b)) for n, b in forms) + '};')
TABLE.append('static const char *const names[] = {' + ', '.join(f'"{n}"' for n, b in forms) + '};')
TABLE.append('static const unsigned char amx[] = {' + ','.join('1' if b[1] & 7 == 2 and b[4] in (0x49, 0x4a, 0x4b, 0x5c, 0x5d, 0x5e, 0x5f, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f) else '0'
                                                              for n, b in forms) + '};')
TABLE.append(f'#define NFORM {len(forms)}')
CALL = f'''
static void *volatile fnp, *volatile bufp;
static void call(int i) {{
    static unsigned mx = 0x1f80;
    fnp = code + 32 * i;
    bufp = buf;
    __asm__ volatile("mov %0, %%{SI}\\n mov %0, %%{DI}\\n call *%1\\n ldmxcsr %2"
                     :: "m"(bufp), "m"(fnp), "m"(mx) : {CLOB}, "memory", "cc",
                        "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
}}
'''
RUN = r'''
    code = mmap(0, 32 * NFORM, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { perror("mmap"); return 2; }
    for (int i = 0; i < NFORM; i++) {
        memset(code + 32 * i, 0xcc, 32);
        memcpy(code + 32 * i, insn[i], ilen[i]);
        code[32 * i + ilen[i]] = 0xc3;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGILL, &sa, NULL); sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGTRAP, &sa, NULL);
    for (int i = start; i < NFORM; i++) {
        memset(buf, 0, sizeof buf);
        last = 0;
        if (sde && amx[i]) {                    /* AMX: #UD without the OS's tile state; SDE */
            last = 4;                           /* runs it instead (and stops), so report it */
            RESULT
            continue;
        }
        if (!sigsetjmp(jb, 1))
            call(i);
        RESULT
    }
'''
if args[0] == 'probe':
    o = [HEAD] + TABLE + [CALL, 'int main(int argc, char **argv) {\n    int sde = argc > 1, start = argc > 2 ? atoi(argv[2]) : 0;',
                          RUN.replace('RESULT', 'printf("%d %d\\n", i, last); fflush(stdout);'), '    return 0;\n}']
    open(args[1], 'w').write('\n'.join(o))
    print(len(forms), 'forms')
else:
    want = [int(x.split()[-1]) for x in open(args[1]).read().splitlines() if x.strip()]
    assert len(want) == len(forms)
    want = aok(want)
    name = 'i386_evex_ud' if I386 else 'amd64_evex_ud'
    o = [f'''// Generated by tools/gen-evex-ud-test.py{" --i386" if I386 else ""} -- do not edit.
//
// Every EVEX encoding of maps 1-3, 5 and 6 -- each opcode, pp, L'L and W,
// register and memory forms: the signal it raises against an Intel CPU's (see
// the generator for how the answers were made).''', HEAD] + TABLE
    o.append('static const unsigned char want[] = {' + ','.join(str(w) for w in want) + '};')
    o.append(CALL)
    o.append('int main(void) {\n    unsigned long checks = 0, bad = 0;\n    const int sde = 0, start = 0;')
    o.append(RUN.replace('RESULT', '''checks++;
        if (last != want[i] && bad++ < 4000)
            printf("FAIL %s: signal %d, want %d\\n", names[i], last, want[i]);'''))
    o.append(f'    printf("{name}: %s (%d forms, %lu checks, %lu mismatches)\\n", bad ? "FAIL" : "PASS", NFORM, checks, bad);\n    return bad != 0;\n}}')
    open(args[2], 'w').write('\n'.join(o))
    print(len(forms), 'forms')
