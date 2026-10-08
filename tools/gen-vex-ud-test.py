#!/usr/bin/env python3
# Generates tests/manual/x86/{amd64,i386}_vex_ud.c: every VEX (C4) encoding
# of maps 1-3 -- each opcode under each pp, VEX.L and VEX.W, as a register
# form, a memory form ([rsi]/[esi]) and a register form whose vvvv is not
# 1111b -- run under a SIGILL/SIGSEGV/SIGFPE/SIGBUS catcher; the signal each
# raises (or none) is checked against what real x86 hardware did. This is
# the gate for the VEX cutover: AOK runs every VEX op as gadgets, so an
# encoding missing from gen.c's table is #UD and anything the hardware
# runs must be in it.
#
#   tools/gen-vex-ud-test.py probe PROBE.c [--i386]       # run PROBE on x86
#   tools/gen-vex-ud-test.py test ANSWERS OUT.c [--i386]  # ANSWERS = its output
#
# AOK is an Intel CPU (GenuineIntel), and where Intel and AMD differ Intel's
# answer is the one: the amd64 answers are Intel SDE's (`sde64 -spr
# -chip_check_die 0 -chip_check_emit_file 1 -- PROBE sde`, the encodings its
# chip check flags as not Sapphire Rapids' made #UD; "sde" reports AMX, which
# SDE cannot run, as the #UD it is without the OS's tile state). An AMD
# Ryzen differs from it in: the opmask instructions, AVX-VNNI, VEX GFNI, VAES
# and VPCLMULQDQ at L1 (it lacks them), and VPERMQ/VPERMPD with W0 and FMA4
# (it runs them). The i386 answers are the Ryzen's in 32-bit mode with those
# same differences taken from SDE (which cannot run 32-bit code here), and
# VPEXTRQ/VPINSRQ with W1 as their W0 forms (the SDM's N.E.: as KMOVQ r64 is
# KMOVD there; the Ryzen #UDs them).
#
# The instructions are laid out in an executable buffer, one 32-byte slot
# each ending in RET, and called with rsi and rdi at a zeroed 64-byte
# buffer; MXCSR is put back after each (VLDMXCSR loads the buffer).
import sys

I386 = '--i386' in sys.argv
args = [a for a in sys.argv[1:] if a != '--i386']
IMM1 = {0x70, 0x71, 0x72, 0x73, 0xc2, 0xc4, 0xc5, 0xc6}
forms = []   # (name, bytes)
for m in (1, 2, 3):
    for op in range(256):
        imm = m == 3 or (m == 1 and op in IMM1)
        for pp in range(4):
            for L in (0, 1):
                for W in (0, 1):
                    for kind in ('reg', 'mem', 'vvvv'):
                        # vvvv holds the register inverted: 1111b none, 1101b xmm2
                        vvvv = 0b1101 if kind == 'vvvv' else 0b1111
                        b2 = W << 7 | vvvv << 3 | L << 2 | pp
                        modrm = 0x06 if kind == 'mem' else 0xc1
                        b = [0xc4, 0xe0 | m, b2, op, modrm] + ([0x03] if imm else [])   # (R X B clear)
                        if m == 1 and op == 0x77:
                            b = b[:4]                   # VZEROUPPER/VZEROALL: no ModRM
                        forms.append((f'{m}.{op:02x} pp{pp} L{L} W{W} {kind}', b))

SI, DI = ('esi', 'edi') if I386 else ('rsi', 'rdi')
CLOB = '"eax", "ecx", "edx", "esi", "edi"' if I386 else '"rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"'
HEAD = r'''#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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
TABLE.append('static const unsigned char amx[] = {' + ','.join('1' if b[1] & 0x1f == 2 and b[3] in (0x49, 0x4a, 0x4b, 0x5c, 0x5d, 0x5e, 0x5f, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f) else '0'
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
    for (int i = 0; i < NFORM; i++) {
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
    o = [HEAD] + TABLE + [CALL, 'int main(int argc, char **argv) {\n    int sde = argc > 1;',
                          RUN.replace('RESULT', 'printf("%d\\n", last);'), '    return 0;\n}']
    open(args[1], 'w').write('\n'.join(o))
    print(len(forms), 'forms')
else:
    want = [int(x) for x in open(args[1]).read().split()]
    assert len(want) == len(forms)
    name = 'i386_vex_ud' if I386 else 'amd64_vex_ud'
    o = [f'''// Generated by tools/gen-vex-ud-test.py{" --i386" if I386 else ""} -- do not edit.
//
// Every VEX (C4) encoding of maps 1-3 -- each opcode, pp, L and W, register,
// memory and vvvv != 1111b forms: the signal it raises against an Intel CPU's
// (see the generator for how the answers were made).''', HEAD] + TABLE
    o.append('static const unsigned char want[] = {' + ','.join(str(w) for w in want) + '};')
    o.append(CALL)
    o.append('int main(void) {\n    unsigned long checks = 0, bad = 0;\n    const int sde = 0;')
    o.append(RUN.replace('RESULT', '''checks++;
        if (last != want[i] && bad++ < 4000)
            printf("FAIL %s: signal %d, want %d\\n", names[i], last, want[i]);'''))
    o.append(f'    printf("{name}: %s (%d forms, %lu checks, %lu mismatches)\\n", bad ? "FAIL" : "PASS", NFORM, checks, bad);\n    return bad != 0;\n}}')
    open(args[2], 'w').write('\n'.join(o))
    print(len(forms), 'forms')
