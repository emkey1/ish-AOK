// i386_push16 -- the 0x66 operand-size prefix makes the stack instructions
// move two bytes: PUSH and POP of a register, an immediate or memory, PUSHA,
// POPA, LEAVE, CALL and RET. And PUSHA/POPA fault as one instruction.
//
// Measured on camd (x86_64 Linux 6.12, gcc -m32), a 32-bit task:
//   - 66 50+r, 66 68 iw, 66 6a ib, 66 ff /6: ESP -= 2 and two bytes are
//     stored; the neighbours stay put. PUSH SP stores SP from before the
//     decrement, and PUSH m16 with an ESP base reads through the old ESP.
//   - 66 58+r, 66 8f /0: ESP += 2, the low word of the register is loaded and
//     the high word kept. POP m16 with an ESP base addresses through the NEW
//     ESP. POP SP is the popped word with ESP's high word.
//   - 66 60 / 66 61: sixteen bytes. PUSHAW's SP slot is SP from before the
//     instruction, and so is PUSHAD's ESP slot; POPA skips that slot.
//   - 66 c9: ESP = EBP whole (the stack is 32-bit), then BP = the popped word.
//   - 66 e8, 66 ff /2: push the low word of the return address and jump to the
//     target's low word; 66 c3 / 66 c2 iw pop a word and jump to it. Below
//     64 KiB nothing can be mapped, so each is SIGSEGV SEGV_MAPERR at that
//     address, with ESP already moved. 66 ff /4 (JMPW r/m16) the same, less
//     the stack: it shares the JIT's word-sized target, which kept the high
//     half of a 32-bit value on the x86_64 gadgets.
//   - FE is INC and DEC r/m8 only: /2 to /7 (a byte-sized CALL, JMP or PUSH)
//     are SIGILL ILL_ILLOPN at the instruction, where the JIT ran them.
//   - A fault leaves ESP where it was, and EIP at the instruction: a push to
//     an unmapped page, a pop from one, a POP m16 whose store faults (after
//     the pop has read the stack), and a PUSHA or POPA that crosses into one
//     partway through. The registers a POPA had loaded before its fault are
//     NOT restored -- camd's Zen+ has DI, SI and BP when the fifth word
//     faults -- so there only ESP and EIP are compared.
//
// On AOK's i386 JIT every one of the 16-bit forms moved ESP by 4 (the 32-bit
// push/pop gadgets whatever the prefix); PUSHA stored the already-decremented
// ESP; a PUSHA or POPA that faulted partway had moved ESP and was re-run from
// there; LEAVE moved only SP; CALLW/RETW pushed and popped four bytes and
// kept the whole target; and FE /2, /4 and /6 called, jumped and pushed.
//
// Harness: each probe is a small asm function that loads all eight registers
// from a context block (ESP too, into a window of pages pre-filled with 0xAA),
// runs the one instruction, and stores all eight back through a scratch
// register that holds the context's address. The 32-bit forms go first as the
// positive control: the same harness must see them move four bytes.
//
// i386 only (the amd64 guest has no 16-bit call/ret and no PUSHA).
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include "../test_common.h"

#if !defined(__i386__)
int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    printf("i386_push16: SKIP (i386 only)\n");
    return 0;
}
#else

#ifndef ILL_ILLOPN
#define ILL_ILLOPN 2
#endif
#ifndef SEGV_MAPERR
#define SEGV_MAPERR 1
#endif

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };
static const char *const rname[8] = {
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
};

struct ctx {
    uint32_t in[8];     // 0x00: loaded before the instruction
    uint32_t out[8];    // 0x20: stored after it
    uint32_t real_esp;  // 0x40
    uint32_t pad[15];
    uint8_t data[64];   // memory operands
};
_Static_assert(offsetof(struct ctx, out) == 0x20, "asm offsets");
_Static_assert(offsetof(struct ctx, real_esp) == 0x40, "asm offsets");

#define HIDDEN __attribute__((visibility("hidden")))

#define LABEL(name) ".globl " #name "\n\t.hidden " #name "\n" #name ":\n\t"
// scr: the register that holds the context's address across the instruction.
#define PROBE(name, scr, insn)                                                 \
    ".globl " #name "\n\t.hidden " #name "\n\t.type " #name ",@function\n"     \
    #name ":\n\t"                                                              \
    "movl 4(%esp), %eax\n\t"                                                   \
    "pushl %ebp\n\tpushl %ebx\n\tpushl %esi\n\tpushl %edi\n\t"                 \
    "movl %esp, 0x40(%eax)\n\t"                                                \
    "movl 0x04(%eax), %ecx\n\tmovl 0x08(%eax), %edx\n\t"                       \
    "movl 0x0c(%eax), %ebx\n\tmovl 0x14(%eax), %ebp\n\t"                       \
    "movl 0x18(%eax), %esi\n\tmovl 0x1c(%eax), %edi\n\t"                       \
    "movl 0x10(%eax), %esp\n\tmovl 0x00(%eax), %eax\n"                         \
    LABEL(name##_insn)                                                         \
    insn "\n"                                                                  \
    LABEL(name##_end)                                                          \
    "movl %eax, 0x20(%" scr ")\n\tmovl %ecx, 0x24(%" scr ")\n\t"               \
    "movl %edx, 0x28(%" scr ")\n\tmovl %ebx, 0x2c(%" scr ")\n\t"               \
    "movl %esp, 0x30(%" scr ")\n\tmovl %ebp, 0x34(%" scr ")\n\t"               \
    "movl %esi, 0x38(%" scr ")\n\tmovl %edi, 0x3c(%" scr ")\n\t"               \
    "movl 0x40(%" scr "), %esp\n\t"                                            \
    "popl %edi\n\tpopl %esi\n\tpopl %ebx\n\tpopl %ebp\n\tret\n\t"

// Every instruction is spelled in bytes, so the encoding under test is the
// one written here whatever the assembler prefers.
__asm__(
    ".pushsection .text\n\t"
    PROBE(c_push32_eax, "ebx", ".byte 0x50")
    PROBE(c_pop32_eax, "ebx", ".byte 0x58")
    PROBE(c_push32_imm, "ebx", ".byte 0x68, 0x78, 0x56, 0x34, 0x12")
    PROBE(c_pop32_mem, "ebx", ".byte 0x8f, 0x01")              // popl (%ecx)

    PROBE(push16_r0, "ebx", ".byte 0x66, 0x50")
    PROBE(push16_r1, "ebx", ".byte 0x66, 0x51")
    PROBE(push16_r2, "ebx", ".byte 0x66, 0x52")
    PROBE(push16_r3, "ecx", ".byte 0x66, 0x53")
    PROBE(push16_r4, "ebx", ".byte 0x66, 0x54")
    PROBE(push16_r5, "ebx", ".byte 0x66, 0x55")
    PROBE(push16_r6, "ebx", ".byte 0x66, 0x56")
    PROBE(push16_r7, "ebx", ".byte 0x66, 0x57")
    PROBE(pop16_r0, "ebx", ".byte 0x66, 0x58")
    PROBE(pop16_r1, "ebx", ".byte 0x66, 0x59")
    PROBE(pop16_r2, "ebx", ".byte 0x66, 0x5a")
    PROBE(pop16_r3, "ecx", ".byte 0x66, 0x5b")
    PROBE(pop16_r4, "ebx", ".byte 0x66, 0x5c")
    PROBE(pop16_r5, "ebx", ".byte 0x66, 0x5d")
    PROBE(pop16_r6, "ebx", ".byte 0x66, 0x5e")
    PROBE(pop16_r7, "ebx", ".byte 0x66, 0x5f")
    PROBE(pop16_rm_r0, "ebx", ".byte 0x66, 0x8f, 0xc0")        // 8f /0, register form

    PROBE(push16_imm16, "ebx", ".byte 0x66, 0x68, 0x34, 0x12")
    PROBE(push16_imm8n, "ebx", ".byte 0x66, 0x6a, 0xfb")
    PROBE(push16_imm8p, "ebx", ".byte 0x66, 0x6a, 0x05")
    PROBE(push16_mem, "ebx", ".byte 0x66, 0xff, 0x31")          // pushw (%ecx)
    PROBE(push16_mem_esp, "ebx", ".byte 0x66, 0xff, 0x74, 0x24, 0x02") // pushw 2(%esp)
    PROBE(pop16_mem, "ebx", ".byte 0x66, 0x8f, 0x01")           // popw (%ecx)
    PROBE(pop16_mem_esp, "ebx", ".byte 0x66, 0x8f, 0x44, 0x24, 0x02")  // popw 2(%esp)

    PROBE(pusha16, "ebx", ".byte 0x66, 0x60")
    PROBE(pusha32, "ebx", ".byte 0x60")
    PROBE(popa16, "ebx", ".byte 0x66, 0x61")
    PROBE(popa32, "ebx", ".byte 0x61")

    PROBE(leave16, "ebx", ".byte 0x66, 0xc9")
    PROBE(leave32, "ebx", ".byte 0xc9")

    PROBE(call16_rel, "ebx", ".byte 0x66, 0xe8, 0x34, 0x12")
    PROBE(call16_reg, "ebx", ".byte 0x66, 0xff, 0xd0")          // callw *%ax
    PROBE(call16_mem, "ebx", ".byte 0x66, 0xff, 0x11")          // callw *(%ecx)
    PROBE(ret16, "ebx", ".byte 0x66, 0xc3")
    PROBE(ret16_imm, "ebx", ".byte 0x66, 0xc2, 0x06, 0x00")
    PROBE(jmp16_reg, "ebx", ".byte 0x66, 0xff, 0xe0")           // jmpw *%ax
    PROBE(jmp16_mem, "ebx", ".byte 0x66, 0xff, 0x21")           // jmpw *(%ecx)

    PROBE(fe_call, "ebx", ".byte 0xfe, 0xd0")                   // FE /2, register
    PROBE(fe_jmp, "ebx", ".byte 0xfe, 0x21")                    // FE /4, memory
    PROBE(fe_push, "ebx", ".byte 0xfe, 0x31")                   // FE /6, memory
    PROBE(fe_7, "ebx", ".byte 0xfe, 0xf8")                      // FE /7
    ".popsection\n"
);

typedef void probe_fn(struct ctx *);
#define DECL(name)                                                             \
    extern probe_fn name HIDDEN;                                               \
    extern const char name##_insn[] HIDDEN, name##_end[] HIDDEN;
DECL(c_push32_eax) DECL(c_pop32_eax) DECL(c_push32_imm) DECL(c_pop32_mem)
DECL(push16_r0) DECL(push16_r1) DECL(push16_r2) DECL(push16_r3)
DECL(push16_r4) DECL(push16_r5) DECL(push16_r6) DECL(push16_r7)
DECL(pop16_r0) DECL(pop16_r1) DECL(pop16_r2) DECL(pop16_r3)
DECL(pop16_r4) DECL(pop16_r5) DECL(pop16_r6) DECL(pop16_r7)
DECL(pop16_rm_r0)
DECL(push16_imm16) DECL(push16_imm8n) DECL(push16_imm8p)
DECL(push16_mem) DECL(push16_mem_esp) DECL(pop16_mem) DECL(pop16_mem_esp)
DECL(pusha16) DECL(pusha32) DECL(popa16) DECL(popa32)
DECL(leave16) DECL(leave32)
DECL(call16_rel) DECL(call16_reg) DECL(call16_mem) DECL(ret16) DECL(ret16_imm)
DECL(jmp16_reg) DECL(jmp16_mem)
DECL(fe_call) DECL(fe_jmp) DECL(fe_push) DECL(fe_7)

static probe_fn *const push16_r[8] = {
    push16_r0, push16_r1, push16_r2, push16_r3,
    push16_r4, push16_r5, push16_r6, push16_r7,
};
static probe_fn *const pop16_r[8] = {
    pop16_r0, pop16_r1, pop16_r2, pop16_r3,
    pop16_r4, pop16_r5, pop16_r6, pop16_r7,
};
static const char *const push16_insn[8] = {
    push16_r0_insn, push16_r1_insn, push16_r2_insn, push16_r3_insn,
    push16_r4_insn, push16_r5_insn, push16_r6_insn, push16_r7_insn,
};

// The mapping: page 0 the context, page 1 unmapped (PROT_NONE), pages 2 and 3
// the stack window, page 4 PROT_NONE again. A fault test points ESP or a
// memory operand at an edge.
static long pg;
static uint8_t *map;
static struct ctx *C;
static uint8_t *stk_lo;   // start of page 2
static uint8_t *stk_mid;  // start of page 3
static uint8_t *stk_hi;   // start of page 4 (PROT_NONE)
static uint8_t *TOP;      // the usual ESP: the middle of page 3
#define A(p) ((uint32_t) (uintptr_t) (p))

static const uint32_t init_regs[8] = {
    0x4a5b6c7d, 0x3c4d5e6f, 0x2e3f4051, 0x16273849,
    0, 0x0b1c2d3e, 0x6e7f8091, 0x7d8e9fa0,
};

static void reset(uint32_t esp) {
    memset(stk_lo, 0xaa, 2 * pg);
    memset(C->data, 0xcc, sizeof C->data);
    memcpy(C->in, init_regs, sizeof init_regs);
    C->in[ESP] = esp;
    memset(C->out, 0, sizeof C->out);
}

static void put16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

// ---- signals ----------------------------------------------------------------

static sigjmp_buf env;
static struct {
    int sig, code;
    uint32_t addr, eip, esp, regs[8];
} F;

static void on_fault(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    greg_t *g = uc->uc_mcontext.gregs;
    F.sig = sig;
    F.code = si->si_code;
    F.addr = A(si->si_addr);
    F.eip = g[REG_EIP];
    F.esp = g[REG_ESP];
    F.regs[EAX] = g[REG_EAX];
    F.regs[ECX] = g[REG_ECX];
    F.regs[EDX] = g[REG_EDX];
    F.regs[EBX] = g[REG_EBX];
    F.regs[ESP] = g[REG_ESP];
    F.regs[EBP] = g[REG_EBP];
    F.regs[ESI] = g[REG_ESI];
    F.regs[EDI] = g[REG_EDI];
    siglongjmp(env, 1);
}

// Returns 1 if the probe took a signal (recorded in F).
static int run(probe_fn *fn, int scr) {
    C->in[scr] = A(C);
    memset(&F, 0, sizeof F);
    if (sigsetjmp(env, 1) == 0) {
        fn(C);
        return 0;
    }
    return 1;
}

// ---- checks -----------------------------------------------------------------

static int fail(const char *what, const char *fmt, ...) {
    va_list ap;
    printf("FAIL %s: ", what);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    failures_total++;
    return 0;
}

static int no_fault(const char *what, int faulted) {
    if (!faulted)
        return 1;
    return fail(what, "signal %d (si_code %d, addr %#x) at eip %#x, esp %#x",
                F.sig, F.code, F.addr, F.eip, F.esp);
}

static int check_regs(const char *what, const uint32_t *got, const uint32_t *want) {
    int ok = 1;
    for (int r = 0; r < 8; r++) {
        if (got[r] != want[r])
            ok = fail(what, "%s = %#010x, want %#010x", rname[r], got[r], want[r]);
    }
    return ok;
}

// A window of memory around an address, snapshotted before the probe, and
// the bytes it should hold afterwards.
#define WIN 40
static uint8_t *win_at;
static uint8_t want_win[2 * WIN];

static void snap(uint8_t *center) {
    win_at = center - WIN;
    memcpy(want_win, win_at, sizeof want_win);
}
static void want16(uint8_t *at, uint32_t v) { put16(want_win + (at - win_at), v); }
static void want32(uint8_t *at, uint32_t v) { put32(want_win + (at - win_at), v); }

static int check_win(const char *what) {
    if (memcmp(win_at, want_win, sizeof want_win) == 0)
        return 1;
    for (int i = 0; i < 2 * WIN; i++) {
        if (win_at[i] != want_win[i])
            fail(what, "byte at %#x (%+d from the centre) is %#04x, want %#04x",
                 A(win_at + i), i - WIN, win_at[i], want_win[i]);
    }
    return 0;
}

static void ok(const char *what, int good) {
    if (good)
        test_logf("  ok   %s\n", what);
}

// A signal at `insn` with ESP and the other registers as they were before it.
static int check_fault_at(const char *what, int faulted, const char *insn,
                          const uint32_t *regs) {
    if (!faulted)
        return fail(what, "no signal (esp after %#x, want a SIGSEGV)", C->out[ESP]);
    int good = 1;
    if (F.sig != SIGSEGV)
        good = fail(what, "signal %d, want SIGSEGV", F.sig);
    if (F.eip != A(insn))
        good = fail(what, "eip %#x, want %#x (the instruction)", F.eip, A(insn));
    if (!check_regs(what, F.regs, regs))
        good = 0;
    return good;
}

// The same, where only ESP and EIP are defined (a POPA that faults partway).
static int check_fault_esp(const char *what, int faulted, const char *insn,
                           uint32_t esp) {
    if (!faulted)
        return fail(what, "no signal (esp after %#x, want a SIGSEGV)", C->out[ESP]);
    int good = 1;
    if (F.sig != SIGSEGV)
        good = fail(what, "signal %d, want SIGSEGV", F.sig);
    if (F.eip != A(insn))
        good = fail(what, "eip %#x, want %#x (the instruction)", F.eip, A(insn));
    if (F.esp != esp)
        good = fail(what, "esp %#x, want %#x", F.esp, esp);
    return good;
}

// ---- the positive control: 32-bit forms, same harness ------------------------

static void test_controls(void) {
    uint32_t want[8];
    int good;

    reset(A(TOP));
    snap(TOP);
    good = no_fault("control: pushl %eax", run(c_push32_eax, EBX));
    memcpy(want, C->in, sizeof want);
    want[ESP] = A(TOP) - 4;
    want32(TOP - 4, C->in[EAX]);
    good = good && check_regs("control: pushl %eax", C->out, want) &&
           check_win("control: pushl %eax");
    ok("control: pushl %eax moves esp by 4 and stores 4 bytes", good);

    reset(A(TOP));
    put32(TOP, 0x13579bdf);
    snap(TOP);
    good = no_fault("control: popl %eax", run(c_pop32_eax, EBX));
    memcpy(want, C->in, sizeof want);
    want[EAX] = 0x13579bdf;
    want[ESP] = A(TOP) + 4;
    good = good && check_regs("control: popl %eax", C->out, want) &&
           check_win("control: popl %eax");
    ok("control: popl %eax moves esp by 4", good);

    reset(A(TOP));
    snap(TOP);
    good = no_fault("control: pushl $imm32", run(c_push32_imm, EBX));
    memcpy(want, C->in, sizeof want);
    want[ESP] = A(TOP) - 4;
    want32(TOP - 4, 0x12345678);
    good = good && check_regs("control: pushl $imm32", C->out, want) &&
           check_win("control: pushl $imm32");
    ok("control: pushl $imm32", good);

    // popl (%ecx) whose store faults: ESP back where it was.
    reset(A(TOP));
    put32(TOP, 0x2468ace0);
    C->in[ECX] = A(map + pg);
    int f = run(c_pop32_mem, EBX);
    memcpy(want, C->in, sizeof want);
    good = check_fault_at("control: popl (%ecx) to an unmapped page", f,
                          c_pop32_mem_insn, want);
    ok("control: popl (%ecx) to an unmapped page faults with esp unmoved", good);
}

// ---- 66 50+r, 66 58+r --------------------------------------------------------

static void test_push_pop_reg(void) {
    char what[64];
    uint32_t want[8];
    for (int r = 0; r < 8; r++) {
        int scr = r == EBX ? ECX : EBX;
        snprintf(what, sizeof what, "pushw %%%s (66 %02x)", rname[r] + 1, 0x50 + r);
        reset(A(TOP));
        snap(TOP);
        int good = no_fault(what, run(push16_r[r], scr));
        memcpy(want, C->in, sizeof want);
        want[ESP] = A(TOP) - 2;
        // PUSH SP stores SP from before the decrement.
        want16(TOP - 2, C->in[r]);
        good = good && check_regs(what, C->out, want) && check_win(what);
        ok(what, good);

        snprintf(what, sizeof what, "popw %%%s (66 %02x)", rname[r] + 1, 0x58 + r);
        reset(A(TOP));
        put16(TOP, 0x4b3c);
        put16(TOP + 2, 0x5a69);
        snap(TOP);
        good = no_fault(what, run(pop16_r[r], scr));
        memcpy(want, C->in, sizeof want);
        want[ESP] = A(TOP) + 2;
        want[r] = (want[r] & 0xffff0000) | 0x4b3c;
        good = good && check_regs(what, C->out, want) && check_win(what);
        ok(what, good);
    }

    const char *what2 = "popw %ax, the 8f /0 register form";
    reset(A(TOP));
    put16(TOP, 0x4b3c);
    snap(TOP);
    int good = no_fault(what2, run(pop16_rm_r0, EBX));
    memcpy(want, C->in, sizeof want);
    want[ESP] = A(TOP) + 2;
    want[EAX] = (want[EAX] & 0xffff0000) | 0x4b3c;
    good = good && check_regs(what2, C->out, want) && check_win(what2);
    ok(what2, good);
}

// ---- immediates and memory ---------------------------------------------------

static void test_push_imm_mem(void) {
    static const struct {
        const char *what;
        probe_fn *fn;
        uint32_t value;
    } imms[] = {
        {"pushw $0x1234 (66 68 iw)", push16_imm16, 0x1234},
        {"pushw $-5 (66 6a ib, sign-extended to a word)", push16_imm8n, 0xfffb},
        {"pushw $5 (66 6a ib)", push16_imm8p, 0x0005},
    };
    uint32_t want[8];
    for (size_t i = 0; i < sizeof imms / sizeof imms[0]; i++) {
        reset(A(TOP));
        snap(TOP);
        int good = no_fault(imms[i].what, run(imms[i].fn, EBX));
        memcpy(want, C->in, sizeof want);
        want[ESP] = A(TOP) - 2;
        want16(TOP - 2, imms[i].value);
        good = good && check_regs(imms[i].what, C->out, want) &&
               check_win(imms[i].what);
        ok(imms[i].what, good);
    }

    const char *what = "pushw (%ecx) (66 ff /6)";
    reset(A(TOP));
    put16(C->data + 8, 0x7e81);
    C->in[ECX] = A(C->data + 8);
    snap(TOP);
    int good = no_fault(what, run(push16_mem, EBX));
    memcpy(want, C->in, sizeof want);
    want[ESP] = A(TOP) - 2;
    want16(TOP - 2, 0x7e81);
    good = good && check_regs(what, C->out, want) && check_win(what);
    ok(what, good);

    // The source address is computed from ESP before the decrement.
    what = "pushw 2(%esp) reads through the old esp";
    reset(A(TOP));
    put16(TOP, 0x0f1e);
    put16(TOP + 2, 0xa596);
    snap(TOP);
    good = no_fault(what, run(push16_mem_esp, EBX));
    memcpy(want, C->in, sizeof want);
    want[ESP] = A(TOP) - 2;
    want16(TOP - 2, 0xa596);
    good = good && check_regs(what, C->out, want) && check_win(what);
    ok(what, good);

    what = "popw (%ecx) (66 8f /0) stores two bytes";
    reset(A(TOP));
    put16(TOP, 0x3c2d);
    put16(TOP + 2, 0x5a4b);
    C->in[ECX] = A(C->data + 8);
    snap(C->data + 8);
    good = no_fault(what, run(pop16_mem, EBX));
    memcpy(want, C->in, sizeof want);
    want[ESP] = A(TOP) + 2;
    want16(C->data + 8, 0x3c2d);
    good = good && check_regs(what, C->out, want) && check_win(what);
    ok(what, good);

    // The destination address is computed from ESP after the increment.
    what = "popw 2(%esp) writes through the new esp";
    reset(A(TOP));
    for (int i = 0; i < 8; i++)
        TOP[i] = 0x10 + 0x11 * i;
    snap(TOP);
    good = no_fault(what, run(pop16_mem_esp, EBX));
    memcpy(want, C->in, sizeof want);
    want[ESP] = A(TOP) + 2;
    want16(TOP + 4, 0x2110);
    good = good && check_regs(what, C->out, want) && check_win(what);
    ok(what, good);
}

// ---- page crossings and faults -----------------------------------------------

static void test_edges(void) {
    uint32_t want[8];
    int good, f;
    const char *what;

    what = "pushw across a page boundary";
    reset(A(stk_mid + 1));
    snap(stk_mid);
    good = no_fault(what, run(push16_imm16, EBX));
    memcpy(want, C->in, sizeof want);
    want[ESP] = A(stk_mid) - 1;
    want16(stk_mid - 1, 0x1234);
    good = good && check_regs(what, C->out, want) && check_win(what);
    ok(what, good);

    what = "popw %ax across a page boundary";
    reset(A(stk_mid - 1));
    put16(stk_mid - 1, 0x6d5e);
    snap(stk_mid);
    good = no_fault(what, run(pop16_r0, EBX));
    memcpy(want, C->in, sizeof want);
    want[ESP] = A(stk_mid) + 1;
    want[EAX] = (want[EAX] & 0xffff0000) | 0x6d5e;
    good = good && check_regs(what, C->out, want) && check_win(what);
    ok(what, good);

    what = "popw (%ecx) with the store across a page boundary";
    reset(A(TOP));
    put16(TOP, 0x8f7e);
    C->in[ECX] = A(stk_mid - 1);
    snap(stk_mid);
    good = no_fault(what, run(pop16_mem, EBX));
    memcpy(want, C->in, sizeof want);
    want[ESP] = A(TOP) + 2;
    want16(stk_mid - 1, 0x8f7e);
    good = good && check_regs(what, C->out, want) && check_win(what);
    ok(what, good);

    what = "pushw with esp at the bottom of the mapping";
    reset(A(stk_lo));
    f = run(push16_imm16, EBX);
    memcpy(want, C->in, sizeof want);
    want[EBX] = A(C);
    good = check_fault_at(what, f, push16_imm16_insn, want);
    if (f && F.addr != A(stk_lo) - 2)
        good = fail(what, "si_addr %#x, want %#x", F.addr, A(stk_lo) - 2);
    ok(what, good);

    what = "pushw straddling the bottom of the mapping";
    reset(A(stk_lo) + 1);
    f = run(push16_imm16, EBX);
    memcpy(want, C->in, sizeof want);
    want[EBX] = A(C);
    good = check_fault_at(what, f, push16_imm16_insn, want);
    ok(what, good);

    what = "pushw %sp with esp at the bottom of the mapping";
    reset(A(stk_lo));
    f = run(push16_r[ESP], EBX);
    memcpy(want, C->in, sizeof want);
    want[EBX] = A(C);
    good = check_fault_at(what, f, push16_insn[ESP], want);
    ok(what, good);

    what = "popw %ax with esp at the top of the mapping";
    reset(A(stk_hi));
    f = run(pop16_r0, EBX);
    memcpy(want, C->in, sizeof want);
    want[EBX] = A(C);
    good = check_fault_at(what, f, pop16_r0_insn, want);
    if (f && F.addr != A(stk_hi))
        good = fail(what, "si_addr %#x, want %#x", F.addr, A(stk_hi));
    ok(what, good);

    what = "popw %ax straddling the top of the mapping";
    reset(A(stk_hi) - 1);
    f = run(pop16_r0, EBX);
    memcpy(want, C->in, sizeof want);
    want[EBX] = A(C);
    good = check_fault_at(what, f, pop16_r0_insn, want);
    ok(what, good);

    // The pop has read the stack by the time the store faults.
    what = "popw (%ecx) to an unmapped page";
    reset(A(TOP));
    put16(TOP, 0x1357);
    C->in[ECX] = A(map + pg);
    f = run(pop16_mem, EBX);
    memcpy(want, C->in, sizeof want);
    want[EBX] = A(C);
    good = check_fault_at(what, f, pop16_mem_insn, want);
    if (f && F.addr != A(map + pg))
        good = fail(what, "si_addr %#x, want %#x", F.addr, A(map + pg));
    ok(what, good);

    what = "pushw (%ecx) from an unmapped page";
    reset(A(TOP));
    C->in[ECX] = A(map + pg);
    f = run(push16_mem, EBX);
    memcpy(want, C->in, sizeof want);
    want[EBX] = A(C);
    good = check_fault_at(what, f, push16_mem_insn, want);
    ok(what, good);
}

// ---- PUSHA, POPA -------------------------------------------------------------

static void test_pusha_popa(void) {
    uint32_t want[8];
    int good, f;
    const char *what;
    // PUSHA's order, and POPA's in reverse.
    static const int order[8] = {EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI};

    what = "pushaw (66 60)";
    reset(A(TOP));
    snap(TOP);
    good = no_fault(what, run(pusha16, EBX));
    memcpy(want, C->in, sizeof want);
    want[ESP] = A(TOP) - 16;
    for (int i = 0; i < 8; i++)
        want16(TOP - 2 * (i + 1), C->in[order[i]]);
    good = good && check_regs(what, C->out, want) && check_win(what);
    ok(what, good);

    what = "pushal (60) stores esp from before the instruction";
    reset(A(TOP));
    snap(TOP);
    good = no_fault(what, run(pusha32, EBX));
    memcpy(want, C->in, sizeof want);
    want[ESP] = A(TOP) - 32;
    for (int i = 0; i < 8; i++)
        want32(TOP - 4 * (i + 1), C->in[order[i]]);
    good = good && check_regs(what, C->out, want) && check_win(what);
    ok(what, good);

    // The frame, lowest address first: DI SI BP (SP, skipped) BX DX CX AX.
    static const uint16_t frame16[8] = {
        0x1d2d, 0x3d4d, 0x5d6d, 0x7d8d, 0 /* BX */, 0x9dad, 0xbdcd, 0xdded,
    };
    what = "popaw (66 61)";
    reset(A(TOP));
    for (int i = 0; i < 8; i++)
        put16(TOP + 2 * i, i == 4 ? A(C) & 0xffff : frame16[i]);
    snap(TOP);
    good = no_fault(what, run(popa16, EBX));
    memcpy(want, C->in, sizeof want);
    want[EBX] = A(C);
    for (int i = 0; i < 8; i++) {
        int r = order[7 - i];
        if (r != ESP)
            want[r] = (want[r] & 0xffff0000) | (i == 4 ? A(C) & 0xffff : frame16[i]);
    }
    want[ESP] = A(TOP) + 16;
    good = good && check_regs(what, C->out, want) && check_win(what);
    ok(what, good);

    static const uint32_t frame32[8] = {
        0x11223344, 0x55667788, 0x99aabbcc, 0xdeadbeef,
        0 /* EBX */, 0x0badf00d, 0x12121212, 0x34343434,
    };
    what = "popal (61) skips the esp slot";
    reset(A(TOP));
    for (int i = 0; i < 8; i++)
        put32(TOP + 4 * i, i == 4 ? A(C) : frame32[i]);
    snap(TOP);
    good = no_fault(what, run(popa32, EBX));
    for (int i = 0; i < 8; i++)
        want[order[7 - i]] = i == 4 ? A(C) : frame32[i];
    want[ESP] = A(TOP) + 32;
    good = good && check_regs(what, C->out, want) && check_win(what);
    ok(what, good);

    // Partway into the unmapped page: nothing moves.
    what = "pushaw faulting at its fifth word";
    reset(A(stk_lo) + 8);
    f = run(pusha16, EBX);
    memcpy(want, C->in, sizeof want);
    want[EBX] = A(C);
    good = check_fault_at(what, f, pusha16_insn, want);
    ok(what, good);

    what = "pushal faulting at its fifth dword";
    reset(A(stk_lo) + 16);
    f = run(pusha32, EBX);
    memcpy(want, C->in, sizeof want);
    want[EBX] = A(C);
    good = check_fault_at(what, f, pusha32_insn, want);
    ok(what, good);

    what = "popaw faulting at its fifth word";
    reset(A(stk_hi) - 8);
    for (int i = 0; i < 4; i++)
        put16(stk_hi - 8 + 2 * i, frame16[i]);
    f = run(popa16, EBX);
    good = check_fault_esp(what, f, popa16_insn, A(stk_hi) - 8);
    ok(what, good);

    what = "popal faulting at its fifth dword";
    reset(A(stk_hi) - 16);
    for (int i = 0; i < 4; i++)
        put32(stk_hi - 16 + 4 * i, frame32[i]);
    f = run(popa32, EBX);
    good = check_fault_esp(what, f, popa32_insn, A(stk_hi) - 16);
    ok(what, good);

    what = "popal with esp in the unmapped page below";
    reset(A(stk_lo) - 16);
    f = run(popa32, EBX);
    memcpy(want, C->in, sizeof want);
    want[EBX] = A(C);
    good = check_fault_at(what, f, popa32_insn, want);
    ok(what, good);
}

// ---- LEAVE -------------------------------------------------------------------

static void test_leave(void) {
    uint32_t want[8];
    int good;
    const char *what;

    // ESP starts far from EBP, in another 64 KiB: LEAVE copies all of EBP.
    what = "leavew (66 c9) sets all of esp from ebp";
    reset(0x00000100);
    C->in[EBP] = A(TOP);
    put16(TOP, 0x6b7a);
    put16(TOP + 2, 0x8c9d);
    snap(TOP);
    good = no_fault(what, run(leave16, EBX));
    memcpy(want, C->in, sizeof want);
    want[EBX] = A(C);
    want[ESP] = A(TOP) + 2;
    want[EBP] = (A(TOP) & 0xffff0000) | 0x6b7a;
    good = good && check_regs(what, C->out, want) && check_win(what);
    ok(what, good);

    what = "leave (c9)";
    reset(0x00000100);
    C->in[EBP] = A(TOP);
    put32(TOP, 0x89abcdef);
    snap(TOP);
    good = no_fault(what, run(leave32, EBX));
    memcpy(want, C->in, sizeof want);
    want[EBX] = A(C);
    want[ESP] = A(TOP) + 4;
    want[EBP] = 0x89abcdef;
    good = good && check_regs(what, C->out, want) && check_win(what);
    ok(what, good);
}

// ---- CALL, RET and JMP -------------------------------------------------------

// A near transfer to a word-sized target: SIGSEGV at that address, which
// nothing can map, with ESP as the instruction left it.
static int check_jump(const char *what, int faulted, uint32_t target, uint32_t esp) {
    if (!faulted)
        return fail(what, "no signal (esp after %#x, want SIGSEGV at %#x)",
                    C->out[ESP], target);
    int good = 1;
    if (F.sig != SIGSEGV)
        good = fail(what, "signal %d, want SIGSEGV", F.sig);
    if (F.eip != target)
        good = fail(what, "eip %#x, want %#x", F.eip, target);
    if (F.addr != target)
        good = fail(what, "si_addr %#x, want %#x", F.addr, target);
    if (F.code != SEGV_MAPERR)
        good = fail(what, "si_code %d, want SEGV_MAPERR", F.code);
    if (F.esp != esp)
        good = fail(what, "esp %#x, want %#x", F.esp, esp);
    return good;
}

static void test_call_ret(void) {
    int good, f;
    const char *what;

    what = "callw rel16 (66 e8)";
    reset(A(TOP));
    snap(TOP);
    f = run(call16_rel, EBX);
    want16(TOP - 2, A(call16_rel_end));
    good = check_jump(what, f, (A(call16_rel_end) + 0x1234) & 0xffff, A(TOP) - 2);
    good = check_win(what) && good;
    ok(what, good);

    what = "callw *%ax (66 ff /2)";
    reset(A(TOP));
    C->in[EAX] = 0x55552468;
    snap(TOP);
    f = run(call16_reg, EBX);
    want16(TOP - 2, A(call16_reg_end));
    good = check_jump(what, f, 0x2468, A(TOP) - 2);
    good = check_win(what) && good;
    ok(what, good);

    what = "callw *(%ecx) (66 ff /2)";
    reset(A(TOP));
    put16(C->data + 8, 0x3579);
    C->in[ECX] = A(C->data + 8);
    snap(TOP);
    f = run(call16_mem, EBX);
    want16(TOP - 2, A(call16_mem_end));
    good = check_jump(what, f, 0x3579, A(TOP) - 2);
    good = check_win(what) && good;
    ok(what, good);

    what = "retw (66 c3)";
    reset(A(TOP));
    put16(TOP, 0x4321);
    put16(TOP + 2, 0x0807);
    snap(TOP);
    f = run(ret16, EBX);
    good = check_jump(what, f, 0x4321, A(TOP) + 2);
    good = check_win(what) && good;
    ok(what, good);

    what = "retw $6 (66 c2 iw)";
    reset(A(TOP));
    put16(TOP, 0x4321);
    snap(TOP);
    f = run(ret16_imm, EBX);
    good = check_jump(what, f, 0x4321, A(TOP) + 8);
    good = check_win(what) && good;
    ok(what, good);

    what = "jmpw *%ax (66 ff /4)";
    reset(A(TOP));
    C->in[EAX] = 0x55551357;
    snap(TOP);
    f = run(jmp16_reg, EBX);
    good = check_jump(what, f, 0x1357, A(TOP));
    good = check_win(what) && good;
    ok(what, good);

    what = "jmpw *(%ecx) (66 ff /4)";
    reset(A(TOP));
    put16(C->data + 8, 0x2b1a);
    C->in[ECX] = A(C->data + 8);
    snap(TOP);
    f = run(jmp16_mem, EBX);
    good = check_jump(what, f, 0x2b1a, A(TOP));
    good = check_win(what) && good;
    ok(what, good);

    // Faults before the transfer leave ESP alone.
    what = "callw *%ax with esp at the bottom of the mapping";
    reset(A(stk_lo));
    C->in[EAX] = 0x55552468;
    f = run(call16_reg, EBX);
    uint32_t want[8];
    memcpy(want, C->in, sizeof want);
    want[EBX] = A(C);
    good = check_fault_at(what, f, call16_reg_insn, want);
    ok(what, good);

    what = "retw with esp at the top of the mapping";
    reset(A(stk_hi));
    f = run(ret16, EBX);
    memcpy(want, C->in, sizeof want);
    want[EBX] = A(C);
    good = check_fault_at(what, f, ret16_insn, want);
    ok(what, good);
}

// ---- FE /2 to /7 ------------------------------------------------------------

static void test_fe(void) {
    static const struct {
        const char *what;
        probe_fn *fn;
        const char *insn;
    } fe[] = {
        {"fe d0 (FE /2, a byte-sized call)", fe_call, fe_call_insn},
        {"fe 21 (FE /4, a byte-sized jmp)", fe_jmp, fe_jmp_insn},
        {"fe 31 (FE /6, a byte-sized push)", fe_push, fe_push_insn},
        {"fe f8 (FE /7)", fe_7, fe_7_insn},
    };
    for (size_t i = 0; i < sizeof fe / sizeof fe[0]; i++) {
        reset(A(TOP));
        C->in[ECX] = A(C->data + 8);
        snap(TOP);
        int f = run(fe[i].fn, EBX);
        int good = 1;
        if (!f) {
            good = fail(fe[i].what, "no signal (esp after %#x, want SIGILL)", C->out[ESP]);
        } else {
            if (F.sig != SIGILL)
                good = fail(fe[i].what, "signal %d, want SIGILL", F.sig);
            if (F.code != ILL_ILLOPN)
                good = fail(fe[i].what, "si_code %d, want ILL_ILLOPN", F.code);
            if (F.eip != A(fe[i].insn))
                good = fail(fe[i].what, "eip %#x, want %#x", F.eip, A(fe[i].insn));
            if (F.esp != A(TOP))
                good = fail(fe[i].what, "esp %#x, want %#x", F.esp, A(TOP));
        }
        good = check_win(fe[i].what) && good;
        ok(fe[i].what, good);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));

    pg = sysconf(_SC_PAGESIZE);
    map = mmap(NULL, 5 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    if (mprotect(map + pg, pg, PROT_NONE) != 0 ||
        mprotect(map + 4 * pg, pg, PROT_NONE) != 0) {
        perror("mprotect");
        return 1;
    }
    C = (struct ctx *) map;
    stk_lo = map + 2 * pg;
    stk_mid = map + 3 * pg;
    stk_hi = map + 4 * pg;
    TOP = stk_mid + pg / 2;

    // The probes run with ESP in the window, sometimes at an unmapped edge,
    // so the handler needs a stack of its own.
    static char altstack[64 * 1024];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack};
    if (sigaltstack(&ss, NULL) != 0) {
        perror("sigaltstack");
        return 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);

    test_controls();
    test_push_pop_reg();
    test_push_imm_mem();
    test_edges();
    test_pusha_popa();
    test_leave();
    test_fe();
    test_call_ret();

    return finish_suite("i386_push16");
}
#endif
