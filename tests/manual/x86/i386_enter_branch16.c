// i386_enter_branch16 -- ENTER at both operand sizes, and the 0x66 near
// branches, which truncate EIP to 16 bits.
//
// ENTER (C8 iw ib) was not decoded on AOK's i386 JIT at all: SIGILL. As x86
// does it (Intel SDM, ENTER): push EBP (BP with 0x66); FrameTemp = ESP; for a
// nesting level L > 0 (the byte mod 32), push the L-1 frame pointers at
// EBP-4, EBP-8, ... (EBP-2, -4, ... with 0x66) and then FrameTemp; EBP (BP)
// = FrameTemp; ESP -= the allocation. The JIT does every load and store
// before ESP or EBP moves, so a fault -- the push into an unmapped page, a
// frame pointer read from one -- leaves them as they were, with EIP on the
// instruction, as camd has it.
//
// With the 0x66 prefix, a taken JMP rel16/rel8, Jcc rel8/rel16, LOOP, LOOPE,
// LOOPNE and JCXZ jump to the low 16 bits of the target (SDM: IF OperandSize
// = 16 THEN tempEIP AND 0000FFFFH). Below 64 KiB nothing is mapped, so each is
// SIGSEGV SEGV_MAPERR there, with EIP there. The JIT jumped to the whole
// target. Each probe here puts a ud2 at the whole target, so a branch that
// does not truncate is SIGILL rather than a silent jump. A branch not taken
// falls through as ever.
//
// Measured on camd (x86_64 Linux 6.12, gcc -m32), a 32-bit task. The harness
// is i386_push16's: each probe loads all eight registers from a context
// block, runs the instruction, and stores them back.
//
// i386 only.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
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
    printf("i386_enter_branch16: SKIP (i386 only)\n");
    return 0;
}
#else

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
};
_Static_assert(offsetof(struct ctx, out) == 0x20, "asm offsets");
_Static_assert(offsetof(struct ctx, real_esp) == 0x40, "asm offsets");

#define HIDDEN __attribute__((visibility("hidden")))
#define LABEL(name) ".globl " #name "\n\t.hidden " #name "\n" #name ":\n\t"
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

// A taken branch whose operand-size-16 target is its next instruction's low
// word (plus `disp`): `pre` sets up the flags, `br` is the branch, `name_next`
// labels the instruction after it, and two ud2 sit at the whole target.
#define BRANCH(name, pre, br)                                                  \
    PROBE(name, "ebx", pre br "\n" LABEL(name##_next) ".byte 0x0f, 0x0b, 0x0f, 0x0b")

__asm__(
    ".pushsection .text\n\t"
    PROBE(c_push32_ebp, "ebx", ".byte 0x55")                        // push %ebp
    PROBE(enter32_l0, "ebx", ".byte 0xc8, 0x08, 0x00, 0x00")
    PROBE(enter32_l1, "ebx", ".byte 0xc8, 0x08, 0x00, 0x01")
    PROBE(enter32_l2, "ebx", ".byte 0xc8, 0x0c, 0x00, 0x02")
    PROBE(enter32_l3, "ebx", ".byte 0xc8, 0x00, 0x00, 0x03")
    PROBE(enter32_l33, "ebx", ".byte 0xc8, 0x04, 0x00, 0x21")       // level 33 = 1
    PROBE(enter16_l0, "ebx", ".byte 0x66, 0xc8, 0x08, 0x00, 0x00")
    PROBE(enter16_l1, "ebx", ".byte 0x66, 0xc8, 0x06, 0x00, 0x01")
    PROBE(enter16_l3, "ebx", ".byte 0x66, 0xc8, 0x00, 0x00, 0x03")

    PROBE(c_jmp32_rel8, "ebx", ".byte 0xeb, 0x00")                   // falls to the next
    BRANCH(jmp16_rel16, "", ".byte 0x66, 0xe9, 0x02, 0x00")          // +2: the second ud2
    BRANCH(jmp16_rel8, "", ".byte 0x66, 0xeb, 0x00")
    BRANCH(je16_rel8, ".byte 0x39, 0xc0\n\t", ".byte 0x66, 0x74, 0x00")  // cmp %eax,%eax
    BRANCH(je16_rel16, ".byte 0x39, 0xc0\n\t", ".byte 0x66, 0x0f, 0x84, 0x00, 0x00")
    BRANCH(jcxz16, "", ".byte 0x66, 0xe3, 0x00")
    BRANCH(loop16, "", ".byte 0x66, 0xe2, 0x00")
    BRANCH(loopz16, ".byte 0x39, 0xc0\n\t", ".byte 0x66, 0xe1, 0x00")
    BRANCH(loopnz16, ".byte 0x85, 0xe4\n\t", ".byte 0x66, 0xe0, 0x00")  // test %esp,%esp
    PROBE(jne16_rel16_not, "ebx", ".byte 0x39, 0xc0, 0x66, 0x0f, 0x85, 0x00, 0x00")
    PROBE(loop16_not, "ebx", ".byte 0x66, 0xe2, 0x00")               // ecx 1 -> 0
    ".popsection\n"
);

typedef void probe_fn(struct ctx *);
#define DECL(name)                                                             \
    extern probe_fn name HIDDEN;                                               \
    extern const char name##_insn[] HIDDEN, name##_end[] HIDDEN;
#define DECLB(name) DECL(name) extern const char name##_next[] HIDDEN;
DECL(c_push32_ebp) DECL(enter32_l0) DECL(enter32_l1) DECL(enter32_l2) DECL(enter32_l3)
DECL(enter32_l33) DECL(enter16_l0) DECL(enter16_l1) DECL(enter16_l3)
DECL(c_jmp32_rel8) DECLB(jmp16_rel16) DECLB(jmp16_rel8) DECLB(je16_rel8) DECLB(je16_rel16)
DECLB(jcxz16) DECLB(loop16) DECLB(loopz16) DECLB(loopnz16)
DECL(jne16_rel16_not) DECL(loop16_not)

// Page 0 the context, page 1 unmapped, pages 2 and 3 the stack window, page 4
// unmapped.
static long pg;
static uint8_t *map;
static struct ctx *C;
static uint8_t *stk_lo, *TOP;
#define A(p) ((uint32_t) (uintptr_t) (p))

static const uint32_t init_regs[8] = {
    0x4a5b6c7d, 0x3c4d5e6f, 0x2e3f4051, 0x16273849,
    0, 0x0b1c2d3e, 0x6e7f8091, 0x7d8e9fa0,
};

static void reset(uint32_t esp, uint32_t ebp) {
    // Every word of the window a distinct value, so a copied frame pointer
    // says which one it was.
    for (long i = 0; i < 2 * pg; i += 4) {
        uint32_t v = 0xf0000000u | (uint32_t) i;
        memcpy(stk_lo + i, &v, 4);
    }
    memcpy(C->in, init_regs, sizeof init_regs);
    C->in[ESP] = esp;
    C->in[EBP] = ebp;
    memset(C->out, 0, sizeof C->out);
}

static uint32_t get32(uint32_t addr) { uint32_t v; memcpy(&v, (void *) (uintptr_t) addr, 4); return v; }
static uint32_t get16(uint32_t addr) { uint16_t v; memcpy(&v, (void *) (uintptr_t) addr, 2); return v; }

static sigjmp_buf env;
static struct {
    int sig, code;
    uint32_t addr, eip, regs[8];
} F;

static void on_fault(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    greg_t *g = uc->uc_mcontext.gregs;
    F.sig = sig;
    F.code = si->si_code;
    F.addr = A(si->si_addr);
    F.eip = g[REG_EIP];
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

static int run(probe_fn *fn) {
    C->in[EBX] = A(C);
    memset(&F, 0, sizeof F);
    if (sigsetjmp(env, 1) == 0) {
        fn(C);
        return 0;
    }
    return 1;
}

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
    return fail(what, "signal %d (si_code %d, addr %#x) at eip %#x", F.sig, F.code, F.addr, F.eip);
}

static int check_reg(const char *what, const char *which, uint32_t got, uint32_t want) {
    if (got == want)
        return 1;
    return fail(what, "%s = %#010x, want %#010x", which, got, want);
}

static void ok(const char *what, int good) {
    if (good)
        test_logf("  ok   %s\n", what);
}

// ---- ENTER --------------------------------------------------------------------

// What ENTER alloc, level at `bytes` (4, or 2 with 0x66) leaves, from the SDM,
// checked against the probe's registers and the stack window.
static void enter_case(const char *what, probe_fn *fn, int bytes, uint32_t alloc, int level) {
    uint32_t esp0 = A(TOP), ebp0 = A(TOP) + 0x100;
    reset(esp0, ebp0);
    int good = no_fault(what, run(fn));
    if (!good)
        return;
    level %= 32;
    uint32_t frame = esp0 - bytes;
    int pushes = level == 0 ? 1 : level + 1;
    uint32_t want_esp = esp0 - bytes * pushes - alloc;
    uint32_t want_ebp = bytes == 4 ? frame : (ebp0 & 0xffff0000u) | (frame & 0xffff);
    good &= check_reg(what, "esp", C->out[ESP], want_esp);
    good &= check_reg(what, "ebp", C->out[EBP], want_ebp);
    for (int r = 0; r < 8; r++) {
        if (r != ESP && r != EBP && r != EBX)
            good &= check_reg(what, rname[r], C->out[r], init_regs[r]);
    }
    uint32_t (*get)(uint32_t) = bytes == 4 ? get32 : get16;
    uint32_t mask = bytes == 4 ? 0xffffffffu : 0xffff;
    // The old frame pointer, then the copied ones, then FrameTemp.
    if (get(esp0 - bytes) != (ebp0 & mask))
        good = fail(what, "saved frame pointer %#x, want %#x", get(esp0 - bytes), ebp0 & mask);
    for (int i = 1; i < level; i++) {
        uint32_t at = esp0 - bytes - (uint32_t) (bytes * i);
        // The word at ebp0 - bytes*i, which ENTER only reads.
        uint32_t want = get(ebp0 - (uint32_t) (bytes * i));
        if (get(at) != (want & mask))
            good = fail(what, "frame pointer %d at %#x is %#x, want %#x", i, at, get(at), want & mask);
    }
    if (level > 0) {
        uint32_t at = esp0 - bytes * (uint32_t) (level + 1);
        if (get(at) != (frame & mask))
            good = fail(what, "FrameTemp at %#x is %#x, want %#x", at, get(at), frame & mask);
    }
    ok(what, good);
}

// ENTER whose first push, or a frame pointer's read, faults: nothing moves.
static void enter_fault(const char *what, probe_fn *fn, const char *insn, uint32_t esp,
                        uint32_t ebp) {
    reset(esp, ebp);
    int faulted = run(fn);
    if (!faulted) {
        fail(what, "no signal (esp after %#x, want a SIGSEGV)", C->out[ESP]);
        return;
    }
    int good = 1;
    if (F.sig != SIGSEGV)
        good = fail(what, "signal %d, want SIGSEGV", F.sig);
    if (F.eip != A(insn))
        good = fail(what, "eip %#x, want %#x (the instruction)", F.eip, A(insn));
    good &= check_reg(what, "esp", F.regs[ESP], esp);
    good &= check_reg(what, "ebp", F.regs[EBP], ebp);
    ok(what, good);
}

static void test_enter(void) {
    // The control: push %ebp in the same harness.
    reset(A(TOP), 0x12345678);
    int good = no_fault("control: push %ebp", run(c_push32_ebp));
    if (good) {
        good &= check_reg("control: push %ebp", "esp", C->out[ESP], A(TOP) - 4);
        if (get32(A(TOP) - 4) != 0x12345678)
            good = fail("control: push %ebp", "stored %#x", get32(A(TOP) - 4));
    }
    ok("control: push %ebp", good);

    enter_case("enter $8, $0", enter32_l0, 4, 8, 0);
    enter_case("enter $8, $1", enter32_l1, 4, 8, 1);
    enter_case("enter $12, $2", enter32_l2, 4, 12, 2);
    enter_case("enter $0, $3", enter32_l3, 4, 0, 3);
    enter_case("enter $4, $33 (level mod 32)", enter32_l33, 4, 4, 33);
    enter_case("enterw $8, $0", enter16_l0, 2, 8, 0);
    enter_case("enterw $6, $1", enter16_l1, 2, 6, 1);
    enter_case("enterw $0, $3", enter16_l3, 2, 0, 3);

    // The push of EBP into the unmapped page below the window.
    enter_fault("enter $8, $0 pushing into an unmapped page", enter32_l0, enter32_l0_insn,
                A(stk_lo) + 2, A(TOP));
    // A frame pointer read from the unmapped page: EBP just above it.
    enter_fault("enter $12, $2 reading a frame pointer from an unmapped page", enter32_l2,
                enter32_l2_insn, A(TOP), A(stk_lo) + 2);
}

// ---- 16-bit branches ------------------------------------------------------------

static void branch_case(const char *what, probe_fn *fn, const char *next, uint32_t disp,
                        uint32_t ecx, uint32_t want_ecx) {
    reset(A(TOP), A(TOP));
    C->in[ECX] = ecx;
    int faulted = run(fn);
    uint32_t target = (A(next) + disp) & 0xffff;
    if (!faulted) {
        fail(what, "no signal, want SIGSEGV at %#x", target);
        return;
    }
    int good = 1;
    if (F.sig != SIGSEGV)
        good = fail(what, "signal %d at eip %#x, want SIGSEGV at %#x (the whole target is %#x)",
                    F.sig, F.eip, target, A(next) + disp);
    else {
        if (F.code != SEGV_MAPERR)
            good = fail(what, "si_code %d, want SEGV_MAPERR", F.code);
        if (F.addr != target)
            good = fail(what, "si_addr %#x, want %#x", F.addr, target);
        if (F.eip != target)
            good = fail(what, "eip %#x, want %#x", F.eip, target);
        good &= check_reg(what, "esp", F.regs[ESP], A(TOP));
        good &= check_reg(what, "ecx", F.regs[ECX], want_ecx);
    }
    ok(what, good);
}

static void test_branches(void) {
    reset(A(TOP), A(TOP));
    ok("control: jmp rel8 (32-bit) falls to its target", no_fault("control: jmp rel8", run(c_jmp32_rel8)));

    branch_case("jmpw rel16 +2", jmp16_rel16, jmp16_rel16_next, 2, 7, 7);
    branch_case("jmpw rel8", jmp16_rel8, jmp16_rel8_next, 0, 7, 7);
    branch_case("jew rel8, taken", je16_rel8, je16_rel8_next, 0, 7, 7);
    branch_case("jew rel16, taken", je16_rel16, je16_rel16_next, 0, 7, 7);
    branch_case("jcxzw, ecx 0", jcxz16, jcxz16_next, 0, 0, 0);
    branch_case("loopw, ecx 2", loop16, loop16_next, 0, 2, 1);
    branch_case("loopew, ecx 2 and ZF", loopz16, loopz16_next, 0, 2, 1);
    branch_case("loopnew, ecx 2 and not ZF", loopnz16, loopnz16_next, 0, 2, 1);

    reset(A(TOP), A(TOP));
    int good = no_fault("jnew rel16, not taken", run(jne16_rel16_not));
    ok("jnew rel16, not taken, falls through", good);
    reset(A(TOP), A(TOP));
    C->in[ECX] = 1;
    good = no_fault("loopw, ecx 1", run(loop16_not));
    if (good)
        good = check_reg("loopw, ecx 1", "ecx", C->out[ECX], 0);
    ok("loopw, ecx 1: not taken, ecx 0", good);
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
    if (mprotect(map + pg, pg, PROT_NONE) != 0 || mprotect(map + 4 * pg, pg, PROT_NONE) != 0) {
        perror("mprotect");
        return 1;
    }
    C = (struct ctx *) map;
    stk_lo = map + 2 * pg;
    TOP = map + 3 * pg + pg / 2;

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

    test_enter();
    test_branches();
    return finish_suite("i386_enter_branch16");
}
#endif
