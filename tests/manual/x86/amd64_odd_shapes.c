// Shapes the amd64 JIT used to send to C bridges (amd64_jit_reg_imm_op,
// reg_reg_op, modrm_imm): ALU-immediate register forms with a meaningless
// FS/GS prefix or REX.R, MOV $imm8 to AH..BH, MOVSXD without REX.W (a 32-bit
// move, zero-extended, or with 0x66 a 16-bit one), and the encodings that are
// #UD -- LOCK on a register or non-RMW form, C6/C7 /1-/7, XABORT and XBEGIN
// on a CPU without RTM. Every answer from an AMD Ryzen (camd).
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static sigjmp_buf jb;
static volatile int got_sig;
static void on_sig(int sig) {
    got_sig = sig;
    siglongjmp(jb, 1);
}
static uint64_t mem_word;
__attribute__((used)) static uint32_t m32 = 0x80000002u;   // (named only in asm)

int main(void) {
    uint64_t a, flags;

    // 64 48 83 C0 05: fs add $5, %rax
    a = 0x7ffffffffffffffeULL;
    __asm__ volatile(".byte 0x64, 0x48, 0x83, 0xc0, 0x05\n pushfq\n pop %1" : "+a"(a), "=r"(flags));
    CHECK(a == 0x8000000000000003ULL && (flags & 0x8d5) == 0x0894, "fs add: %llx %llx", (unsigned long long) a, (unsigned long long) flags);
    // 65 81 E8 imm32: gs sub $0x10, %eax (32-bit: zero-extends)
    a = 0xffffffff00000005ULL;
    __asm__ volatile(".byte 0x65, 0x81, 0xe8, 0x10, 0, 0, 0\n pushfq\n pop %1" : "+a"(a), "=r"(flags));
    CHECK(a == 0xfffffff5ULL && (flags & 0x8d5) == 0x0085, "gs sub: %llx %llx", (unsigned long long) a, (unsigned long long) flags);
    // 4C 83 C0 07: REX.WR add $7, %rax (REX.R ignored)
    a = 1;
    __asm__ volatile(".byte 0x4c, 0x83, 0xc0, 0x07" : "+a"(a));
    CHECK(a == 8, "rex.r add: %llx", (unsigned long long) a);
    // 4C 81 F0: REX.WR xor $0x0f0f0f0f, %rax
    a = 0xffffffffffffffffULL;
    __asm__ volatile(".byte 0x4c, 0x81, 0xf0, 0x0f, 0x0f, 0x0f, 0x0f" : "+a"(a));
    CHECK(a == 0xfffffffff0f0f0f0ULL, "rex.r xor: %llx", (unsigned long long) a);
    // 64 C1 E0 04: fs shl $4, %eax
    a = 0x1234567812345678ULL;
    __asm__ volatile(".byte 0x64, 0xc1, 0xe0, 0x04" : "+a"(a));
    CHECK(a == 0x23456780ULL, "fs shl: %llx", (unsigned long long) a);
    // 64 48 C7 C0: fs mov $-2, %rax
    a = 0;
    __asm__ volatile(".byte 0x64, 0x48, 0xc7, 0xc0, 0xfe, 0xff, 0xff, 0xff" : "+a"(a));
    CHECK(a == 0xfffffffffffffffeULL, "fs mov: %llx", (unsigned long long) a);

    // C6 /0 to AH, CH, DH, BH (no REX)
    uint64_t b, c, d;
    a = 0x1111111111111111ULL; b = 0x2222222222222222ULL; c = 0x3333333333333333ULL; d = 0x4444444444444444ULL;
    __asm__ volatile(".byte 0xc6, 0xc4, 0xa1\n .byte 0xc6, 0xc5, 0xa2\n .byte 0xc6, 0xc6, 0xa3\n .byte 0xc6, 0xc7, 0xa4"
                     : "+a"(a), "+c"(c), "+d"(d), "+b"(b));
    CHECK(a == 0x111111111111a111ULL && c == 0x333333333333a233ULL && d == 0x444444444444a344ULL &&
          b == 0x222222222222a422ULL, "mov to ah..bh: %llx %llx %llx %llx",
          (unsigned long long) a, (unsigned long long) c, (unsigned long long) d, (unsigned long long) b);
    // 64 C6 C4: fs mov $0x5a, %ah
    a = 0;
    __asm__ volatile(".byte 0x64, 0xc6, 0xc4, 0x5a" : "+a"(a));
    CHECK(a == 0x5a00, "fs mov to ah: %llx", (unsigned long long) a);

    // MOVSXD without REX.W: 63 C1 = movsxd %ecx, %eax -- a 32-bit move
    a = 0xdeadbeefdeadbeefULL; c = 0x00000000fffffff0ULL;
    __asm__ volatile(".byte 0x63, 0xc1" : "+a"(a) : "c"(c));
    CHECK(a == 0xfffffff0ULL, "movsxd r32: %llx", (unsigned long long) a);
    // 66 63 C1: the low word only
    a = 0xdeadbeefdeadbeefULL; c = 0x8765;
    __asm__ volatile(".byte 0x66, 0x63, 0xc1" : "+a"(a) : "c"(c));
    CHECK(a == 0xdeadbeefdead8765ULL, "movsxd r16: %llx", (unsigned long long) a);
    // 48 63 C1: sign-extends
    a = 0; c = 0x80000000ULL;
    __asm__ volatile(".byte 0x48, 0x63, 0xc1" : "+a"(a) : "c"(c));
    CHECK(a == 0xffffffff80000000ULL, "movsxd r64: %llx", (unsigned long long) a);
    // 4C 63 C1: REX.WR movsxd %ecx, %r8
    uint64_t r8v;
    c = 0x80000001ULL;
    __asm__ volatile(".byte 0x4c, 0x63, 0xc1\n mov %%r8, %0" : "=r"(r8v) : "c"(c) : "r8");
    CHECK(r8v == 0xffffffff80000001ULL, "movsxd r8: %llx", (unsigned long long) r8v);

    // MOVSXD from memory: without REX.W a 32-bit load (zero-extended), with
    // 0x66 a 16-bit one; with REX.W (0x66 or not) and through %fs, sign-extended
    a = 0xdeadbeefdeadbeefULL;
    __asm__ volatile(".byte 0x63, 0x05\n .long m32 - 1f\n 1:" : "+a"(a));
    CHECK(a == 0x80000002ULL, "movsxd r32, m32: %llx", (unsigned long long) a);
    a = 0xdeadbeefdeadbeefULL;
    __asm__ volatile(".byte 0x66, 0x63, 0x05\n .long m32 - 1f\n 1:" : "+a"(a));
    CHECK(a == 0xdeadbeefdead0002ULL, "movsxd r16, m16: %llx", (unsigned long long) a);
    a = 0;
    __asm__ volatile(".byte 0x66, 0x48, 0x63, 0x05\n .long m32 - 1f\n 1:" : "+a"(a));
    CHECK(a == 0xffffffff80000002ULL, "66 movsxd r64, m32: %llx", (unsigned long long) a);
    static __thread uint32_t t32;
    t32 = 0x90000003u;
    unsigned long tp;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tp));
    long toff = (long) ((unsigned long) &t32 - tp);
    a = 0;
    __asm__ volatile(".byte 0x64, 0x48, 0x63, 0x04, 0x0d\n .long 0" : "+a"(a) : "c"(toff));   // movslq %fs:(,%rcx), %rax
    CHECK(a == 0xffffffff90000003ULL, "fs movsxd: %llx", (unsigned long long) a);
    // LEA ignores a segment prefix
    a = 0;
    __asm__ volatile(".byte 0x64, 0x48, 0x8d, 0x44, 0x0b, 0x10" : "+a"(a) : "c"(0x1000), "b"(0x20));   // lea %fs:0x10(%rbx,%rcx), %rax
    CHECK(a == 0x1030, "fs lea: %llx", (unsigned long long) a);

    // XCHG with AH..BH: 86 E3 = xchg %ah, %bl; 86 E0 = xchg %ah, %al (one register)
    a = 0x1111111111112233ULL; b = 0x4444444444445566ULL;
    __asm__ volatile(".byte 0x86, 0xe3" : "+a"(a), "+b"(b));
    CHECK(a == 0x1111111111116633ULL && b == 0x4444444444445522ULL, "xchg ah, bl: %llx %llx",
          (unsigned long long) a, (unsigned long long) b);
    a = 0x1111111111112233ULL;
    __asm__ volatile(".byte 0x86, 0xe0" : "+a"(a));
    CHECK(a == 0x1111111111113322ULL, "xchg ah, al: %llx", (unsigned long long) a);
    c = 0x77; d = 0x8899;
    __asm__ volatile(".byte 0x86, 0xf5" : "+c"(c), "+d"(d));   // xchg %dh, %ch
    CHECK(c == 0x8877 && d == 0x0099, "xchg dh, ch: %llx %llx", (unsigned long long) c, (unsigned long long) d);
    // FS-prefixed shift and TEST of a register
    a = 0x8000000000000001ULL;
    __asm__ volatile(".byte 0x64, 0x48, 0xd1, 0xc0\n pushfq\n pop %1" : "+a"(a), "=r"(flags));   // fs rol $1, %rax
    CHECK(a == 3 && (flags & 0x801) == 0x801, "fs rol: %llx %llx", (unsigned long long) a, (unsigned long long) flags);
    a = 0xf0;
    __asm__ volatile(".byte 0x65, 0xf6, 0xc4, 0x0f\n pushfq\n pop %1" : "+a"(a), "=r"(flags));   // gs test $0x0f, %ah
    CHECK((flags & 0x40) == 0x40, "gs test ah: %llx", (unsigned long long) flags);

    // #UD
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    static const char *const ud_name[] = {
        "lock add $5, %rax", "lock mov $1, (mem)", "lock shl $1, (mem)", "c6 /1 (mem)",
        "c7 /2 %eax", "xabort", "xbegin", "lock movsxd %ecx, %eax",
        "lock add %eax, %ecx", "lock mov %ecx, %eax", "lock test %eax, %eax", "lock add %cl, %ah",
        "lock mov %eax, (mem)", "lock lea (mem), %eax", "lea %ecx, %eax", "lock movslq (mem), %rax",
        "lock mov (mem), %rax", "lock xchg %eax, %ecx", "lock shl %eax", "lock test $1, %eax",
        "lock test $1, (mem)",
    };
    for (int i = 0; i < 21; i++) {
        got_sig = 0;
        a = 1;
        mem_word = 7;
        if (sigsetjmp(jb, 1) == 0) {
            switch (i) {
            case 0: __asm__ volatile(".byte 0xf0, 0x48, 0x83, 0xc0, 0x05" : "+a"(a)); break;
            case 1: __asm__ volatile(".byte 0xf0, 0xc6, 0x05\n .long mem_word - 1f\n .byte 1\n 1:" ::: "memory"); break;
            case 2: __asm__ volatile(".byte 0xf0, 0xc1, 0x25\n .long mem_word - 1f\n .byte 1\n 1:" ::: "memory"); break;
            case 3: __asm__ volatile(".byte 0xc6, 0x0d\n .long mem_word - 1f\n .byte 1\n 1:" ::: "memory"); break;
            case 4: __asm__ volatile(".byte 0xc7, 0xd0, 1, 0, 0, 0" : "+a"(a)); break;
            case 5: __asm__ volatile(".byte 0xc6, 0xf8, 0x01" ::: "memory"); break;
            case 6: __asm__ volatile(".byte 0xc7, 0xf8, 0, 0, 0, 0" ::: "memory"); break;
            case 7: __asm__ volatile(".byte 0xf0, 0x63, 0xc1" : "+a"(a) : "c"(5)); break;
            case 8: __asm__ volatile(".byte 0xf0, 0x01, 0xc8" : "+a"(a) : "c"(5)); break;
            case 9: __asm__ volatile(".byte 0xf0, 0x89, 0xc8" : "+a"(a) : "c"(5)); break;
            case 10: __asm__ volatile(".byte 0xf0, 0x85, 0xc0\n movq $2, %0" : "+a"(a)); break;
            case 11: __asm__ volatile(".byte 0xf0, 0x00, 0xcc" : "+a"(a) : "c"(5)); break;
            case 12: __asm__ volatile(".byte 0xf0, 0x89, 0x05\n .long mem_word - 1f\n 1:" :: "a"(9) : "memory"); break;
            case 13: __asm__ volatile(".byte 0xf0, 0x8d, 0x05\n .long mem_word - 1f\n 1:" : "+a"(a)); break;
            case 14: __asm__ volatile(".byte 0x8d, 0xc1" : "+a"(a)); break;
            case 15: __asm__ volatile(".byte 0xf0, 0x48, 0x63, 0x05\n .long mem_word - 1f\n 1:" : "+a"(a)); break;
            case 16: __asm__ volatile(".byte 0xf0, 0x48, 0x8b, 0x05\n .long mem_word - 1f\n 1:" : "+a"(a)); break;
            case 17: __asm__ volatile(".byte 0xf0, 0x87, 0xc8" : "+a"(a) : "c"(5)); break;
            case 18: __asm__ volatile(".byte 0xf0, 0xd1, 0xe0" : "+a"(a)); break;
            case 19: __asm__ volatile(".byte 0xf0, 0xf7, 0xc0, 1, 0, 0, 0\n movq $2, %0" : "+a"(a)); break;
            case 20: __asm__ volatile(".byte 0xf0, 0xf6, 0x05\n .long mem_word - 1f\n .byte 1\n 1:\n movq $2, %0" : "+a"(a)); break;
            }
        }
        CHECK(got_sig == SIGILL && a == 1 && mem_word == 7, "%s: signal %d, rax %llx, mem %llx", ud_name[i], got_sig,
              (unsigned long long) a, (unsigned long long) mem_word);
    }

    printf("amd64_odd_shapes: %s (%d checks, %d failures)\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
