// amd64 GS segment base: the 65 prefix, ARCH_SET_GS and ARCH_GET_GS.
//
// Neither amd64 engine decoded 65 -- `mov %gs:0x10,%rax` was SIGILL, si_code
// 2 -- and ARCH_SET_GS was EINVAL, on the grounds that there was only one TLS
// base. Programs that keep their own per-thread data in GS (Wine-style
// runtimes, some JITs and sanitizers) need both.
//
// Everything below was measured on x86_64 Linux 6.12 (camd, AMD Zen+) and
// passes there unchanged:
//
//  - The GS base is 0 from exec on, so a GS-relative access is an ordinary
//    one at its offset: %gs:0x10 is SIGSEGV SEGV_MAPERR at 0x10, reported at
//    the instruction. ARCH_GET_GS reads 0.
//  - ARCH_SET_GS sets the base and ARCH_GET_GS reads it back; the GS selector
//    is left 0 whatever was loaded. Every memory operand under 65 is then at
//    base + effective address -- loads, stores, read-modify-write, LOCK,
//    moffs, SSE, AVX, x87, PUSH/POP and indirect CALL -- except LEA, which
//    computes the effective address alone. Under 67 the effective address is
//    truncated to 32 bits first and the base added after. On a register
//    operand 65 does nothing. The FS base is a separate register throughout.
//  - For the string instructions and XLAT the override moves the implicit
//    DS operand -- the MOVS and LODS source at RSI, the XLAT table at RBX --
//    and not the ES:RDI destination of MOVS or STOS.
//  - ARCH_SET_GS (and ARCH_SET_FS) of an address at or above TASK_SIZE_MAX,
//    0x7ffffffff000, is EPERM and changes nothing; ARCH_GET_GS to an
//    unmapped pointer is EFAULT.
//  - Loading a non-null selector into GS (8E, POP GS) sets its base to 0. A
//    null one clears it on Intel and keeps it on AMD parts without CPUID
//    0x80000021:EAX[6] (camd keeps it; iSH-AOK reports GenuineIntel and
//    clears it), so that expectation follows the CPUID vendor, as in
//    amd64_segment_regs.c.
//  - A signal handler runs with the base, and one the handler sets survives
//    sigreturn: the frame does not carry it. fork and a new thread inherit
//    it, and a thread's own ARCH_SET_GS is its alone.
//  - execve clears both bases: at the exec stop PTRACE_GETREGS reports
//    fs_base and gs_base 0, and the new image's first GS access faults.
//  - PTRACE_GETREGS and PEEKUSER report gs_base, and SETREGS sets it.
//
// The 67-prefixed forms are here because the JIT leaves an address-size
// prefix to the interpreter, so they are what exercises the interpreter's
// decoder; the others run as the JIT compiles them.
//
// x86_64 only; the i386 frontend is a different decoder.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../test_common.h"

static const char *suite = "amd64_gs_base";

#if defined(__x86_64__)

#include <cpuid.h>
#include <ucontext.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>

#ifndef SEGV_MAPERR
#define SEGV_MAPERR 1
#endif
#define ARCH_SET_GS_ 0x1001
#define ARCH_SET_FS_ 0x1002
#define ARCH_GET_FS_ 0x1003
#define ARCH_GET_GS_ 0x1004
#define TASK_SIZE_MAX_ 0x7ffffffff000ULL
#define PATTERN 0xdeadbeefcafef00dULL

static sigjmp_buf env;
static volatile int f_sig, f_code;
static volatile uint64_t f_addr, f_rip;
// Each probe stores the address of the instruction that may fault here just
// before running it.
static uint64_t expect_ip;
static unsigned checks;
static uint64_t fs_base0;

// What GS points at. gq(i) is qword i as it was filled, read without GS.
static uint64_t gsbuf[32] __attribute__((aligned(64)));
static uint64_t flatbuf[4];
static uint64_t gq(unsigned i) {
    return 0x8070605040302010ULL + i * 0x0101010101010101ULL;
}
static uint8_t gb(unsigned off) {
    return (uint8_t) (gq(off / 8) >> (8 * (off % 8)));
}
static void fill(void) {
    for (unsigned i = 0; i < 32; i++)
        gsbuf[i] = gq(i);
    memset(flatbuf, 0, sizeof flatbuf);
}

// The registers a probe starts with and ends with.
static uint64_t in_rax, in_rdi, in_rsi, in_rcx, in_rbx;
static uint64_t out_rax, out_rdi, out_rsi;

static void fault_handler(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    f_sig = sig;
    f_code = si->si_code;
    f_addr = (uintptr_t) si->si_addr;
    f_rip = (uint64_t) uc->uc_mcontext.gregs[REG_RIP];
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

static int run_clean(const char *what, void (*fn)(void)) {
    int sig = run(fn);
    checks++;
    if (sig != 0) {
        printf("FAIL %s: %s (si_code %d) at %#llx, addr %#llx\n", what,
               signame(sig), f_code, (unsigned long long) f_rip,
               (unsigned long long) f_addr);
        failures_total++;
        return 0;
    }
    return 1;
}

// A probe that must fault with want_sig/want_code at want_addr, reported at
// expect_ip.
static void run_fault(const char *what, void (*fn)(void), int want_sig,
                      int want_code, uint64_t want_addr) {
    int sig = run(fn);
    checks++;
    if (sig != want_sig) {
        printf("FAIL %s: %s (si_code %d), want %s\n", what, signame(sig), f_code,
               signame(want_sig));
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
    if (want_sig == SIGSEGV && f_addr != want_addr) {
        printf("FAIL %s: si_addr %#llx, want %#llx\n", what,
               (unsigned long long) f_addr, (unsigned long long) want_addr);
        failures_total++;
    }
    test_logf("  ok   %s -> %s code %d addr %#llx\n", what, signame(sig), f_code,
              (unsigned long long) f_addr);
}

static long arch_prctl_(int code, uint64_t arg) {
    return syscall(SYS_arch_prctl, code, arg);
}
static uint64_t get_gs(void) {
    uint64_t v = PATTERN;
    if (arch_prctl_(ARCH_GET_GS_, (uint64_t) (uintptr_t) &v) != 0)
        return PATTERN;
    return v;
}
static int set_gs(uint64_t base) {
    return arch_prctl_(ARCH_SET_GS_, base) == 0;
}

// The address of the next instruction, stored before it runs.
#define MARK "lea 1f(%%rip), %%r11\n\tmov %%r11, %[ip]\n1:\t"

// One instruction (or a short run) with rax, rdi, rsi, rcx and rbx loaded from
// in_* and read back into out_*. xmm0 and the x87 stack are the probe's own.
#define PROBE(name, insn)                                                      \
    static void name(void) {                                                   \
        register uint64_t rax __asm__("rax") = in_rax;                         \
        register uint64_t rdi __asm__("rdi") = in_rdi;                         \
        register uint64_t rsi __asm__("rsi") = in_rsi;                         \
        register uint64_t rcx __asm__("rcx") = in_rcx;                         \
        register uint64_t rbx __asm__("rbx") = in_rbx;                         \
        __asm__ volatile(MARK insn                                             \
                         : "+r"(rax), "+r"(rdi), "+r"(rsi), "+r"(rcx),         \
                           "+r"(rbx), [ip] "=m"(expect_ip)                     \
                         :: "r11", "rdx", "xmm0", "memory", "cc");             \
        out_rax = rax;                                                         \
        out_rdi = rdi;                                                         \
        out_rsi = rsi;                                                         \
    }

// ---- Loads ----
PROBE(ld_abs, "mov %%gs:8, %%rax")                            // 65 48 8b 04 25
PROBE(ld_base, "mov %%gs:(%%rdi), %%rax")                     // base register
PROBE(ld_sib, "mov %%gs:8(%%rdi,%%rcx,8), %%rax")             // base+index*8+disp
PROBE(ld_byte, "movb %%gs:10, %%al")                          // 8a
PROBE(ld_word, "movw %%gs:12, %%ax")                          // 66 8b
PROBE(ld_zx, "movzbl %%gs:9, %%eax")                          // 0f b6
PROBE(ld_sx, "movslq %%gs:8, %%rax")                          // 63
PROBE(ld_add, "add %%gs:8, %%rax")                            // 03
PROBE(ld_imul, "imul %%gs:16, %%rax")                         // 0f af
PROBE(ld_cmp, "cmp %%gs:8, %%rax\n\tsete %%al\n\tmovzbl %%al, %%eax") // 3b
PROBE(ld_test, "test %%rax, %%gs:24\n\tsetne %%al\n\tmovzbl %%al, %%eax") // 85
PROBE(ld_cmov, "cmp %%rax, %%rax\n\tcmove %%gs:32, %%rax")    // 0f 44
PROBE(ld_moffs, ".byte 0x65, 0x48, 0xa1\n\t.quad 0x10")       // movabs %gs:0x10,%rax
PROBE(ld_movq_xmm, "movq %%gs:8, %%xmm0\n\tmovq %%xmm0, %%rax")          // f3 0f 7e
PROBE(ld_movdqu, "movdqu %%gs:16, %%xmm0\n\tpsrldq $8, %%xmm0\n\tmovq %%xmm0, %%rax") // f3 0f 6f
PROBE(ld_x87, "fildll %%gs:32\n\tfistpll (%%rsi)")            // df /5, then flat
// The stack-using probes step over the red zone first.
#define RZ(insn) "lea -128(%%rsp), %%rsp\n\t" insn "\n\tlea 128(%%rsp), %%rsp"
PROBE(ld_push, RZ("pushq %%gs:8\n\tpopq %%rax"))            // ff /6
PROBE(ld_bt, "btq $4, %%gs:8\n\tsetc %%al\n\tmovzbl %%al, %%eax")        // 0f ba /4
PROBE(ld_lea, ".byte 0x65\n\tlea 8(%%rdi), %%rax")              // no base: 8 + rdi
PROBE(ld_a32, "mov %%gs:(%%edi), %%rax")                      // 67: interpreter
PROBE(ld_a32_add, "add %%gs:8(%%edi), %%rax")                 // 67 03
PROBE(ld_lods, ".byte 0x65\n\tlodsq")                        // source is gs:rsi
PROBE(ld_xlat, ".byte 0x65\n\txlatb")                        // al = gs:[rbx+al]
PROBE(ld_reg, ".byte 0x65\n\tmov %%rdi, %%rax")               // 65 on a register
PROBE(ld_adc_acc, "stc\n\t.byte 0x65, 0x15, 1, 0, 0, 0")      // adc $1,%eax, 15 id
PROBE(ld_after_jmp, "jmp 2f\n2:\tmov %%gs:8, %%rax")          // a block of its own

// ---- Stores and read-modify-write ----
PROBE(st_mov, "mov %%rax, %%gs:48")                           // 89
PROBE(st_byte, "movb %%al, %%gs:56")                          // 88
PROBE(st_imm, "movq $-2, %%gs:64")                            // c7
PROBE(st_imm8, "movb $0x5a, %%gs:193")                        // c6
PROBE(st_add83, "addq $5, %%gs:72")                           // 83 /0
PROBE(st_sub81, "subl $0x10000, %%gs:80")                     // 81 /5
PROBE(st_or80, "orb $0x80, %%gs:88")                          // 80 /1
PROBE(st_add01, "add %%rax, %%gs:96")                         // 01
PROBE(st_inc, "incq %%gs:104")                                // ff /0
PROBE(st_neg, "negq %%gs:112")                                // f7 /3
PROBE(st_xchg, "xchg %%rax, %%gs:120")                        // 87
PROBE(st_lock_xadd, "lock xadd %%rax, %%gs:128")              // f0 0f c1
PROBE(st_lock_cmpxchg, "lock cmpxchg %%rdi, %%gs:136")        // f0 0f b1
PROBE(st_lock_inc, "lock incq %%gs:144")                      // f0 ff /0
PROBE(st_pop, RZ("pushq %%rax\n\tpopq %%gs:152"))             // 8f /0
PROBE(st_movdqu, "movq %%rax, %%xmm0\n\tmovdqu %%xmm0, %%gs:160")        // f3 0f 7f
PROBE(st_bts, "btsq $1, %%gs:176")                            // 0f ba /5
PROBE(st_setcc, "cmp %%rax, %%rax\n\tsete %%gs:184")          // 0f 94
PROBE(st_a32, "mov %%rax, %%gs:(%%edi)")                      // 67 89
PROBE(st_movs, ".byte 0x65\n\tmovsq")                        // gs:rsi -> es:rdi
PROBE(st_stos, ".byte 0x65\n\tstosq")                        // es:rdi, 65 ignored

// ---- The block with both prefixes, and an indirect call through GS ----
static uint64_t fs0_seen;
static void fs_and_gs(void) {
    uint64_t a, b;
    __asm__ volatile("mov %%fs:0, %0\n\tmov %%gs:8, %1" : "=&r"(a), "=&r"(b));
    fs0_seen = a;
    out_rax = b;
}
__asm__(".text\n"
        ".globl gs_call_target\n"
        "gs_call_target:\n\t"
        "mov $0x5a5a5a5a, %eax\n\t"
        "ret\n");
void gs_call_target(void);
PROBE(ld_call, RZ("call *%%gs:200"))                          // ff /2

// ---- Faults at the default base ----
static void pf_ld(void) {
    __asm__ volatile(MARK "mov %%gs:0x10, %%rax" : [ip] "=m"(expect_ip) :: "r11", "rax");
}
static void pf_ld_a32(void) {
    __asm__ volatile(MARK "mov %%gs:(%%eax), %%rax"
                     : [ip] "=m"(expect_ip) : "a"(0x10) : "r11");
}
static void pf_st(void) {
    __asm__ volatile(MARK "movb $1, %%gs:0x18" : [ip] "=m"(expect_ip) :: "r11", "memory");
}
static void pf_lods(void) {
    __asm__ volatile(MARK ".byte 0x65\n\tlodsq" : [ip] "=m"(expect_ip) : "S"(0x20) : "r11", "rax");
}
static void ud_control(void) {
    __asm__ volatile(MARK "ud2" : [ip] "=m"(expect_ip) :: "r11");
}
// The same load with no prefix, which must fault the same way: the
// comparison that says the harness can see a MAPERR at 0x10 at all.
static void pf_flat(void) {
    __asm__ volatile(MARK "mov 0x10, %%rax" : [ip] "=m"(expect_ip) :: "r11", "rax");
}

// ---- Selector loads ----
static uint64_t sel_out;
static uint16_t load_sel;
static void load_gs_sel(void) {
    unsigned s = load_sel, got;
    __asm__ volatile("mov %1, %%gs\n\tmov %%gs, %0" : "=&r"(got) : "r"(s));
    sel_out = got;
}
static void pop_gs_sel(void) {
    uint64_t s = load_sel;
    unsigned got;
    __asm__ volatile("push %1\n\tpop %%gs\n\tmov %%gs, %0" : "=&r"(got) : "r"(s) : "memory");
    sel_out = got;
}
static void read_gs_sel(void) {
    unsigned got;
    __asm__ volatile("mov %%gs, %0" : "=r"(got));
    sel_out = got;
}

// Whether a null selector loaded into GS clears its base: Intel does; AMD
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

static void check_selectors(void) {
    int null_clears = null_sel_clears_base();
    static const struct {
        const char *what;
        void (*fn)(void);
        uint16_t sel;
    } loads[] = {
        {"mov $0x2b,%gs", load_gs_sel, 0x2b},
        {"mov $0x23,%gs", load_gs_sel, 0x23},
        {"pop %gs of 0x2b", pop_gs_sel, 0x2b},
        {"mov $0,%gs", load_gs_sel, 0},
        {"mov $3,%gs", load_gs_sel, 3},
        {"pop %gs of 0", pop_gs_sel, 0},
    };
    char what[96];
    for (unsigned i = 0; i < sizeof(loads) / sizeof(loads[0]); i++) {
        set_gs((uint64_t) (uintptr_t) gsbuf);
        load_sel = loads[i].sel;
        if (!run_clean(loads[i].what, loads[i].fn))
            continue;
        check_u64(loads[i].what, sel_out, loads[i].sel);
        snprintf(what, sizeof what, "gs base after %s", loads[i].what);
        check_u64(what, get_gs(),
                  (loads[i].sel & ~3) == 0 && !null_clears ? (uint64_t) (uintptr_t) gsbuf : 0);
    }
    // ARCH_SET_GS leaves the selector 0, and the base is the one set.
    load_sel = 0x2b;
    run(load_gs_sel);
    set_gs((uint64_t) (uintptr_t) gsbuf);
    if (run_clean("mov %gs,%eax after ARCH_SET_GS", read_gs_sel))
        check_u64("gs selector after ARCH_SET_GS", sel_out, 0);
    fill();
    if (run_clean("mov %gs:8 after a selector load and ARCH_SET_GS", ld_abs))
        check_u64("mov %gs:8 after a selector load and ARCH_SET_GS", out_rax, gq(1));
}

// ---- arch_prctl edge cases ----
// ARCH_SET_FS of a bad base, in one asm block: if it were to succeed, nothing
// may touch TLS before the good base is put back.
static long set_fs_bad(uint64_t bad) {
    long ret;
    uint64_t arg = bad;
    __asm__ volatile("syscall\n\t"
                     "test %%rax, %%rax\n\t"
                     "jnz 2f\n\t"
                     "mov %[good], %%rsi\n\t"
                     "mov $158, %%eax\n\t"
                     "syscall\n\t"
                     "xor %%eax, %%eax\n"
                     "2:"
                     : "=a"(ret), "+S"(arg)
                     : "0"(158L), "D"((long) ARCH_SET_FS_), [good] "r"(fs_base0)
                     : "rcx", "r11", "memory");
    return ret;
}

static void check_arch_prctl(void) {
    static const uint64_t bad[] = {
        TASK_SIZE_MAX_, TASK_SIZE_MAX_ + 0x1000, 0x800000000000ULL,
        0xffff800000000000ULL, 0xfffffffffffff000ULL,
    };
    char what[96];
    uint64_t good = (uint64_t) (uintptr_t) gsbuf;
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        set_gs(good);
        errno = 0;
        long r = arch_prctl_(ARCH_SET_GS_, bad[i]);
        int e = errno;
        snprintf(what, sizeof what, "ARCH_SET_GS %#llx", (unsigned long long) bad[i]);
        check_u64(what, r == -1 ? (uint64_t) e : 0, EPERM);
        snprintf(what, sizeof what, "gs base after a refused ARCH_SET_GS %#llx",
                 (unsigned long long) bad[i]);
        check_u64(what, get_gs(), good);
        snprintf(what, sizeof what, "ARCH_SET_FS %#llx", (unsigned long long) bad[i]);
        long rf = set_fs_bad(bad[i]);
        check_u64(what, rf < 0 ? (uint64_t) -rf : 0, EPERM);
    }
    // The highest page is not refused.
    check_u64("ARCH_SET_GS TASK_SIZE_MAX - 1", arch_prctl_(ARCH_SET_GS_, TASK_SIZE_MAX_ - 1), 0);
    check_u64("ARCH_GET_GS after it", get_gs(), TASK_SIZE_MAX_ - 1);
    errno = 0;
    long r = arch_prctl_(ARCH_GET_GS_, 8);
    check_u64("ARCH_GET_GS to address 8", r == -1 ? (uint64_t) errno : 0, EFAULT);
    uint64_t fs = 0;
    arch_prctl_(ARCH_GET_FS_, (uint64_t) (uintptr_t) &fs);
    check_u64("fs base untouched by every ARCH_SET_GS", fs, fs_base0);
    set_gs(0);
}

// ---- Signals ----
static volatile uint64_t h_read, h_base;
static volatile int h_set;
static void usr1_handler(int sig, siginfo_t *si, void *ucv) {
    (void) sig;
    (void) si;
    (void) ucv;
    uint64_t v;
    __asm__ volatile("mov %%gs:8, %0" : "=r"(v));
    h_read = v;
    h_base = get_gs();
    if (h_set)
        set_gs((uint64_t) (uintptr_t) &gsbuf[8]);
}

static void check_signal(void) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = usr1_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, &old);

    fill();
    set_gs((uint64_t) (uintptr_t) gsbuf);
    h_set = 0;
    h_read = h_base = 0;
    raise(SIGUSR1);
    check_u64("mov %gs:8 inside a signal handler", h_read, gq(1));
    check_u64("ARCH_GET_GS inside a signal handler", h_base, (uint64_t) (uintptr_t) gsbuf);
    check_u64("ARCH_GET_GS after sigreturn", get_gs(), (uint64_t) (uintptr_t) gsbuf);

    h_set = 1;
    raise(SIGUSR1);
    check_u64("a base set in the handler survives sigreturn", get_gs(),
              (uint64_t) (uintptr_t) &gsbuf[8]);
    if (run_clean("mov %gs:8 after the handler's ARCH_SET_GS", ld_abs))
        check_u64("mov %gs:8 after the handler's ARCH_SET_GS", out_rax, gq(9));
    sigaction(SIGUSR1, &old, NULL);
    set_gs(0);
}

// ---- fork and threads ----
static uint64_t read_gs8(void) {
    uint64_t v;
    __asm__ volatile("mov %%gs:8, %0" : "=r"(v));
    return v;
}

static volatile uint64_t t_base, t_read, t_base_own;
static void *thread_fn(void *arg) {
    (void) arg;
    t_base = get_gs();
    t_read = read_gs8();
    set_gs((uint64_t) (uintptr_t) &gsbuf[16]);
    t_base_own = get_gs();
    return NULL;
}

static void check_inherit(void) {
    fill();
    set_gs((uint64_t) (uintptr_t) gsbuf);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0)
        _exit(get_gs() == (uint64_t) (uintptr_t) gsbuf && read_gs8() == gq(1) ? 0 : 1);
    int status = 0;
    checks++;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
        printf("FAIL fork: the child did not inherit the gs base (status %#x)\n", status);
        failures_total++;
    }

    pthread_t t;
    checks++;
    if (pthread_create(&t, NULL, thread_fn, NULL) != 0 || pthread_join(t, NULL) != 0) {
        printf("FAIL pthread_create/join\n");
        failures_total++;
        set_gs(0);
        return;
    }
    check_u64("a new thread's gs base", t_base, (uint64_t) (uintptr_t) gsbuf);
    check_u64("mov %gs:8 in a new thread", t_read, gq(1));
    check_u64("a thread's own ARCH_SET_GS", t_base_own, (uint64_t) (uintptr_t) &gsbuf[16]);
    check_u64("...leaves the creator's gs base alone", get_gs(), (uint64_t) (uintptr_t) gsbuf);
    set_gs(0);
}

// ---- exec ----
// The re-executed image: the base is 0, and so a GS access faults at its
// offset. Exits 0 when both hold.
static int exec_child(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    if (get_gs() != 0)
        return 1;
    if (run(pf_ld) != SIGSEGV || f_code != SEGV_MAPERR || f_addr != 0x10)
        return 2;
    return 0;
}

static void check_exec(void) {
    fill();
    set_gs((uint64_t) (uintptr_t) gsbuf);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        char *argv[] = {"amd64_gs_base", "--exec-child", NULL};
        execv("/proc/self/exe", argv);
        _exit(99);
    }
    int status = 0;
    checks++;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
        printf("FAIL exec: the new image's gs base was not 0 (status %#x)\n", status);
        failures_total++;
    }

    // At the exec stop, before the new image has run an instruction.
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(3);
        char *argv[] = {"amd64_gs_base", "--exec-stop", NULL};
        execv("/proc/self/exe", argv);
        _exit(99);
    }
    checks++;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status) ||
            WSTOPSIG(status) != SIGTRAP) {
        printf("FAIL exec stop: status %#x\n", status);
        failures_total++;
        if (pid > 0) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
        set_gs(0);
        return;
    }
    struct user_regs_struct regs;
    checks++;
    if (ptrace(PTRACE_GETREGS, pid, 0, &regs) != 0) {
        printf("FAIL exec stop: GETREGS: %s\n", strerror(errno));
        failures_total++;
    } else {
        check_u64("fs_base at the exec stop", regs.fs_base, 0);
        check_u64("gs_base at the exec stop", regs.gs_base, 0);
    }
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &status, 0);
    set_gs(0);
}

// ---- ptrace ----
static void check_ptrace(void) {
    fill();
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(3);
        if (!set_gs((uint64_t) (uintptr_t) gsbuf))
            _exit(4);
        raise(SIGSTOP);
        // The tracer moved the base 8 bytes up.
        _exit(get_gs() == (uint64_t) (uintptr_t) &gsbuf[1] && read_gs8() == gq(2) ? 0 : 1);
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
    checks++;
    if (ptrace(PTRACE_GETREGS, pid, 0, &regs) != 0) {
        printf("FAIL ptrace: GETREGS: %s\n", strerror(errno));
        failures_total++;
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return;
    }
    check_u64("ptrace gs_base", regs.gs_base, (uint64_t) (uintptr_t) gsbuf);
    check_u64("ptrace fs_base", regs.fs_base, fs_base0);
    check_u64("ptrace gs", regs.gs, 0);
    errno = 0;
    long peek = ptrace(PTRACE_PEEKUSER, pid,
                       (void *) offsetof(struct user, regs.gs_base), 0);
    check_u64("PEEKUSER gs_base", errno == 0 ? (uint64_t) peek : PATTERN,
              (uint64_t) (uintptr_t) gsbuf);
    regs.gs_base = (uint64_t) (uintptr_t) &gsbuf[1];
    checks++;
    if (ptrace(PTRACE_SETREGS, pid, 0, &regs) != 0) {
        printf("FAIL ptrace: SETREGS: %s\n", strerror(errno));
        failures_total++;
    }
    ptrace(PTRACE_CONT, pid, 0, 0);
    checks++;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL ptrace: after SETREGS the child saw another gs base "
               "(status %#x)\n", status);
        failures_total++;
    }
}

static int has_avx(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d))
        return 0;
    return (c >> 28) & 1;
}
static void ld_vex(void) {
    uint64_t r;
    __asm__ volatile("vmovdqu %%gs:16, %%xmm0\n\tvmovq %%xmm0, %0" : "=r"(r) :: "xmm0");
    out_rax = r;
}

static void set_in(uint64_t rax, uint64_t rdi, uint64_t rsi, uint64_t rcx, uint64_t rbx) {
    fill();
    in_rax = rax;
    in_rdi = rdi;
    in_rsi = rsi;
    in_rcx = rcx;
    in_rbx = rbx;
}

// Run a probe that must complete, and check rax.
static void load(const char *what, void (*fn)(void), uint64_t want) {
    if (run_clean(what, fn))
        check_u64(what, out_rax, want);
}
// ...and check one qword of gsbuf.
static void store(const char *what, void (*fn)(void), unsigned slot, uint64_t want) {
    if (run_clean(what, fn))
        check_u64(what, gsbuf[slot], want);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--exec-child") == 0)
        return exec_child();
    if (argc == 2 && strcmp(argv[1], "--exec-stop") == 0)
        return 0;
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

    // Positive controls: the harness must see a known #UD, and a known
    // unprefixed MAPERR at 0x10, at their addresses, before a GS fault or
    // the absence of one means anything.
    if (run(ud_control) != SIGILL || f_rip != expect_ip) {
        printf("FAIL harness: ud2 gave %s at %#llx (want SIGILL at %#llx)\n",
               signame(f_sig), (unsigned long long) f_rip,
               (unsigned long long) expect_ip);
        failures_total++;
        return finish_suite(suite);
    }
    run_fault("mov 0x10,%rax (no prefix)", pf_flat, SIGSEGV, SEGV_MAPERR, 0x10);
    if (arch_prctl_(ARCH_GET_FS_, (uint64_t) (uintptr_t) &fs_base0) != 0 || fs_base0 == 0) {
        printf("FAIL harness: ARCH_GET_FS: %s\n", strerror(errno));
        failures_total++;
        return finish_suite(suite);
    }

    // The base from exec: 0.
    check_u64("ARCH_GET_GS at start", get_gs(), 0);
    run_fault("mov %gs:0x10,%rax at base 0", pf_ld, SIGSEGV, SEGV_MAPERR, 0x10);
    run_fault("addr32 mov %gs:(%eax),%rax at base 0", pf_ld_a32, SIGSEGV, SEGV_MAPERR, 0x10);
    run_fault("movb $1,%gs:0x18 at base 0", pf_st, SIGSEGV, SEGV_MAPERR, 0x18);
    run_fault("gs lodsq at base 0", pf_lods, SIGSEGV, SEGV_MAPERR, 0x20);

    // ARCH_SET_GS, and every form through the new base.
    checks++;
    if (!set_gs((uint64_t) (uintptr_t) gsbuf)) {
        printf("FAIL ARCH_SET_GS: %s\n", strerror(errno));
        failures_total++;
        return finish_suite(suite);
    }
    check_u64("ARCH_GET_GS after ARCH_SET_GS", get_gs(), (uint64_t) (uintptr_t) gsbuf);

    set_in(PATTERN, 8, 0, 1, 0);
    load("mov %gs:8,%rax", ld_abs, gq(1));
    load("mov %gs:(%rdi),%rax", ld_base, gq(1));
    load("mov %gs:8(%rdi,%rcx,8),%rax", ld_sib, gq(3));
    load("movb %gs:10,%al", ld_byte, (PATTERN & ~0xffULL) | gb(10));
    load("movw %gs:12,%ax", ld_word, (PATTERN & ~0xffffULL) | gb(12) | (uint64_t) gb(13) << 8);
    load("movzbl %gs:9,%eax", ld_zx, gb(9));
    load("movslq %gs:8,%rax", ld_sx, (uint64_t) (int64_t) (int32_t) gq(1));
    load("mov %gs:0x10 (moffs)", ld_moffs, gq(2));
    load("movq %gs:8,%xmm0", ld_movq_xmm, gq(1));
    load("movdqu %gs:16,%xmm0", ld_movdqu, gq(3));
    load("pushq %gs:8", ld_push, gq(1));
    load("jmp; mov %gs:8,%rax", ld_after_jmp, gq(1));
    set_in(3, 8, 0, 1, 0);
    load("imul %gs:16,%rax", ld_imul, gq(2) * 3);
    set_in(1, 8, 0, 1, 0);
    load("add %gs:8,%rax", ld_add, gq(1) + 1);
    load("btq $4,%gs:8", ld_bt, (gq(1) >> 4) & 1);
    set_in(gq(1), 8, 0, 1, 0);
    load("cmp %gs:8,%rax", ld_cmp, 1);
    set_in(gq(3), 8, 0, 1, 0);
    load("test %rax,%gs:24", ld_test, 1);
    set_in(PATTERN, 8, 0, 1, 0);
    load("cmove %gs:32,%rax", ld_cmov, gq(4));
    load("lea %gs:8(%rdi),%rax", ld_lea, 16);
    load("65 mov %rdi,%rax", ld_reg, 8);
    set_in(5, 8, 0, 1, 0);
    load("65 adc $1,%eax (accumulator form)", ld_adc_acc, 7);
    set_in(PATTERN, 8, 0, 1, 0);
    load("addr32 mov %gs:(%edi),%rax", ld_a32, gq(1));
    set_in(PATTERN, 0xffffffff00000010ULL, 0, 1, 0);
    load("addr32 mov %gs:(%edi),%rax, high bits of rdi", ld_a32, gq(2));
    set_in(1, 0xffffffff00000008ULL, 0, 1, 0);
    load("addr32 add %gs:8(%edi),%rax", ld_a32_add, gq(2) + 1);
    set_in(PATTERN, 8, 24, 1, 0);
    load("gs lodsq", ld_lods, gq(3));
    check_u64("gs lodsq advances rsi", out_rsi, 32);
    set_in(3, 8, 0, 1, 16);
    load("gs xlatb", ld_xlat, gb(19));
    set_in(PATTERN, 8, 0, 1, 0);
    gsbuf[4] = 12345;
    in_rsi = (uint64_t) (uintptr_t) &flatbuf[0];
    if (run_clean("fildll %gs:32", ld_x87))
        check_u64("fildll %gs:32", flatbuf[0], 12345);
    set_in(PATTERN, 8, 0, 1, 0);
    gsbuf[25] = (uint64_t) (uintptr_t) gs_call_target;
    load("call *%gs:200", ld_call, 0x5a5a5a5a);
    if (has_avx()) {
        fill();
        load("vmovdqu %gs:16,%xmm0", ld_vex, gq(2));
    } else {
        checks += 2;
    }
    fill();
    if (run_clean("mov %fs:0 and mov %gs:8 in one block", fs_and_gs)) {
        check_u64("mov %fs:0 beside a gs access", fs0_seen, fs_base0);
        check_u64("mov %gs:8 beside an fs access", out_rax, gq(1));
    }

    set_in(0x1122334455667788ULL, 8, 0, 1, 0);
    store("mov %rax,%gs:48", st_mov, 6, 0x1122334455667788ULL);
    store("movb %al,%gs:56", st_byte, 7, (gq(7) & ~0xffULL) | 0x88);
    store("movq $-2,%gs:64", st_imm, 8, (uint64_t) -2);
    store("movb $0x5a,%gs:193", st_imm8, 24, (gq(24) & ~0xff00ULL) | 0x5a00);
    store("addq $5,%gs:72", st_add83, 9, gq(9) + 5);
    store("subl $0x10000,%gs:80", st_sub81, 10,
          (gq(10) & ~0xffffffffULL) | (uint32_t) ((uint32_t) gq(10) - 0x10000));
    store("orb $0x80,%gs:88", st_or80, 11, gq(11) | 0x80);
    store("add %rax,%gs:96", st_add01, 12, gq(12) + 0x1122334455667788ULL);
    store("incq %gs:104", st_inc, 13, gq(13) + 1);
    store("negq %gs:112", st_neg, 14, -gq(14));
    store("xchg %rax,%gs:120", st_xchg, 15, 0x1122334455667788ULL);
    check_u64("xchg %rax,%gs:120 old value", out_rax, gq(15));
    store("lock xadd %rax,%gs:128", st_lock_xadd, 16, gq(16) + 0x1122334455667788ULL);
    check_u64("lock xadd old value", out_rax, gq(16));
    set_in(gq(17), 0x99, 0, 1, 0);
    store("lock cmpxchg %rdi,%gs:136", st_lock_cmpxchg, 17, 0x99);
    set_in(0x1122334455667788ULL, 8, 0, 1, 0);
    store("lock incq %gs:144", st_lock_inc, 18, gq(18) + 1);
    store("popq %gs:152", st_pop, 19, 0x1122334455667788ULL);
    store("movdqu %xmm0,%gs:160", st_movdqu, 20, 0x1122334455667788ULL);
    check_u64("movdqu %xmm0,%gs:160, high half", gsbuf[21], 0);
    store("btsq $1,%gs:176", st_bts, 22, gq(22) | 2);
    store("sete %gs:184", st_setcc, 23, (gq(23) & ~0xffULL) | 1);
    set_in(0x1122334455667788ULL, 0xffffffff00000030ULL, 0, 1, 0);
    store("addr32 mov %rax,%gs:(%edi)", st_a32, 6, 0x1122334455667788ULL);
    set_in(0, (uint64_t) (uintptr_t) &flatbuf[1], 40, 1, 0);
    if (run_clean("gs movsq", st_movs)) {
        check_u64("gs movsq: from gs:rsi to es:rdi", flatbuf[1], gq(5));
        check_u64("gs movsq advances rsi", out_rsi, 48);
    }
    set_in(0x77, (uint64_t) (uintptr_t) &flatbuf[2], 0, 1, 0);
    if (run_clean("gs stosq", st_stos)) {
        check_u64("gs stosq stores at es:rdi", flatbuf[2], 0x77);
        check_u64("gs stosq leaves gs:rdi alone", gsbuf[0], gq(0));
    }

    check_selectors();
    check_arch_prctl();
    check_signal();
    check_inherit();
    check_exec();
    check_ptrace();

    set_gs(0);
    check_u64("ARCH_GET_GS after ARCH_SET_GS 0", get_gs(), 0);
    run_fault("mov %gs:0x10,%rax at base 0 again", pf_ld, SIGSEGV, SEGV_MAPERR, 0x10);

    // Every probe above counts itself; a siglongjmp that landed in the wrong
    // place would skip some silently.
    checks++;
    if (checks != 186) {
        printf("FAIL harness: ran %u checks, want 186\n", checks);
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
