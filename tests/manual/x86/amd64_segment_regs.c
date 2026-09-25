// amd64 segment registers: MOV r/m, Sreg (8C), MOV Sreg, r/m (8E), and PUSH /
// POP FS and GS (0F A0, A1, A8, A9).
//
// None of them existed in either amd64 engine; every one raised SIGILL. .NET
// found it. The PAL's CONTEXT_CaptureContext stores CS and SS into a CONTEXT
// on every managed exception -- `8c 4f 38`, mov %cs,0x38(%rdi) -- so on
// alpine-amd64 with dotnet8-sdk, `dotnet new console` died with rc 132 while
// `dotnet --info`, which never throws, ran. Its RtlRestoreContext then hands
// those two values to IRETQ, so they have to be the real ones: CS 0x33 and
// SS 0x2b.
//
// Everything below was measured on x86_64 Linux 6.12 (camd, AMD Zen+) and
// passes there unchanged:
//
//  - CS reads 0x33 and SS 0x2b; ES, DS, FS and GS read 0 from exec on, then
//    whatever was last loaded, which survives a signal handler and fork.
//  - A memory destination takes exactly two bytes whatever the operand size,
//    REX.W included. A register destination takes the operand size: a 32-bit
//    write zero-extends to 64, REX.W zero-extends, 66 keeps bits 16-63.
//  - REX.R does not extend the Sreg field. Encodings 6 and 7 are #UD (with a
//    memory operand too), and so is CS as a destination; so is LOCK.
//  - A load takes the low word of a register or two bytes of memory. PUSH
//    FS/GS stores eight bytes zero-extended (two with 66); POP takes eight and
//    keeps the low word.
//  - Loadable: null, 0x20-0x23, 0x28-0x2b, 0x30-0x33 and 0x78-0x7b (GDT
//    entries 4, 5, 6 and 15) into ES, DS, FS or GS, any RPL; SS takes 0x2b and
//    nothing else, null included. The rest -- kernel entries, TSS/LDT
//    descriptors, the TLS slots a 64-bit task has not filled, anything past
//    the GDT and every LDT selector -- is #GP: SIGSEGV, si_code SI_KERNEL,
//    reported at the instruction, and a POP that faults leaves RSP alone.
//  - Loading a non-null selector into FS sets the FS base to 0. A null one
//    clears it on Intel and keeps it on AMD parts without CPUID
//    0x80000021:EAX[6] (Linux's X86_BUG_NULL_SEG) -- camd keeps it, and iSH-AOK
//    reports GenuineIntel and clears it -- so that one expectation follows the
//    CPUID vendor.
//  - A signal frame's REG_CSGSFS is 0x002b000000000033 (CS and SS; the GS and
//    FS words are 0 whatever is loaded) and uc_flags has all of
//    UC_FP_XSTATE | UC_SIGCONTEXT_SS | UC_STRICT_RESTORE_SS.
//  - PTRACE_GETREGS reports the loaded ES, DS, FS and GS, and SETREGS changes
//    them.
//  - IRETQ, which .NET's RtlRestoreContext ends in and which pops the CS and SS
//    the 8C stores captured: CS must be 0x33 and SS 0x2b (low word only), and
//    anything else -- or a non-canonical RIP -- is #GP at the IRETQ with RSP
//    still at the frame. RFLAGS takes the arithmetic flags, DF, AC and ID; IF,
//    IOPL, VM, VIF, VIP and the high half stay as they were.
//
// Not asserted here: si_addr of a #GP (NULL) and its error code in REG_ERR
// (the selector), which x86/gpf_siginfo checks; TF and NT through IRETQ;
// IRETD/IRETW (their 32- and 16-bit RIP and RSP would need code and stack
// below 4 GiB); and IRETQ to CS 0x23, which Linux turns into a switch to
// 32-bit code. The 67-prefixed forms are here because the JIT leaves an
// address-size prefix to the interpreter, so they are what exercises the
// interpreter's copy of these instructions.
//
// x86_64 only; the i386 frontend is a different decoder.
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../test_common.h"

static const char *suite = "amd64_segment_regs";

#if defined(__x86_64__)

#include <cpuid.h>
#include <ucontext.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>

#ifndef SI_KERNEL
#define SI_KERNEL 0x80
#endif
#ifndef SEGV_MAPERR
#define SEGV_MAPERR 1
#endif
#define ARCH_SET_FS_ 0x1002
#define ARCH_GET_FS_ 0x1003
#define PATTERN 0xdeadbeefcafef00dULL

static sigjmp_buf env;
static volatile int f_sig, f_code;
static volatile uint64_t f_addr, f_rip, f_rsp;
// Each probe stores the address of the instruction that may fault here just
// before running it, so a fault can be checked to report that instruction
// and not the one after it.
static uint64_t expect_ip;
static uint64_t out0, out1;
static unsigned char membuf[128];
static uint16_t load_sel;
static uint64_t fs_base0;
static unsigned checks;

static void fault_handler(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    f_sig = sig;
    f_code = si->si_code;
    f_addr = (uintptr_t) si->si_addr;
    f_rip = (uint64_t) uc->uc_mcontext.gregs[REG_RIP];
    f_rsp = (uint64_t) uc->uc_mcontext.gregs[REG_RSP];
    siglongjmp(env, 1);
}

static int run(void (*fn)(void)) {
    f_sig = 0;
    if (sigsetjmp(env, 1) == 0)
        fn();
    return f_sig;
}

static void check_u64(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        printf("FAIL %s: got %#llx, want %#llx\n", what,
               (unsigned long long) got, (unsigned long long) want);
        failures_total++;
    } else {
        test_logf("  ok   %s = %#llx\n", what, (unsigned long long) got);
    }
}

static const char *signame(int sig) {
    return sig == 0 ? "no signal" : strsignal(sig);
}

// A probe that must complete without a signal.
static int run_clean(const char *what, void (*fn)(void)) {
    int sig = run(fn);
    checks++;
    if (sig != 0) {
        printf("FAIL %s: %s (si_code %d) at %#llx\n", what, signame(sig), f_code,
               (unsigned long long) f_rip);
        failures_total++;
        return 0;
    }
    return 1;
}

// A probe that must fault with want_sig/want_code, reported at expect_ip.
static void run_fault(const char *what, void (*fn)(void), int want_sig, int want_code) {
    int sig = run(fn);
    checks++;
    if (sig != want_sig) {
        printf("FAIL %s: %s, want %s\n", what, signame(sig), signame(want_sig));
        failures_total++;
        return;
    }
    if (f_code != want_code) {
        printf("FAIL %s: si_code %d, want %d\n", what, f_code, want_code);
        failures_total++;
    }
    if (f_rip != expect_ip) {
        printf("FAIL %s: reported at %#llx, the instruction is at %#llx\n", what,
               (unsigned long long) f_rip, (unsigned long long) expect_ip);
        failures_total++;
    }
    if (want_code == SEGV_MAPERR && f_addr != 0) {
        printf("FAIL %s: si_addr %#llx, want 0\n", what, (unsigned long long) f_addr);
        failures_total++;
    }
    test_logf("  ok   %s -> %s code %d\n", what, signame(sig), f_code);
}

// The address of the next instruction, stored before it runs.
#define MARK "lea 1f(%%rip), %%r11\n\tmov %%r11, %[ip]\n1:\t"

// ---- 8C to a register ----
#define REG_READ(name, bytes)                                                  \
    static void name(void) {                                                   \
        uint64_t r = PATTERN;                                                  \
        __asm__ volatile(bytes : "+a"(r));                                     \
        out0 = r;                                                              \
    }
REG_READ(rd_cs_eax, ".byte 0x8c, 0xc8")               // mov %cs,%eax
REG_READ(rd_ss_eax, ".byte 0x8c, 0xd0")               // mov %ss,%eax
REG_READ(rd_es_eax, ".byte 0x8c, 0xc0")
REG_READ(rd_ds_eax, ".byte 0x8c, 0xd8")
REG_READ(rd_fs_eax, ".byte 0x8c, 0xe0")
REG_READ(rd_gs_eax, ".byte 0x8c, 0xe8")
REG_READ(rd_cs_ax, ".byte 0x66, 0x8c, 0xc8")          // mov %cs,%ax
REG_READ(rd_cs_rax, ".byte 0x48, 0x8c, 0xc8")         // mov %cs,%rax
REG_READ(rd_ss_rax_66w, ".byte 0x66, 0x48, 0x8c, 0xd0") // REX.W beats 66
REG_READ(rd_cs_rexr, ".byte 0x44, 0x8c, 0xc8")        // REX.R: still CS
REG_READ(rd_ss_a32, ".byte 0x67, 0x8c, 0xd0")         // interpreter's copy

// Followed in the same block by more work, as .NET's is: the JIT continues
// its block after the bridge, and the results must still be there after it.
static void rd_cs_then_add(void) {
    uint64_t r = PATTERN;
    __asm__ volatile(".byte 0x8c, 0xc8\n\t"           // mov %cs,%eax
                     "addq $0x100, %%rax\n\t"
                     ".byte 0x8c, 0xd1\n\t"           // mov %ss,%ecx
                     "addq %%rcx, %%rax"
                     : "+a"(r) :: "rcx");
    out0 = r;
}
static void rd_cs_r8d(void) {
    register uint64_t r8 __asm__("r8") = PATTERN;
    __asm__ volatile(".byte 0x41, 0x8c, 0xc8" : "+r"(r8));       // mov %cs,%r8d
    out0 = r8;
}
static void rd_cs_r8w(void) {
    register uint64_t r8 __asm__("r8") = PATTERN;
    __asm__ volatile(".byte 0x66, 0x41, 0x8c, 0xc8" : "+r"(r8)); // mov %cs,%r8w
    out0 = r8;
}

// ---- 8C to memory: always two bytes ----
#define MEM_READ(name, bytes)                                                  \
    static void name(void) {                                                   \
        memset(membuf, 0xaa, sizeof(membuf));                                  \
        __asm__ volatile(bytes :: "D"(membuf) : "memory");                     \
    }
MEM_READ(mr_cs, ".byte 0x8c, 0x0f")                    // mov %cs,(%rdi)
MEM_READ(mr_cs_w, ".byte 0x48, 0x8c, 0x0f")            // rex.w: still 2 bytes
MEM_READ(mr_ss_66, ".byte 0x66, 0x8c, 0x17")           // mov %ss,(%rdi)
MEM_READ(mr_cs_rexr, ".byte 0x4c, 0x8c, 0x0f")         // REX.W+R
MEM_READ(mr_net, ".byte 0x8c, 0x4f, 0x38\n\t.byte 0x8c, 0x57, 0x42") // .NET's pair

static void check_membuf(const char *what, unsigned off, uint16_t want) {
    checks++;
    for (unsigned i = 0; i < sizeof(membuf); i++) {
        unsigned char w = 0xaa;
        if (i == off)
            w = want & 0xff;
        else if (i == off + 1)
            w = want >> 8;
        if (membuf[i] != w) {
            printf("FAIL %s: byte %u is %02x, want %02x\n", what, i, membuf[i], w);
            failures_total++;
            return;
        }
    }
    test_logf("  ok   %s stored %#x in two bytes\n", what, want);
}

// ---- #UD and memory faults ----
static void ud_control(void) {
    __asm__ volatile(MARK "ud2" : [ip] "=m"(expect_ip) :: "r11");
}
static void ud_8c_reg6(void) {
    __asm__ volatile(MARK ".byte 0x8c, 0xf0" : [ip] "=m"(expect_ip) :: "r11", "rax");
}
static void ud_8c_reg7(void) {
    __asm__ volatile(MARK ".byte 0x8c, 0xf8" : [ip] "=m"(expect_ip) :: "r11", "rax");
}
static void ud_8c_mem6(void) {
    __asm__ volatile(MARK ".byte 0x8c, 0x37" : [ip] "=m"(expect_ip) : "D"(membuf) : "r11", "memory");
}
static void ud_8c_lock(void) {
    __asm__ volatile(MARK ".byte 0xf0, 0x8c, 0x0f" : [ip] "=m"(expect_ip) : "D"(membuf) : "r11", "memory");
}
static void ud_8e_cs(void) {
    __asm__ volatile(MARK ".byte 0x8e, 0xc8" : [ip] "=m"(expect_ip) : "a"(0x33) : "r11");
}
static void ud_8e_rexr_cs(void) {
    __asm__ volatile(MARK ".byte 0x44, 0x8e, 0xc8" : [ip] "=m"(expect_ip) : "a"(0x33) : "r11");
}
static void ud_8e_reg6(void) {
    __asm__ volatile(MARK ".byte 0x8e, 0xf0" : [ip] "=m"(expect_ip) : "a"(0x2b) : "r11");
}
static void ud_8e_reg7(void) {
    __asm__ volatile(MARK ".byte 0x8e, 0xf8" : [ip] "=m"(expect_ip) : "a"(0x2b) : "r11");
}
// The legacy one-byte push/pop of CS, DS, ES and SS do not exist in 64-bit mode.
static void ud_push_cs(void) {
    __asm__ volatile(MARK ".byte 0x0e" : [ip] "=m"(expect_ip) :: "r11", "memory");
}
static void ud_push_es(void) {
    __asm__ volatile(MARK ".byte 0x06" : [ip] "=m"(expect_ip) :: "r11", "memory");
}
static void ud_pop_ds(void) {
    __asm__ volatile(MARK ".byte 0x1f" : [ip] "=m"(expect_ip) :: "r11", "memory");
}
static void ud_pop_ss(void) {
    __asm__ volatile(MARK ".byte 0x17" : [ip] "=m"(expect_ip) :: "r11", "memory");
}
static void pf_8c_null(void) {
    __asm__ volatile(MARK ".byte 0x8c, 0x08" : [ip] "=m"(expect_ip) : "a"(0) : "r11", "memory");
}
static void pf_8e_null(void) {
    __asm__ volatile(MARK ".byte 0x8e, 0x00" : [ip] "=m"(expect_ip) : "a"(0) : "r11");
}

// ---- 8E loads ----
static void ld_es(void) {
    unsigned got;
    __asm__ volatile(MARK "mov %[s], %%es\n\tmov %%es, %[g]\n\tmov %[z], %%es"
                     : [g] "=&r"(got), [ip] "=m"(expect_ip)
                     : [s] "r"((unsigned) load_sel), [z] "r"(0) : "r11");
    out0 = got;
}
static void ld_ds(void) {
    unsigned got;
    __asm__ volatile(MARK "mov %[s], %%ds\n\tmov %%ds, %[g]\n\tmov %[z], %%ds"
                     : [g] "=&r"(got), [ip] "=m"(expect_ip)
                     : [s] "r"((unsigned) load_sel), [z] "r"(0) : "r11");
    out0 = got;
}
static void ld_gs(void) {
    unsigned got;
    __asm__ volatile(MARK "mov %[s], %%gs\n\tmov %%gs, %[g]\n\tmov %[z], %%gs"
                     : [g] "=&r"(got), [ip] "=m"(expect_ip)
                     : [s] "r"((unsigned) load_sel), [z] "r"(0) : "r11");
    out0 = got;
}
static void ld_ss(void) {
    unsigned got, old;
    __asm__ volatile("mov %%ss, %[o]\n\t" MARK "mov %[s], %%ss\n\tmov %%ss, %[g]\n\tmov %[o], %%ss"
                     : [g] "=&r"(got), [o] "=&r"(old), [ip] "=m"(expect_ip)
                     : [s] "r"((unsigned) load_sel) : "r11");
    out0 = got;
}
// FS carries the TLS base, so nothing may touch TLS between the load and the
// raw arch_prctl that puts the base back -- which also leaves FS's selector 0,
// read back into out1.
static void ld_fs(void) {
    static uint64_t base_after;
    unsigned got, after;
    __asm__ volatile(MARK "mov %[s], %%fs\n\t"
                     "mov %%fs, %[g]\n\t"
                     "mov $0x1003, %%edi\n\t"
                     "lea %[b], %%rsi\n\t"
                     "mov $158, %%eax\n\t"
                     "syscall\n\t"
                     "mov $0x1002, %%edi\n\t"
                     "mov %[b0], %%rsi\n\t"
                     "mov $158, %%eax\n\t"
                     "syscall\n\t"
                     "mov %%fs, %[a]"
                     : [g] "=&r"(got), [a] "=&r"(after), [b] "=m"(base_after),
                       [ip] "=m"(expect_ip)
                     : [s] "r"((unsigned) load_sel), [b0] "r"(fs_base0)
                     : "rax", "rdi", "rsi", "rcx", "r11", "memory");
    out0 = got;
    out1 = base_after;
    membuf[0] = (unsigned char) after;
}
// The same through memory, two bytes of it, and through the interpreter.
static void ld_es_mem(void) {
    unsigned got;
    memset(membuf, 0xff, sizeof(membuf));
    membuf[0] = 0x2b;
    membuf[1] = 0;
    __asm__ volatile(".byte 0x48, 0x8e, 0x07\n\tmov %%es, %[g]\n\tmov %[z], %%es"
                     : [g] "=&r"(got) : "D"(membuf), [z] "r"(0) : "memory");
    out0 = got;
}
static void ld_es_high(void) {
    unsigned got;
    __asm__ volatile("mov %[s], %%es\n\tmov %%es, %[g]\n\tmov %[z], %%es"
                     : [g] "=&r"(got) : [s] "r"(0xffff0023u), [z] "r"(0));
    out0 = got;
}
static void ld_es_rexr(void) {
    unsigned got;
    __asm__ volatile(".byte 0x44, 0x8e, 0xc0\n\tmov %%es, %[g]\n\tmov %[z], %%es"
                     : [g] "=&r"(got) : "a"(0x2b), [z] "r"(0));
    out0 = got;
}
static void ld_ds_a32(void) {
    unsigned got;
    __asm__ volatile(".byte 0x67, 0x8e, 0xd8\n\tmov %%ds, %[g]\n\tmov %[z], %%ds"
                     : [g] "=&r"(got) : "a"(0x7b), [z] "r"(0));
    out0 = got;
}

// ---- PUSH / POP FS and GS ----
static void push_gs64(void) {
    uint64_t v;
    __asm__ volatile("sub $128, %%rsp\n\t"
                     "mov %[s], %%gs\n\t"
                     "movq $-1, -8(%%rsp)\n\t"
                     "push %%gs\n\t"
                     "pop %[v]\n\t"
                     "mov %[z], %%gs\n\t"
                     "add $128, %%rsp"
                     : [v] "=&r"(v) : [s] "r"(0x2b), [z] "r"(0) : "memory");
    out0 = v;
}
static void push_fs64(void) {
    uint64_t v;
    __asm__ volatile("sub $128, %%rsp\n\t"
                     "movq $-1, -8(%%rsp)\n\t"
                     "push %%fs\n\t"
                     "pop %[v]\n\t"
                     "add $128, %%rsp"
                     : [v] "=&r"(v) :: "memory");
    out0 = v;
}
static void push_gs16(void) {
    uint64_t sp0, sp1, v;
    __asm__ volatile("sub $128, %%rsp\n\t"
                     "mov %[s], %%gs\n\t"
                     "mov %%rsp, %[a]\n\t"
                     "movq $-1, -8(%%rsp)\n\t"
                     ".byte 0x66, 0x0f, 0xa8\n\t"        // pushw %gs
                     "mov %%rsp, %[b]\n\t"
                     "mov -6(%%rsp), %[v]\n\t"
                     "add $2, %%rsp\n\t"
                     "mov %[z], %%gs\n\t"
                     "add $128, %%rsp"
                     : [a] "=&r"(sp0), [b] "=&r"(sp1), [v] "=&r"(v)
                     : [s] "r"(0x23), [z] "r"(0) : "memory");
    out0 = sp0 - sp1;
    out1 = v;
}
static void pop_gs64(void) {
    uint64_t sp0, sp1;
    unsigned got;
    __asm__ volatile("sub $128, %%rsp\n\t"
                     "mov %%rsp, %[a]\n\t"
                     "push %[v]\n\t"
                     "pop %%gs\n\t"
                     "mov %%rsp, %[b]\n\t"
                     "mov %%gs, %[g]\n\t"
                     "mov %[z], %%gs\n\t"
                     "add $128, %%rsp"
                     : [a] "=&r"(sp0), [b] "=&r"(sp1), [g] "=&r"(got)
                     : [v] "r"(0xffffffff0000002bULL), [z] "r"(0) : "memory");
    out0 = got;
    out1 = sp1 - sp0;
}
static void pop_gs16(void) {
    uint64_t sp0, sp1;
    unsigned got;
    __asm__ volatile("sub $128, %%rsp\n\t"
                     "mov %%rsp, %[a]\n\t"
                     "pushw $0x23\n\t"
                     ".byte 0x66, 0x0f, 0xa9\n\t"        // popw %gs
                     "mov %%rsp, %[b]\n\t"
                     "mov %%gs, %[g]\n\t"
                     "mov %[z], %%gs\n\t"
                     "add $128, %%rsp"
                     : [a] "=&r"(sp0), [b] "=&r"(sp1), [g] "=&r"(got)
                     : [z] "r"(0) : "memory");
    out0 = got;
    out1 = sp1 - sp0;
}
static void pop_fs64(void) {
    static uint64_t base_after;
    unsigned got;
    __asm__ volatile("sub $128, %%rsp\n\t"
                     "push %[v]\n\t"
                     "pop %%fs\n\t"
                     "mov %%fs, %[g]\n\t"
                     "mov $0x1003, %%edi\n\t"
                     "lea %[b], %%rsi\n\t"
                     "mov $158, %%eax\n\t"
                     "syscall\n\t"
                     "mov $0x1002, %%edi\n\t"
                     "mov %[b0], %%rsi\n\t"
                     "mov $158, %%eax\n\t"
                     "syscall\n\t"
                     "add $128, %%rsp"
                     : [g] "=&r"(got), [b] "=m"(base_after)
                     : [v] "r"((uint64_t) 0x33), [b0] "r"(fs_base0)
                     : "rax", "rdi", "rsi", "rcx", "r11", "memory");
    out0 = got;
    out1 = base_after;
}
// #GP from a POP: the stack pointer at the fault is the one before the POP.
static void pop_gs_bad(void) {
    __asm__ volatile("sub $128, %%rsp\n\t"
                     "push %[v]\n\t"
                     "mov %%rsp, %[sp]\n\t"
                     MARK "pop %%gs\n\t"
                     "mov %[z], %%gs\n\t"
                     "add $128, %%rsp"
                     : [sp] "=m"(out1), [ip] "=m"(expect_ip)
                     : [v] "r"((uint64_t) 0x10), [z] "r"(0) : "r11", "memory");
}

// ---- IRETQ ----
static uint64_t iret_cs, iret_ss, iret_flags, iret_target;
static uint64_t iret_want_rsp, iret_out_rsp, iret_out_flags;

// Builds an IRETQ frame whose RSP is a fresh 16-byte-aligned spot and whose
// RIP is label 2 (or iret_target), and runs it. Label 2 records RSP and
// RFLAGS, then clears DF, TF, NT, RF and AC before any C code runs again. The
// frame's address goes to out1 for the fault case.
static void do_iretq(void) {
    __asm__ volatile("mov %%rsp, %%r12\n\t"
                     "sub $256, %%rsp\n\t"
                     "and $-16, %%rsp\n\t"
                     "mov %%rsp, %%r13\n\t"
                     "mov %%r13, %[want]\n\t"
                     "sub $64, %%rsp\n\t"
                     "mov %[tgt], %%rax\n\t"
                     "test %%rax, %%rax\n\t"
                     "jnz 3f\n\t"
                     "lea 2f(%%rip), %%rax\n"
                     "3:\n\t"
                     "push %[ss]\n\t"
                     "push %%r13\n\t"
                     "push %[fl]\n\t"
                     "push %[cs]\n\t"
                     "push %%rax\n\t"
                     "mov %%rsp, %[frame]\n\t"
                     MARK "iretq\n\t"
                     "ud2\n"
                     "2:\n\t"
                     "mov %%rsp, %[out_rsp]\n\t"
                     "pushfq\n\t"
                     "pop %[out_fl]\n\t"
                     "pushfq\n\t"
                     "andq $~0x54500, (%%rsp)\n\t"
                     "popfq\n\t"
                     "mov %%r12, %%rsp"
                     : [out_rsp] "=m"(iret_out_rsp), [out_fl] "=m"(iret_out_flags),
                       [want] "=m"(iret_want_rsp), [frame] "=m"(out1), [ip] "=m"(expect_ip)
                     : [cs] "r"(iret_cs), [ss] "r"(iret_ss), [fl] "r"(iret_flags),
                       [tgt] "r"(iret_target)
                     : "rax", "r11", "r12", "r13", "memory", "cc");
}

static void check_iretq(const char *what, uint64_t cs, uint64_t ss, uint64_t flags,
        uint64_t target, int want_ok, uint64_t want_flags) {
    iret_cs = cs;
    iret_ss = ss;
    iret_flags = flags;
    iret_target = target;
    iret_out_rsp = iret_out_flags = 0;
    if (!want_ok) {
        run_fault(what, do_iretq, SIGSEGV, SI_KERNEL);
        if (f_sig == SIGSEGV) {
            char label[96];
            snprintf(label, sizeof label, "%s: rsp at the fault", what);
            check_u64(label, f_rsp, out1);
        }
        return;
    }
    if (!run_clean(what, do_iretq))
        return;
    char label[96];
    snprintf(label, sizeof label, "%s: rsp", what);
    check_u64(label, iret_out_rsp, iret_want_rsp);
    snprintf(label, sizeof label, "%s: rflags", what);
    check_u64(label, iret_out_flags, want_flags);
}

// ---- persistence: signal handler, frame layout, fork ----
static volatile uint64_t h_sel, h_csgsfs, h_flags;

static uint64_t read_ds_es_gs(void) {
    unsigned ds, es, gs;
    __asm__ volatile("mov %%ds, %0\n\tmov %%es, %1\n\tmov %%gs, %2"
                     : "=r"(ds), "=r"(es), "=r"(gs));
    return (uint64_t) ds << 32 | (uint64_t) es << 16 | gs;
}

static void usr1_handler(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    (void) sig;
    (void) si;
    h_sel = read_ds_es_gs();
    h_csgsfs = (uint64_t) uc->uc_mcontext.gregs[REG_CSGSFS];
    h_flags = uc->uc_flags;
}

static void load_ds_es_gs(unsigned ds, unsigned es, unsigned gs) {
    __asm__ volatile("mov %0, %%ds\n\tmov %1, %%es\n\tmov %2, %%gs"
                     :: "r"(ds), "r"(es), "r"(gs));
}

// The same two, trap-guarded: an engine without 8C/8E must fail these checks,
// not siglongjmp into a stale buffer and hang. ds:es:gs packed as above.
static uint64_t sel3;
static void do_read3(void) {
    sel3 = read_ds_es_gs();
}
static void do_load3(void) {
    load_ds_es_gs((unsigned) (sel3 >> 32), (unsigned) (sel3 >> 16) & 0xffff,
                  (unsigned) sel3 & 0xffff);
}
static int read3(uint64_t *out) {
    if (run(do_read3) != 0)
        return 0;
    *out = sel3;
    return 1;
}
static int load3(uint64_t v) {
    sel3 = v;
    return run(do_load3) == 0;
}

static void check_persistence(void) {
    const uint64_t want = (uint64_t) 0x2b << 32 | (uint64_t) 0x23 << 16 | 0x7b;
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = usr1_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, &old);

    // Only once both directions are known to work may the handler read them
    // unguarded.
    uint64_t after = 0;
    checks++;
    if (!load3(want) || !read3(&after) || after != want) {
        printf("FAIL persistence: could not load and read back ds, es, gs (%s)\n",
               signame(f_sig));
        failures_total++;
        load3(0);
        sigaction(SIGUSR1, &old, NULL);
        return;
    }
    h_sel = 0;
    raise(SIGUSR1);
    after = 0;
    read3(&after);
    check_u64("ds:es:gs inside a signal handler", h_sel, want);
    check_u64("ds:es:gs after sigreturn", after, want);
    check_u64("REG_CSGSFS", h_csgsfs, 0x002b000000000033ULL);
    check_u64("uc_flags & (FP_XSTATE|SIGCONTEXT_SS|STRICT_RESTORE_SS)", h_flags & 7, 7);
    sigaction(SIGUSR1, &old, NULL);

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        uint64_t v;
        _exit(read3(&v) && v == want ? 0 : 1);
    }
    int status = 0;
    checks++;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
        printf("FAIL fork: the child did not inherit ds 0x2b es 0x23 gs 0x7b "
               "(status %#x)\n", status);
        failures_total++;
    }
    load3(0);
}

// ---- ptrace ----
static void check_ptrace(void) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(3);
        if (!load3((uint64_t) 0x2b << 16 | 0x23))
            _exit(4);
        raise(SIGSTOP);
        // The tracer set ds to 0x2b and gs to 0.
        uint64_t v;
        _exit(read3(&v) && v == ((uint64_t) 0x2b << 32 | (uint64_t) 0x2b << 16) ? 0 : 1);
    }
    int status = 0;
    checks++;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status)) {
        printf("FAIL ptrace: the child did not stop (status %#x)\n", status);
        failures_total++;
        if (pid > 0) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
        return;
    }
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pid, 0, &regs) != 0) {
        printf("FAIL ptrace: GETREGS: %s\n", strerror(errno));
        failures_total++;
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return;
    }
    check_u64("ptrace cs", regs.cs, 0x33);
    check_u64("ptrace ss", regs.ss, 0x2b);
    check_u64("ptrace ds", regs.ds, 0);
    check_u64("ptrace es", regs.es, 0x2b);
    check_u64("ptrace fs", regs.fs, 0);
    check_u64("ptrace gs", regs.gs, 0x23);
    regs.ds = 0x2b;
    regs.gs = 0;
    checks++;
    if (ptrace(PTRACE_SETREGS, pid, 0, &regs) != 0) {
        printf("FAIL ptrace: SETREGS: %s\n", strerror(errno));
        failures_total++;
    }
    ptrace(PTRACE_CONT, pid, 0, 0);
    checks++;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL ptrace: after SETREGS the child read other selectors "
               "(status %#x)\n", status);
        failures_total++;
    }
}

// Whether a null selector loaded into FS clears its base: Intel does; AMD
// does only with CPUID 0x80000021:EAX[6] (NullSelectClearsBase).
static int null_sel_clears_base(void) {
    unsigned a, b, c, d;
    __cpuid(0, a, b, c, d);
    if (b != 0x68747541 && b != 0x6f677948)   // "Auth"enticAMD, "Hygo"nGenuine
        return 1;
    if (__get_cpuid_max(0x80000000, NULL) < 0x80000021)
        return 0;
    __cpuid(0x80000021, a, b, c, d);
    return (a >> 6) & 1;
}

static int loadable(int sreg_is_ss, unsigned sel) {
    if (sreg_is_ss)
        return sel == 0x2b;
    if (sel & 4)
        return 0;
    unsigned index = sel >> 3;
    return index == 0 || index == 4 || index == 5 || index == 6 || index == 15;
}

int main(int argc, char **argv) {
    char what[96];
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    setvbuf(stdout, NULL, _IONBF, 0);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    // Positive control: the harness must see a known #UD, at its address,
    // before any absence of a signal below means anything.
    if (run(ud_control) != SIGILL || f_rip != expect_ip) {
        printf("FAIL harness: ud2 gave %s at %#llx (want SIGILL at %#llx)\n",
               signame(f_sig), (unsigned long long) f_rip,
               (unsigned long long) expect_ip);
        failures_total++;
        return finish_suite(suite);
    }
    if (syscall(SYS_arch_prctl, ARCH_GET_FS_, &fs_base0) != 0 || fs_base0 == 0) {
        printf("FAIL harness: ARCH_GET_FS: %s\n", strerror(errno));
        failures_total++;
        return finish_suite(suite);
    }

    // 8C to a register.
    static const struct {
        const char *what;
        void (*fn)(void);
        uint64_t want;
    } reg_reads[] = {
        {"mov %cs,%eax", rd_cs_eax, 0x33},
        {"mov %ss,%eax", rd_ss_eax, 0x2b},
        {"mov %es,%eax", rd_es_eax, 0},
        {"mov %ds,%eax", rd_ds_eax, 0},
        {"mov %fs,%eax", rd_fs_eax, 0},
        {"mov %gs,%eax", rd_gs_eax, 0},
        {"mov %cs,%ax (66)", rd_cs_ax, (PATTERN & ~0xffffULL) | 0x33},
        {"mov %cs,%rax (REX.W)", rd_cs_rax, 0x33},
        {"mov %ss,%rax (66 REX.W)", rd_ss_rax_66w, 0x2b},
        {"mov %cs,%eax (REX.R ignored)", rd_cs_rexr, 0x33},
        {"mov %ss,%eax (67, interpreter)", rd_ss_a32, 0x2b},
        {"mov %cs,%eax; add; mov %ss,%ecx; add", rd_cs_then_add, 0x33 + 0x100 + 0x2b},
        {"mov %cs,%r8d", rd_cs_r8d, 0x33},
        {"mov %cs,%r8w", rd_cs_r8w, (PATTERN & ~0xffffULL) | 0x33},
    };
    for (unsigned i = 0; i < sizeof(reg_reads) / sizeof(reg_reads[0]); i++)
        if (run_clean(reg_reads[i].what, reg_reads[i].fn))
            check_u64(reg_reads[i].what, out0, reg_reads[i].want);

    // 8C to memory.
    if (run_clean("mov %cs,(%rdi)", mr_cs))
        check_membuf("mov %cs,(%rdi)", 0, 0x33);
    if (run_clean("mov %cs,(%rdi) REX.W", mr_cs_w))
        check_membuf("mov %cs,(%rdi) REX.W", 0, 0x33);
    if (run_clean("mov %ss,(%rdi) 66", mr_ss_66))
        check_membuf("mov %ss,(%rdi) 66", 0, 0x2b);
    if (run_clean("mov %cs,(%rdi) REX.WR", mr_cs_rexr))
        check_membuf("mov %cs,(%rdi) REX.WR", 0, 0x33);
    if (run_clean(".NET CONTEXT_CaptureContext pair", mr_net)) {
        checks++;
        if (membuf[0x38] != 0x33 || membuf[0x39] != 0 || membuf[0x42] != 0x2b ||
                membuf[0x43] != 0 || membuf[0x37] != 0xaa || membuf[0x3a] != 0xaa ||
                membuf[0x41] != 0xaa || membuf[0x44] != 0xaa) {
            printf("FAIL .NET pair: SegCs %02x%02x SegSs %02x%02x\n",
                   membuf[0x39], membuf[0x38], membuf[0x43], membuf[0x42]);
            failures_total++;
        }
    }

    // What must fault.
    run_fault("8c /6", ud_8c_reg6, SIGILL, ILL_ILLOPN);
    run_fault("8c /7", ud_8c_reg7, SIGILL, ILL_ILLOPN);
    run_fault("8c /6, memory operand", ud_8c_mem6, SIGILL, ILL_ILLOPN);
    run_fault("lock 8c", ud_8c_lock, SIGILL, ILL_ILLOPN);
    run_fault("mov %eax,%cs", ud_8e_cs, SIGILL, ILL_ILLOPN);
    run_fault("mov %eax,%cs (REX.R)", ud_8e_rexr_cs, SIGILL, ILL_ILLOPN);
    run_fault("8e /6", ud_8e_reg6, SIGILL, ILL_ILLOPN);
    run_fault("8e /7", ud_8e_reg7, SIGILL, ILL_ILLOPN);
    run_fault("push %cs (0e)", ud_push_cs, SIGILL, ILL_ILLOPN);
    run_fault("push %es (06)", ud_push_es, SIGILL, ILL_ILLOPN);
    run_fault("pop %ds (1f)", ud_pop_ds, SIGILL, ILL_ILLOPN);
    run_fault("pop %ss (17)", ud_pop_ss, SIGILL, ILL_ILLOPN);
    run_fault("mov %cs,(NULL)", pf_8c_null, SIGSEGV, SEGV_MAPERR);
    run_fault("mov (NULL),%es", pf_8e_null, SIGSEGV, SEGV_MAPERR);

    // 8E: every selector into every loadable register.
    static const uint16_t sels[] = {
        0, 1, 3, 0x08, 0x0b, 0x10, 0x13, 0x18, 0x1b, 0x20, 0x23, 0x28, 0x2b,
        0x30, 0x33, 0x38, 0x3b, 0x43, 0x4b, 0x53, 0x5b, 0x63, 0x6b, 0x73,
        0x78, 0x7b, 0x80, 0x83, 0x04, 0x07, 0x2f, 0x7f, 0xfffb,
    };
    static const struct {
        const char *name;
        void (*fn)(void);
        int is_ss;
    } regs[] = {
        {"es", ld_es, 0}, {"ds", ld_ds, 0}, {"gs", ld_gs, 0},
        {"ss", ld_ss, 1}, {"fs", ld_fs, 0},
    };
    int null_clears = null_sel_clears_base();
    for (unsigned r = 0; r < sizeof(regs) / sizeof(regs[0]); r++) {
        for (unsigned i = 0; i < sizeof(sels) / sizeof(sels[0]); i++) {
            load_sel = sels[i];
            snprintf(what, sizeof what, "mov $%#x,%%%s", sels[i], regs[r].name);
            if (!loadable(regs[r].is_ss, sels[i])) {
                run_fault(what, regs[r].fn, SIGSEGV, SI_KERNEL);
                continue;
            }
            if (!run_clean(what, regs[r].fn))
                continue;
            check_u64(what, out0, sels[i]);
            if (regs[r].fn == ld_fs) {
                snprintf(what, sizeof what, "fs base after mov $%#x,%%fs", sels[i]);
                check_u64(what, out1, (sels[i] & ~3) == 0 && !null_clears ? fs_base0 : 0);
                snprintf(what, sizeof what, "fs selector after ARCH_SET_FS (%#x)", sels[i]);
                check_u64(what, membuf[0], 0);
            }
        }
    }
    if (run_clean("mov (%rdi),%es REX.W, two bytes", ld_es_mem))
        check_u64("mov (%rdi),%es REX.W, two bytes", out0, 0x2b);
    if (run_clean("mov $0xffff0023,%es", ld_es_high))
        check_u64("mov $0xffff0023,%es", out0, 0x23);
    if (run_clean("mov %eax,%es (REX.R ignored)", ld_es_rexr))
        check_u64("mov %eax,%es (REX.R ignored)", out0, 0x2b);
    if (run_clean("mov %eax,%ds (67, interpreter)", ld_ds_a32))
        check_u64("mov %eax,%ds (67, interpreter)", out0, 0x7b);

    // PUSH / POP.
    if (run_clean("push %gs", push_gs64))
        check_u64("push %gs: 8 bytes, zero-extended", out0, 0x2b);
    if (run_clean("push %fs", push_fs64))
        check_u64("push %fs: 8 bytes, zero-extended", out0, 0);
    if (run_clean("pushw %gs", push_gs16)) {
        check_u64("pushw %gs: rsp moves 2", out0, 2);
        check_u64("pushw %gs: 2 bytes stored", out1, 0x0023ffffffffffffULL);
    }
    if (run_clean("pop %gs", pop_gs64)) {
        check_u64("pop %gs: low word of 8", out0, 0x2b);
        check_u64("pop %gs: rsp back where it was", out1, 0);
    }
    if (run_clean("popw %gs", pop_gs16)) {
        check_u64("popw %gs", out0, 0x23);
        check_u64("popw %gs: rsp back where it was", out1, 0);
    }
    if (run_clean("pop %fs", pop_fs64)) {
        check_u64("pop %fs", out0, 0x33);
        check_u64("fs base after pop %fs", out1, 0);
    }
    run_fault("pop %gs of 0x10", pop_gs_bad, SIGSEGV, SI_KERNEL);
    if (f_sig == SIGSEGV)
        check_u64("rsp at a faulting pop %gs", f_rsp, out1);

    // IRETQ.
    check_iretq("iretq", 0x33, 0x2b, 0x202, 0, 1, 0x202);
    check_iretq("iretq, arithmetic flags and DF", 0x33, 0x2b, 0xed7, 0, 1, 0xed7);
    check_iretq("iretq, AC and ID", 0x33, 0x2b, 0x240202, 0, 1, 0x240202);
    check_iretq("iretq, IF IOPL VM VIF VIP and bits 32-63 ignored", 0x33, 0x2b,
                0xffffffff001a3002ULL, 0, 1, 0x202);
    check_iretq("iretq, selectors' high bits ignored", 0xffffffffffff0033ULL,
                0xffffffff0000002bULL, 0x202, 0, 1, 0x202);
    check_iretq("iretq to cs 0x30 (RPL 0)", 0x30, 0x2b, 0x202, 0, 0, 0);
    check_iretq("iretq to cs 0", 0, 0x2b, 0x202, 0, 0, 0);
    check_iretq("iretq to cs 0x2b (data)", 0x2b, 0x2b, 0x202, 0, 0, 0);
    check_iretq("iretq to cs 0x10 (kernel)", 0x10, 0x2b, 0x202, 0, 0, 0);
    check_iretq("iretq to ss 0", 0x33, 0, 0x202, 0, 0, 0);
    check_iretq("iretq to ss 0x28 (RPL 0)", 0x33, 0x28, 0x202, 0, 0, 0);
    check_iretq("iretq to ss 0x33 (code)", 0x33, 0x33, 0x202, 0, 0, 0);
    check_iretq("iretq to a non-canonical rip", 0x33, 0x2b, 0x202,
                0x8000000000001000ULL, 0, 0);

    check_persistence();
    check_ptrace();

    // Every probe above counts itself; a siglongjmp that landed in the wrong
    // place would skip some silently.
    checks++;
    if (checks != 357) {
        printf("FAIL harness: ran %u checks, want 357\n", checks);
        failures_total++;
    }
    test_logf("%u checks\n", checks);
    return finish_suite(suite);
}

#else  /* !__x86_64__ */

int main(int argc, char **argv) {
    test_init(argc, argv);
    printf("%s: SKIP (x86_64 guest only)\n", suite);
    return 0;
}

#endif
