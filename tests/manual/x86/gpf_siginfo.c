// gpf_siginfo -- what an x86 task is told about a general protection fault
// (#GP), and about the traps beside it.
//
// Linux delivers a user-mode #GP with force_sig(SIGSEGV)
// (arch/x86/kernel/traps.c, gp_user_force_sig_segv). Measured on camd (Linux
// 6.12, Zen+) with gcc -m64 and -m32, every source agrees:
//   - SIGSEGV, si_code SI_KERNEL (128), si_addr NULL -- a bare signal carries
//     no address;
//   - REG_RIP / REG_EIP at the instruction, not after it;
//   - REG_TRAPNO 13;
//   - REG_ERR the #GP's error code: a bad selector's index and TI bit
//     (selector & 0xfffc: 0x13 gives 0x10, the LDT's 0x07 gives 0x04, a
//     null SS 0), vector * 8 + 2 for `int n` through a gate user mode may not
//     use, and 0 for everything else.
// AOK put the instruction's address in si_addr and 0 in REG_ERR, never
// decoded HLT, CLI or STI on i386 (they ran as no-ops), raised `int n` as the
// vector itself (int $0 was SIGFPE, int $0x81 exited the process with status
// 0x81), and turned an amd64 access to a non-canonical address into
// SEGV_MAPERR at that address.
//
// The traps next to #GP keep their own trap numbers: #PF 14 with CR2 the
// address, #UD 6, #DE 0, and `int $4` (#OF) 4 -- SIGSEGV SI_KERNEL reported
// after the instruction. On amd64 only the interpreter recorded the trap
// number, so a page fault or #UD the JIT raised showed whatever came before.
// Every probe therefore runs after a "primer" fault with a DIFFERENT trap
// number, in its own child process: a frame that kept the primer's number
// fails here instead of passing by coincidence.
//
// Not asserted, because AOK does not raise them yet (each is SIGILL, or no
// fault at all, where Linux raises #GP): privileged 0F opcodes such as RDMSR
// and MOV from CR0, misaligned MOVAPS/MOVDQA, `int n` on amd64, IRET on i386,
// and rt_sigreturn to a bad CS or SS on amd64. Nor are the page fault's
// present and instruction-fetch error-code bits, or CR2 surviving into a later
// #GP's frame (Linux keeps the last page fault's address there).
//
// Each faulting instruction carries a global label, so its address is a
// symbol rather than label arithmetic (which would need different spellings
// for i386 PIC and x86_64 RIP-relative).
//
// x86 only; builds and runs on both guests.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#include "../test_common.h"

#if !defined(__i386__) && !defined(__x86_64__)
int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    printf("gpf_siginfo: SKIP (x86 only)\n");
    return 0;
}
#else

#ifndef SI_KERNEL
#define SI_KERNEL 0x80
#endif
#ifndef SEGV_ACCERR
#define SEGV_ACCERR 2
#endif
#ifndef SEGV_MAPERR
#define SEGV_MAPERR 1
#endif

#define HIDDEN __attribute__((visibility("hidden")))
#define NI __attribute__((noinline, used))
#define AT(n) ".globl " #n "\n\t.hidden " #n "\n" #n ":\n\t"

// ---- the fault record ----

static sigjmp_buf env;
static volatile int f_sig, f_code;
static volatile uintptr_t f_addr, f_ip;
static volatile unsigned long f_trapno, f_err, f_cr2;

static void handler(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    f_sig = sig;
    f_code = si->si_code;
    f_addr = (uintptr_t) si->si_addr;
#ifdef __x86_64__
    f_ip = (uintptr_t) uc->uc_mcontext.gregs[REG_RIP];
    f_cr2 = (unsigned long) uc->uc_mcontext.gregs[REG_CR2];
#else
    f_ip = (uintptr_t) uc->uc_mcontext.gregs[REG_EIP];
    f_cr2 = (unsigned long) uc->uc_mcontext.cr2;
#endif
    f_trapno = (unsigned long) uc->uc_mcontext.gregs[REG_TRAPNO];
    f_err = (unsigned long) uc->uc_mcontext.gregs[REG_ERR];
    siglongjmp(env, 1);
}

static void catch_all(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
}

// ---- probes: each faults once, at the labelled instruction ----

static volatile uintptr_t arg;

extern const char at_hlt[] HIDDEN, at_cli[] HIDDEN, at_sti[] HIDDEN;
NI static void p_hlt(void) { __asm__ volatile(AT(at_hlt) "hlt"); }
NI static void p_cli(void) { __asm__ volatile(AT(at_cli) "cli"); }
NI static void p_sti(void) { __asm__ volatile(AT(at_sti) "sti"); }

extern const char at_in[] HIDDEN;
NI static void p_in(void) {
    __asm__ volatile("mov $0x5658, %%edx\n\t" AT(at_in) "inl %%dx, %%eax" ::: "eax", "edx");
}

extern const char at_mov_es[] HIDDEN, at_mov_ss[] HIDDEN, at_mov_fs[] HIDDEN,
    at_mov_gs[] HIDDEN, at_pop_fs[] HIDDEN;
NI static void p_mov_es(void) {
    __asm__ volatile("mov %0, %%eax\n\t" AT(at_mov_es) "mov %%eax, %%es" :: "m"(arg) : "eax");
}
NI static void p_mov_ss(void) {
    __asm__ volatile("mov %0, %%eax\n\t" AT(at_mov_ss) "mov %%eax, %%ss" :: "m"(arg) : "eax");
}
NI static void p_mov_fs(void) {
    __asm__ volatile("mov %0, %%eax\n\t" AT(at_mov_fs) "mov %%eax, %%fs" :: "m"(arg) : "eax");
}
NI static void p_mov_gs(void) {
    __asm__ volatile("mov %0, %%eax\n\t" AT(at_mov_gs) "mov %%eax, %%gs" :: "m"(arg) : "eax");
}
#ifdef __x86_64__
NI static void p_pop_fs(void) {
    __asm__ volatile("sub $128, %%rsp\n\t"
                     "pushq %0\n\t" AT(at_pop_fs) "pop %%fs\n\t"
                     "add $128, %%rsp" :: "r"((uint64_t) arg) : "memory");
}
#else
NI static void p_pop_fs(void) {
    __asm__ volatile("pushl %0\n\t" AT(at_pop_fs) "pop %%fs" :: "m"(arg) : "memory");
}
#endif

extern const char at_ud2[] HIDDEN, at_div0[] HIDDEN;
NI static void p_ud2(void) { __asm__ volatile(AT(at_ud2) "ud2"); }
NI static void p_div0(void) {
    __asm__ volatile("xor %%ecx, %%ecx\n\txor %%edx, %%edx\n\tmov $1, %%eax\n\t"
                     AT(at_div0) "divl %%ecx" ::: "eax", "ecx", "edx");
}

static volatile char *none_page;
extern const char at_pf[] HIDDEN;
NI static void p_pf(void) {
    __asm__ volatile(AT(at_pf) "movb (%0), %%al" :: "r"(none_page + 0x123) : "eax");
}

#ifdef __i386__
// int n. The gates user mode may use -- 3, 4 and 0x80 -- trap after the
// instruction; the rest are #GP at it.
extern const char at_int81[] HIDDEN, at_int0[] HIDDEN, at_int1[] HIDDEN,
    at_int6[] HIDDEN, at_int0e[] HIDDEN, at_int20[] HIDDEN, at_intff[] HIDDEN,
    at_int4[] HIDDEN, at_int3[] HIDDEN;
NI static void p_int81(void) { __asm__ volatile(AT(at_int81) "int $0x81"); }
NI static void p_int0(void) { __asm__ volatile(AT(at_int0) "int $0"); }
NI static void p_int1(void) { __asm__ volatile(AT(at_int1) "int $1"); }
NI static void p_int6(void) { __asm__ volatile(AT(at_int6) "int $6"); }
NI static void p_int0e(void) { __asm__ volatile(AT(at_int0e) "int $0x0e"); }
NI static void p_int20(void) { __asm__ volatile(AT(at_int20) "int $0x20"); }
NI static void p_intff(void) { __asm__ volatile(AT(at_intff) "int $0xff"); }
NI static void p_int4(void) { __asm__ volatile(AT(at_int4) "int $4"); }
NI static void p_int3(void) { __asm__ volatile(AT(at_int3) ".byte 0xcd, 0x03"); }

extern const char at_pop_es[] HIDDEN, at_pop_ss[] HIDDEN, at_pop_ds[] HIDDEN;
NI static void p_pop_es(void) {
    __asm__ volatile("pushl %0\n\t" AT(at_pop_es) "pop %%es" :: "m"(arg) : "memory");
}
NI static void p_pop_ss(void) {
    __asm__ volatile("pushl %0\n\t" AT(at_pop_ss) "pop %%ss" :: "m"(arg) : "memory");
}
NI static void p_pop_ds(void) {
    __asm__ volatile("pushl %0\n\t" AT(at_pop_ds) "pop %%ds" :: "m"(arg) : "memory");
}

// sigreturn to a frame whose CS or SS the handler broke: the #GP is where
// the task would have resumed.
static volatile unsigned sr_cs, sr_ss;
static volatile uintptr_t sr_resume;
static void usr1_break_frame(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    (void) sig;
    (void) si;
    if (sr_cs != 0)
        uc->uc_mcontext.gregs[REG_CS] = sr_cs;
    if (sr_ss != 0xffff)
        uc->uc_mcontext.gregs[REG_SS] = sr_ss;
    sr_resume = (uintptr_t) uc->uc_mcontext.gregs[REG_EIP];
}
NI static void p_sigreturn(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = usr1_break_frame;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, NULL);
    raise(SIGUSR1);
}
#endif

#ifdef __x86_64__
extern const char at_nc_load[] HIDDEN, at_nc_store[] HIDDEN, at_kaddr[] HIDDEN,
    at_iretq[] HIDDEN;
NI static void p_nc_load(void) {
    __asm__ volatile("movabs $0x8000000000001000, %%rax\n\t"
                     AT(at_nc_load) "mov (%%rax), %%rax" ::: "rax");
}
NI static void p_nc_store(void) {
    __asm__ volatile("movabs $0x0000900000001000, %%rax\n\t"
                     AT(at_nc_store) "movq $0, (%%rax)" ::: "rax", "memory");
}
// Canonical, but the kernel's half: a page fault, not a #GP.
NI static void p_kaddr(void) {
    __asm__ volatile("movabs $0xffff888000001000, %%rax\n\t"
                     AT(at_kaddr) "mov (%%rax), %%rax" ::: "rax");
}

static uint64_t iret_cs, iret_ss, iret_rip;
NI static void p_iretq(void) {
    __asm__ volatile("mov %%rsp, %%r12\n\t"
                     "sub $256, %%rsp\n\tand $-16, %%rsp\n\tmov %%rsp, %%r13\n\t"
                     "mov %[rip], %%rax\n\ttest %%rax, %%rax\n\tjnz 3f\n\t"
                     "lea 2f(%%rip), %%rax\n3:\n\t"
                     "push %[ss]\n\tpush %%r13\n\tpushfq\n\tpush %[cs]\n\tpush %%rax\n\t"
                     AT(at_iretq) "iretq\n\tud2\n2:\n\tmov %%r12, %%rsp"
                     :: [cs] "r"(iret_cs), [ss] "r"(iret_ss), [rip] "r"(iret_rip)
                     : "rax", "r12", "r13", "memory", "cc");
}
#endif

// ---- running one ----

enum { IP_RESUME = -1 };

struct expect {
    const char *what;
    void (*fn)(void);
    const char *at;       // the faulting instruction
    int ip_off;           // the saved IP is at + ip_off, or IP_RESUME
    int sig, code;
    unsigned long trapno;
    unsigned long err;
    unsigned long err_mask; // bits of REG_ERR compared; 0 means all
    int addr;             // what si_addr is: ADDR_NULL, ADDR_PAGE or ADDR_INSN
    uintptr_t arg;
};

// si_addr: NULL for a bare signal; none_page + 0x123 (and CR2 with it) for
// the page fault; the instruction for #UD and #DE.
enum { ADDR_NULL, ADDR_PAGE, ADDR_INSN };

static int check(const struct expect *e) {
    int bad = 0;
    uintptr_t want_ip = e->ip_off == IP_RESUME ? 0 : (uintptr_t) e->at + e->ip_off;
#ifdef __i386__
    if (e->ip_off == IP_RESUME)
        want_ip = sr_resume;
#endif
    uintptr_t want_addr = e->addr == ADDR_PAGE ? (uintptr_t) none_page + 0x123 :
                          e->addr == ADDR_INSN ? (uintptr_t) e->at : 0;
    unsigned long mask = e->err_mask != 0 ? e->err_mask : ~0ul;

    if (f_sig != e->sig) {
        printf("FAIL %s: signal %d, want %d\n", e->what, f_sig, e->sig);
        return 1;
    }
    if (f_code != e->code) {
        printf("FAIL %s: si_code %d, want %d\n", e->what, f_code, e->code);
        bad++;
    }
    if (f_addr != want_addr) {
        printf("FAIL %s: si_addr %#lx, want %#lx\n", e->what,
               (unsigned long) f_addr, (unsigned long) want_addr);
        bad++;
    }
    if (f_ip != want_ip) {
        printf("FAIL %s: saved IP %#lx, want %#lx (the instruction%s)\n", e->what,
               (unsigned long) f_ip, (unsigned long) want_ip,
               e->ip_off > 0 ? ", plus its length: a trap" : "");
        bad++;
    }
    if (f_trapno != e->trapno) {
        printf("FAIL %s: REG_TRAPNO %lu, want %lu\n", e->what, f_trapno, e->trapno);
        bad++;
    }
    if ((f_err & mask) != (e->err & mask)) {
        printf("FAIL %s: REG_ERR %#lx, want %#lx (mask %#lx)\n", e->what, f_err,
               e->err, mask);
        bad++;
    }
    if (e->addr == ADDR_PAGE && f_cr2 != want_addr) {
        printf("FAIL %s: CR2 %#lx, want %#lx\n", e->what, f_cr2,
               (unsigned long) want_addr);
        bad++;
    }
    if (bad == 0)
        test_logf("  ok   %s: sig %d code %d trapno %lu err %#lx\n", e->what,
                  f_sig, f_code, f_trapno, f_err);
    return bad;
}

// In a child: prime the trap state with a fault of a different kind, run the
// probe, check what arrived. The child's exit status is its failure count, so
// a probe that kills its process is a failure the parent can name.
static void run(const struct expect *e) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL %s: fork\n", e->what);
        failures_total++;
        return;
    }
    if (pid == 0) {
        catch_all();
        // The primer: ud2 (trap 6) before a #GP, a #GP (13) before anything
        // else. HLT is a #GP on both ABIs.
        f_sig = 0;
        if (sigsetjmp(env, 1) == 0)
            (e->trapno == 13 ? p_ud2 : p_hlt)();
        if (f_sig == 0) {
            printf("FAIL %s: the primer did not fault\n", e->what);
            _exit(1);
        }
        arg = e->arg;
        f_sig = 0;
        if (sigsetjmp(env, 1) == 0) {
            e->fn();
            printf("FAIL %s: no signal\n", e->what);
            fflush(stdout);
            _exit(1);
        }
        int bad = check(e);
        fflush(stdout);
        _exit(bad > 100 ? 100 : bad);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        printf("FAIL %s: waitpid\n", e->what);
        failures_total++;
    } else if (WIFSIGNALED(status)) {
        printf("FAIL %s: killed by signal %d\n", e->what, WTERMSIG(status));
        failures_total++;
    } else {
        failures_total += WEXITSTATUS(status);
    }
}

#define GP(name, fn, at, code, a) \
    {name, fn, at, 0, SIGSEGV, SI_KERNEL, 13, code, 0, ADDR_NULL, a}

int main(int argc, char **argv) {
    test_init(argc, argv);
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(test_watchdog_secs(120));

    none_page = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (none_page == MAP_FAILED) {
        printf("FAIL mmap PROT_NONE\n");
        return finish_suite("gpf_siginfo");
    }

    static const struct expect common[] = {
        // Positive control first: a page fault, whose si_addr and CR2 are the
        // address, so a harness that never reads them cannot pass the NULLs
        // below. REG_ERR: user mode (4), a read (not 2).
        {"page fault (control)", p_pf, at_pf, 0, SIGSEGV, SEGV_ACCERR, 14, 4, 6,
         ADDR_PAGE, 0},
        {"ud2", p_ud2, at_ud2, 0, SIGILL, 2 /* ILL_ILLOPN */, 6, 0, 0, ADDR_INSN, 0},
        {"div by 0", p_div0, at_div0, 0, SIGFPE, 1 /* FPE_INTDIV */, 0, 0, 0,
         ADDR_INSN, 0},

        GP("in eax, dx", p_in, at_in, 0, 0),
        GP("hlt", p_hlt, at_hlt, 0, 0),
        GP("cli", p_cli, at_cli, 0, 0),
        GP("sti", p_sti, at_sti, 0, 0),

        GP("mov es, 0x13", p_mov_es, at_mov_es, 0x10, 0x13),
        GP("mov es, 0x07 (LDT)", p_mov_es, at_mov_es, 0x04, 0x07),
        GP("mov es, 0x43", p_mov_es, at_mov_es, 0x40, 0x43),
        GP("mov es, 0xfffb", p_mov_es, at_mov_es, 0xfff8, 0xfffb),
        GP("mov es, 0x4003", p_mov_es, at_mov_es, 0x4000, 0x4003),
        GP("mov fs, 0x13", p_mov_fs, at_mov_fs, 0x10, 0x13),
        GP("mov gs, 0x0f (LDT)", p_mov_gs, at_mov_gs, 0x0c, 0x0f),
        GP("mov ss, 0", p_mov_ss, at_mov_ss, 0, 0),
        GP("mov ss, 0x33", p_mov_ss, at_mov_ss, 0x30, 0x33),
        GP("mov ss, 0x13", p_mov_ss, at_mov_ss, 0x10, 0x13),
        GP("pop fs, 0x13", p_pop_fs, at_pop_fs, 0x10, 0x13),
    };
    for (unsigned i = 0; i < sizeof common / sizeof common[0]; i++)
        run(&common[i]);

#ifdef __i386__
    static const struct expect i386_only[] = {
        GP("int $0x81", p_int81, at_int81, 0x81 * 8 + 2, 0),
        GP("int $0", p_int0, at_int0, 2, 0),
        GP("int $1", p_int1, at_int1, 1 * 8 + 2, 0),
        GP("int $6", p_int6, at_int6, 6 * 8 + 2, 0),
        GP("int $0x0e", p_int0e, at_int0e, 0x0e * 8 + 2, 0),
        GP("int $0x20", p_int20, at_int20, 0x20 * 8 + 2, 0),
        GP("int $0xff", p_intff, at_intff, 0xff * 8 + 2, 0),
        // The gates user mode may use: traps, reported after the two bytes.
        {"int $4", p_int4, at_int4, 2, SIGSEGV, SI_KERNEL, 4, 0, 0, ADDR_NULL, 0},
        {"int $3 (cd 03)", p_int3, at_int3, 2, SIGTRAP, SI_KERNEL, 3, 0, 0, ADDR_NULL, 0},

        GP("pop es, 0x13", p_pop_es, at_pop_es, 0x10, 0x13),
        GP("pop ds, 0x43", p_pop_ds, at_pop_ds, 0x40, 0x43),
        GP("pop ss, 0x07 (LDT)", p_pop_ss, at_pop_ss, 0x04, 0x07),
    };
    for (unsigned i = 0; i < sizeof i386_only / sizeof i386_only[0]; i++)
        run(&i386_only[i]);

    // sigreturn: CS's selector if CS is bad, else SS's -- but 0 whenever SS
    // names the LDT (Linux returns there through its espfix stack).
    static const struct { const char *what; unsigned cs, ss, err; } srs[] = {
        {"sigreturn to cs 0x2b", 0x2b, 0xffff, 0x28},
        {"sigreturn to cs 0x10", 0x10, 0xffff, 0x10},
        {"sigreturn to cs 0x07 (LDT)", 0x07, 0xffff, 0x04},
        {"sigreturn to ss 0x13", 0, 0x13, 0x10},
        {"sigreturn to ss 0x23", 0, 0x23, 0x20},
        {"sigreturn to ss 0x07 (LDT)", 0, 0x07, 0},
        {"sigreturn to cs 0x2b, ss 0x0f (LDT)", 0x2b, 0x0f, 0},
    };
    for (unsigned i = 0; i < sizeof srs / sizeof srs[0]; i++) {
        struct expect e = {srs[i].what, p_sigreturn, NULL, IP_RESUME, SIGSEGV,
                           SI_KERNEL, 13, srs[i].err, 0, ADDR_NULL, 0};
        sr_cs = srs[i].cs;
        sr_ss = srs[i].ss;
        run(&e);
    }
#endif

#ifdef __x86_64__
    static const struct expect amd64_only[] = {
        GP("load from 0x8000000000001000", p_nc_load, at_nc_load, 0, 0),
        GP("store to 0x0000900000001000", p_nc_store, at_nc_store, 0, 0),
    };
    for (unsigned i = 0; i < sizeof amd64_only / sizeof amd64_only[0]; i++)
        run(&amd64_only[i]);

    // A canonical kernel address is a page fault at that address. si_addr is
    // checked by hand: it is not none_page.
    {
        struct expect e = {"load from 0xffff888000001000", p_kaddr, at_kaddr, 0,
                           SIGSEGV, SEGV_MAPERR, 14, 4, 6, ADDR_NULL, 0};
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            catch_all();
            f_sig = 0;
            if (sigsetjmp(env, 1) == 0)
                p_hlt();
            f_sig = 0;
            if (sigsetjmp(env, 1) == 0) {
                p_kaddr();
                printf("FAIL %s: no signal\n", e.what);
                _exit(1);
            }
            int bad = 0;
            if (f_addr != 0xffff888000001000ull || f_cr2 != 0xffff888000001000ull) {
                printf("FAIL %s: si_addr %#lx CR2 %#lx, want the address\n", e.what,
                       (unsigned long) f_addr, f_cr2);
                bad++;
            }
            f_addr = 0; // check() wants NULL for a non-page probe
            bad += check(&e);
            _exit(bad);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status)) {
            printf("FAIL %s: killed by signal %d\n", e.what, WTERMSIG(status));
            failures_total++;
        } else {
            failures_total += WEXITSTATUS(status);
        }
    }

    // IRETQ: CS's selector if CS is bad, else SS's; 0 for a non-canonical RIP,
    // and for CS 0x23, where Linux takes the IRET and faults on a RIP past the
    // 32-bit code segment's 4 GiB limit.
    static const struct { const char *what; uint64_t cs, ss, rip, err; } irets[] = {
        {"iretq to cs 0", 0, 0x2b, 0, 0},
        {"iretq to cs 0x2b", 0x2b, 0x2b, 0, 0x28},
        {"iretq to cs 0x10", 0x10, 0x2b, 0, 0x10},
        {"iretq to cs 0x07 (LDT)", 0x07, 0x2b, 0, 0x04},
        {"iretq to cs 0x23", 0x23, 0x2b, 0, 0},
        {"iretq to ss 0x13", 0x33, 0x13, 0, 0x10},
        {"iretq to ss 0", 0x33, 0, 0, 0},
        {"iretq to ss 0x33", 0x33, 0x33, 0, 0x30},
        {"iretq to cs 0x2b, ss 0x13", 0x2b, 0x13, 0, 0x28},
        {"iretq to rip 0x8000000000001000", 0x33, 0x2b, 0x8000000000001000ull, 0},
    };
    for (unsigned i = 0; i < sizeof irets / sizeof irets[0]; i++) {
        struct expect e = GP(irets[i].what, p_iretq, at_iretq, irets[i].err, 0);
        iret_cs = irets[i].cs;
        iret_ss = irets[i].ss;
        iret_rip = irets[i].rip;
        run(&e);
    }
#endif

    return finish_suite("gpf_siginfo");
}
#endif
