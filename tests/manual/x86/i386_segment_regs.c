// i386 segment registers: MOV r/m, Sreg (8C), MOV Sreg, r/m (8E), PUSH and
// POP of ES, CS, SS, DS (06 07 0E 16 17 1E 1F) and of FS and GS (0F A0 A1 A8
// A9), the ES and SS override prefixes, and the TLS descriptors behind FS and
// GS (set_thread_area, get_thread_area).
//
// The i386 engine read only FS and GS through 8C, and both returned GS's
// selector; ES, CS, SS and DS were SIGILL, as was every PUSH and POP of a
// segment register and the 26/36 prefixes. glibc's getcontext stores FS with
// 8C, its pthread_create and musl's clone read GS with it, and hand-written
// code pushes and pops DS and ES.
//
// Everything below was measured on x86_64 Linux 6.12 (camd, AMD Zen+) with
// gcc -m32 (glibc) and passes there unchanged. A 32-bit task on a 64-bit
// kernel sees that kernel's GDT, which is the one iSH-AOK presents:
//
//  - CS reads 0x23; SS, DS and ES 0x2b from exec on; FS 0; GS 0 until libc
//    loads the TLS entry set_thread_area gave it, 12 (0x63).
//  - A memory destination takes exactly two bytes. A register destination
//    takes the operand size: 32 bits zero-extends, 66 keeps bits 16-31.
//  - Encodings 6 and 7 of the Sreg field are #UD, with a memory operand too,
//    and so is CS as a destination, and LOCK.
//  - Loadable into ES, DS, FS and GS: null, GDT entries 4 (0x23), 5 (0x2b),
//    6 (0x33) and 15 (0x7b), with any RPL, and a TLS entry (12-14) once
//    set_thread_area has filled it. SS takes 0x2b and a filled, writable TLS
//    entry, RPL 3 only. Everything else -- kernel entries, TSS and LDT
//    descriptors, empty TLS slots, anything past the GDT, any LDT selector --
//    is #GP: SIGSEGV, si_code SI_KERNEL, at the instruction, and a POP that
//    faults leaves ESP alone.
//  - PUSH moves four bytes (two with 66) and stores the selector in the low
//    word; the high word is zeroed on AMD and left alone on recent Intel
//    parts, so either is accepted. POP takes the low word of four bytes.
//  - An FS or GS override adds the base of the descriptor the register
//    selects, and set_thread_area on an entry FS or GS holds moves it.
//    Clearing that entry leaves the register null.
//  - set_thread_area(-1) takes the first free entry; with all three used it
//    is ESRCH. Entries outside 12-14 are EINVAL, and so is a descriptor that
//    is code, 16-bit or not present without being empty. get_thread_area
//    reads an entry back as set_thread_area took it.
//  - A signal handler runs with DS and ES 0x2b and FS and GS as they were;
//    the frame holds all six. sigreturn reloads GS, FS, DS and ES from the
//    frame with RPL 3 forced, and one that cannot be loaded becomes null. A
//    frame whose CS or SS cannot be returned to is SIGSEGV at the resume point.
//  - fork inherits the selectors and the TLS entries; exec resets them.
//  - PTRACE_GETREGS reports all six, SETREGS changes them and refuses a
//    selector whose RPL is not 3 (EIO), and PTRACE_GET_THREAD_AREA and
//    SET_THREAD_AREA read and write a TLS entry of the tracee.
//
// Not asserted: si_addr of a #GP (Linux reports NULL), the error code in
// REG_ERR, and memory access through a null FS or GS, or through DS, ES or SS
// holding a TLS selector -- the base is applied for FS and GS only, so those
// are flat here where Linux faults or adds the TLS base.
//
// i386 only; amd64 has its own test (amd64_segment_regs), and the two
// engines share nothing here.
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

static const char *suite = "i386_segment_regs";

#if defined(__i386__)

#include <stddef.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <ucontext.h>

#ifndef SI_KERNEL
#define SI_KERNEL 0x80
#endif
#ifndef SEGV_MAPERR
#define SEGV_MAPERR 1
#endif
#ifndef SEGV_ACCERR
#define SEGV_ACCERR 2
#endif
#define NR_SET_THREAD_AREA 243
#define NR_GET_THREAD_AREA 244
#define PTRACE_GET_THREAD_AREA_ 25
#define PTRACE_SET_THREAD_AREA_ 26
#define PATTERN 0xdeadbeefu
#define SEL_TLS 0x63            // GDT entry 12 with RPL 3: libc's TLS

// struct user_desc, with its bit-fields as one word: seg_32bit (bit 0),
// contents (1-2), read_exec_only (3), limit_in_pages (4), seg_not_present
// (5), useable (6).
struct tls_desc {
    uint32_t entry, base, limit, flags;
};
#define DESC_DATA 0x51          // what glibc and musl pass: 32-bit, pages, useable
#define DESC_RODATA 0x59        // the same, read-only
#define DESC_EMPTY 0x28         // read_exec_only | seg_not_present, all else 0

// The address of a probe's faulting instruction, as a symbol of its own.
#define LBL(name) ".globl " #name "\n\t.hidden " #name "\n" #name ":\n\t"
#define DECLARE_IP(name) extern const char name[]

static sigjmp_buf env;
static volatile int f_sig, f_code;
static volatile uint32_t f_addr, f_eip, f_esp;
static uint32_t out0, out1, out2;
static unsigned char membuf[128];
static uint32_t load_sel;
static unsigned checks;
static unsigned char *ro_page, *none_page, *hole_page;
static uint32_t tls_self;       // %gs:0, which is entry 12's base

static void fault_handler(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    f_sig = sig;
    f_code = si->si_code;
    f_addr = (uintptr_t) si->si_addr;
    f_eip = (uint32_t) uc->uc_mcontext.gregs[REG_EIP];
    f_esp = (uint32_t) uc->uc_mcontext.gregs[REG_ESP];
    siglongjmp(env, 1);
}

static int run(void (*fn)(void)) {
    f_sig = 0;
    if (sigsetjmp(env, 1) == 0)
        fn();
    return f_sig;
}

static void check_u32(const char *what, uint32_t got, uint32_t want) {
    checks++;
    if (got != want) {
        printf("FAIL %s: got %#x, want %#x\n", what, got, want);
        failures_total++;
    } else {
        test_logf("  ok   %s = %#x\n", what, got);
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
        printf("FAIL %s: %s (si_code %d) at %#x\n", what, signame(sig), f_code, f_eip);
        failures_total++;
        return 0;
    }
    return 1;
}

// A probe that must fault with want_sig/want_code, reported at ip.
static int run_fault(const char *what, void (*fn)(void), const char *ip,
        int want_sig, int want_code) {
    int sig = run(fn);
    checks++;
    if (sig != want_sig) {
        printf("FAIL %s: %s, want %s\n", what, signame(sig), signame(want_sig));
        failures_total++;
        return 0;
    }
    int ok = 1;
    if (f_code != want_code) {
        printf("FAIL %s: si_code %d, want %d\n", what, f_code, want_code);
        ok = 0;
    }
    if (f_eip != (uint32_t) (uintptr_t) ip) {
        printf("FAIL %s: reported at %#x, the instruction is at %#x\n", what, f_eip,
               (uint32_t) (uintptr_t) ip);
        ok = 0;
    }
    if (!ok)
        failures_total++;
    else
        test_logf("  ok   %s -> %s code %d\n", what, signame(sig), f_code);
    return ok;
}

// ---- 8C to a register ----
#define REG_READ(name, bytes)                                                  \
    static void name(void) {                                                   \
        uint32_t r = PATTERN;                                                  \
        __asm__ volatile(bytes : "+a"(r));                                     \
        out0 = r;                                                              \
    }
REG_READ(rd_cs_eax, ".byte 0x8c, 0xc8")               // mov %cs,%eax
REG_READ(rd_ss_eax, ".byte 0x8c, 0xd0")
REG_READ(rd_es_eax, ".byte 0x8c, 0xc0")
REG_READ(rd_ds_eax, ".byte 0x8c, 0xd8")
REG_READ(rd_fs_eax, ".byte 0x8c, 0xe0")
REG_READ(rd_gs_eax, ".byte 0x8c, 0xe8")
REG_READ(rd_cs_ax, ".byte 0x66, 0x8c, 0xc8")          // mov %cs,%ax
REG_READ(rd_gs_ax, ".byte 0x66, 0x8c, 0xe8")
static void rd_cs_ecx(void) {
    uint32_t r = PATTERN;
    __asm__ volatile(".byte 0x8c, 0xc9" : "+c"(r));          // mov %cs,%ecx
    out0 = r;
}
static void rd_ss_di(void) {
    uint32_t r = PATTERN;
    __asm__ volatile(".byte 0x66, 0x8c, 0xd7" : "+D"(r));    // mov %ss,%di
    out0 = r;
}
// Followed in the same block by more work: the JIT continues its block after
// the helper, and the results must still be there after it.
static void rd_cs_then_add(void) {
    uint32_t r = PATTERN;
    __asm__ volatile(".byte 0x8c, 0xc8\n\t"           // mov %cs,%eax
                     "addl $0x100, %%eax\n\t"
                     ".byte 0x8c, 0xd1\n\t"           // mov %ss,%ecx
                     "addl %%ecx, %%eax"
                     : "+a"(r) :: "ecx", "cc");
    out0 = r;
}

// ---- 8C to memory: always two bytes ----
#define MEM_READ(name, bytes)                                                  \
    static void name(void) {                                                   \
        memset(membuf, 0xaa, sizeof(membuf));                                  \
        __asm__ volatile(bytes :: "D"(membuf), "c"(5) : "memory");             \
    }
MEM_READ(mr_cs, ".byte 0x8c, 0x0f")                    // mov %cs,(%edi)
MEM_READ(mr_ss_66, ".byte 0x66, 0x8c, 0x17")           // mov %ss,(%edi)
MEM_READ(mr_gs_disp8, ".byte 0x8c, 0x6f, 0x38")        // mov %gs,0x38(%edi)
MEM_READ(mr_ds_sib, ".byte 0x8c, 0x5c, 0x8f, 0x04")    // mov %ds,4(%edi,%ecx,4)
MEM_READ(mr_fs_disp32, ".byte 0x8c, 0xa7, 0x40, 0x00, 0x00, 0x00") // mov %fs,0x40(%edi)

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
DECLARE_IP(ip_ud_control);
DECLARE_IP(ip_ud_8c_reg6);
DECLARE_IP(ip_ud_8c_reg7);
DECLARE_IP(ip_ud_8c_mem6);
DECLARE_IP(ip_ud_8c_lock);
DECLARE_IP(ip_ud_8e_cs);
DECLARE_IP(ip_ud_8e_reg6);
DECLARE_IP(ip_ud_8e_reg7);
DECLARE_IP(ip_ud_8e_lock);
DECLARE_IP(ip_pf_8c_null);
DECLARE_IP(ip_pf_8e_null);
DECLARE_IP(ip_acc_8c_ro);
DECLARE_IP(ip_acc_8e_none);

static void ud_control(void) {
    __asm__ volatile(LBL(ip_ud_control) "ud2");
}
static void ud_8c_reg6(void) {
    __asm__ volatile(LBL(ip_ud_8c_reg6) ".byte 0x8c, 0xf0" ::: "eax");
}
static void ud_8c_reg7(void) {
    __asm__ volatile(LBL(ip_ud_8c_reg7) ".byte 0x8c, 0xf8" ::: "eax");
}
static void ud_8c_mem6(void) {
    __asm__ volatile(LBL(ip_ud_8c_mem6) ".byte 0x8c, 0x37" :: "D"(membuf) : "memory");
}
static void ud_8c_lock(void) {
    __asm__ volatile(LBL(ip_ud_8c_lock) ".byte 0xf0, 0x8c, 0x0f" :: "D"(membuf) : "memory");
}
static void ud_8e_cs(void) {
    __asm__ volatile(LBL(ip_ud_8e_cs) ".byte 0x8e, 0xc8" :: "a"(0x23));
}
static void ud_8e_reg6(void) {
    __asm__ volatile(LBL(ip_ud_8e_reg6) ".byte 0x8e, 0xf0" :: "a"(0x2b));
}
static void ud_8e_reg7(void) {
    __asm__ volatile(LBL(ip_ud_8e_reg7) ".byte 0x8e, 0xf8" :: "a"(0x2b));
}
static void ud_8e_lock(void) {
    __asm__ volatile(LBL(ip_ud_8e_lock) ".byte 0xf0, 0x8e, 0xc0" :: "a"(0x2b));
}
static void pf_8c_null(void) {
    __asm__ volatile(LBL(ip_pf_8c_null) ".byte 0x8c, 0x08" :: "a"(0) : "memory");
}
static void pf_8e_null(void) {
    uint16_t old;
    __asm__ volatile("mov %%es, %0\n\t" LBL(ip_pf_8e_null) ".byte 0x8e, 0x00\n\t"
                     "mov %0, %%es" : "=&r"(old) : "a"(0));
}
static void acc_8c_ro(void) {
    __asm__ volatile(LBL(ip_acc_8c_ro) ".byte 0x8c, 0x08" :: "a"(ro_page) : "memory");
}
static void acc_8e_none(void) {
    uint16_t old;
    __asm__ volatile("mov %%es, %0\n\t" LBL(ip_acc_8e_none) ".byte 0x8e, 0x00\n\t"
                     "mov %0, %%es" : "=&r"(old) : "a"(none_page));
}

// ---- 8E loads, each put back before anything can use it ----
DECLARE_IP(ip_ld_es);
DECLARE_IP(ip_ld_ds);
DECLARE_IP(ip_ld_fs);
DECLARE_IP(ip_ld_gs);
DECLARE_IP(ip_ld_ss);
#define LD_PROBE(name, seg)                                                    \
    static void name(void) {                                                   \
        uint32_t got, old;                                                     \
        __asm__ volatile("mov %%" #seg ", %[o]\n\t"                            \
                         LBL(ip_##name) "mov %[s], %%" #seg "\n\t"             \
                         "mov %%" #seg ", %[g]\n\t"                            \
                         "mov %[o], %%" #seg                                   \
                         : [g] "=&r"(got), [o] "=&r"(old) : [s] "r"(load_sel)); \
        out0 = got;                                                            \
    }
LD_PROBE(ld_es, es)
LD_PROBE(ld_ds, ds)
LD_PROBE(ld_fs, fs)
LD_PROBE(ld_gs, gs)
LD_PROBE(ld_ss, ss)

// The same through memory (two bytes of it), from the low word of a register,
// with 66, and from a register other than EAX.
static void ld_es_mem(void) {
    uint32_t got, old;
    memset(membuf, 0xff, sizeof(membuf));
    membuf[0] = 0x2b;
    membuf[1] = 0;
    __asm__ volatile("mov %%es, %[o]\n\t"
                     ".byte 0x8e, 0x07\n\t"           // mov (%edi),%es
                     "mov %%es, %[g]\n\t"
                     "mov %[o], %%es"
                     : [g] "=&r"(got), [o] "=&r"(old) : "D"(membuf) : "memory");
    out0 = got;
}
static void ld_es_high(void) {
    uint32_t got, old;
    __asm__ volatile("mov %%es, %[o]\n\tmov %[s], %%es\n\tmov %%es, %[g]\n\tmov %[o], %%es"
                     : [g] "=&r"(got), [o] "=&r"(old) : [s] "r"(0xffff0023u));
    out0 = got;
}
static void ld_es_66(void) {
    uint32_t got, old;
    __asm__ volatile("mov %%es, %[o]\n\t"
                     ".byte 0x66, 0x8e, 0xc0\n\t"     // mov %ax,%es
                     "mov %%es, %[g]\n\t"
                     "mov %[o], %%es"
                     : [g] "=&r"(got), [o] "=&r"(old) : "a"(0x7b));
    out0 = got;
}
static void ld_fs_ecx_then_add(void) {
    uint32_t r = 1, old;
    __asm__ volatile("mov %%fs, %[o]\n\t"
                     ".byte 0x8e, 0xe1\n\t"           // mov %ecx,%fs
                     "addl $0x100, %[r]\n\t"
                     ".byte 0x8c, 0xe2\n\t"           // mov %fs,%edx
                     "addl %%edx, %[r]\n\t"
                     "mov %[o], %%fs"
                     : [r] "+r"(r), [o] "=&r"(old) : "c"(0x33) : "edx", "cc");
    out0 = r;
}

// ---- PUSH / POP ----
// PUSH into a slot pre-filled with ones: the low word is the selector, and
// the high word 0 (AMD) or the ones (Intel's 16-bit move).
#define PUSH32(name, bytes)                                                    \
    static void name(void) {                                                   \
        uint32_t v, sp0, sp1;                                                  \
        __asm__ volatile("movl $0xffffffff, -4(%%esp)\n\t"                     \
                         "mov %%esp, %[a]\n\t"                                 \
                         bytes "\n\t"                                          \
                         "mov %%esp, %[b]\n\t"                                 \
                         "pop %[v]"                                            \
                         : [v] "=&r"(v), [a] "=&r"(sp0), [b] "=&r"(sp1)        \
                         :: "memory");                                         \
        out0 = v;                                                              \
        out1 = sp0 - sp1;                                                      \
    }
PUSH32(push_es, ".byte 0x06")
PUSH32(push_cs, ".byte 0x0e")
PUSH32(push_ss, ".byte 0x16")
PUSH32(push_ds, ".byte 0x1e")
PUSH32(push_fs, ".byte 0x0f, 0xa0")
PUSH32(push_gs, ".byte 0x0f, 0xa8")
// With 66: ESP moves two, and exactly two bytes are stored.
#define PUSH16(name, bytes)                                                    \
    static void name(void) {                                                   \
        uint32_t v, sp0, sp1;                                                  \
        __asm__ volatile("movl $0xffffffff, -4(%%esp)\n\t"                     \
                         "mov %%esp, %[a]\n\t"                                 \
                         bytes "\n\t"                                          \
                         "mov %%esp, %[b]\n\t"                                 \
                         "mov -2(%%esp), %[v]\n\t"                             \
                         "add $2, %%esp"                                       \
                         : [v] "=&r"(v), [a] "=&r"(sp0), [b] "=&r"(sp1)        \
                         :: "memory");                                         \
        out0 = v;                                                              \
        out1 = sp0 - sp1;                                                      \
    }
PUSH16(pushw_es, ".byte 0x66, 0x06")
PUSH16(pushw_gs, ".byte 0x66, 0x0f, 0xa8")

// POP of a pushed dword: the low word loads, ESP moves four.
#define POP32(name, seg, bytes, value)                                         \
    static void name(void) {                                                   \
        uint32_t got, old, sp0, sp1;                                           \
        __asm__ volatile("mov %%" #seg ", %[o]\n\t"                            \
                         "mov %%esp, %[a]\n\t"                                 \
                         "push %[v]\n\t"                                       \
                         bytes "\n\t"                                          \
                         "mov %%esp, %[b]\n\t"                                 \
                         "mov %%" #seg ", %[g]\n\t"                            \
                         "mov %[o], %%" #seg                                   \
                         : [g] "=&r"(got), [o] "=&r"(old), [a] "=&r"(sp0),     \
                           [b] "=&r"(sp1)                                      \
                         : [v] "r"(value) : "memory");                         \
        out0 = got;                                                            \
        out1 = sp1 - sp0;                                                      \
    }
POP32(pop_es, es, ".byte 0x07", 0xffff002bu)
POP32(pop_ds, ds, ".byte 0x1f", 0x2bu)
POP32(pop_ss, ss, ".byte 0x17", 0x2bu)
POP32(pop_fs, fs, ".byte 0x0f, 0xa1", 0x7bu)
POP32(pop_gs, gs, ".byte 0x0f, 0xa9", (uint32_t) SEL_TLS)
// With 66: two bytes, ESP moves two. The word goes on with pushw, which
// moves two bytes as well (tests/manual/x86/i386_push16.c).
#define POP16(name, seg, bytes, value)                                         \
    static void name(void) {                                                   \
        uint32_t got, old, sp0, sp1;                                           \
        __asm__ volatile("mov %%" #seg ", %[o]\n\t"                            \
                         "mov %%esp, %[a]\n\t"                                 \
                         "pushw $" #value "\n\t"                               \
                         bytes "\n\t"                                          \
                         "mov %%esp, %[b]\n\t"                                 \
                         "mov %%" #seg ", %[g]\n\t"                            \
                         "mov %[o], %%" #seg                                   \
                         : [g] "=&r"(got), [o] "=&r"(old), [a] "=&r"(sp0),     \
                           [b] "=&r"(sp1) :: "memory");                        \
        out0 = got;                                                            \
        out1 = sp1 - sp0;                                                      \
    }
POP16(popw_es, es, ".byte 0x66, 0x07", 0x23)
POP16(popw_fs, fs, ".byte 0x66, 0x0f, 0xa1", 0x33)

// A POP that #GPs leaves ESP where it was: out1 is ESP just before it.
DECLARE_IP(ip_pop_es_bad);
DECLARE_IP(ip_pop_ss_null);
static void pop_es_bad(void) {
    __asm__ volatile("push %[v]\n\t"
                     "mov %%esp, %[sp]\n\t"
                     LBL(ip_pop_es_bad) ".byte 0x07\n\t"      // pop %es
                     "push %%ss\n\t"
                     "pop %%es"
                     : [sp] "=m"(out1) : [v] "r"(0x10) : "memory");
}
static void pop_ss_null(void) {
    __asm__ volatile("push %[v]\n\t"
                     "mov %%esp, %[sp]\n\t"
                     LBL(ip_pop_ss_null) ".byte 0x17\n\t"     // pop %ss
                     "push %%ds\n\t"
                     "pop %%ss"
                     : [sp] "=m"(out1) : [v] "r"(0) : "memory");
}
// A stack fault: ESP at a page that cannot take the access. The handler runs
// on the alternate stack, and the probe's ESP comes back with the siglongjmp.
DECLARE_IP(ip_push_es_ro);
DECLARE_IP(ip_push_fs_hole);
DECLARE_IP(ip_pop_ds_none);
static void push_es_ro(void) {
    __asm__ volatile("mov %%esp, %%esi\n\t"
                     "mov %[t], %%esp\n\t"
                     LBL(ip_push_es_ro) ".byte 0x06\n\t"      // push %es
                     "mov %%esi, %%esp"
                     :: [t] "r"(ro_page + 4096) : "esi", "memory");
}
static void push_fs_hole(void) {
    __asm__ volatile("mov %%esp, %%esi\n\t"
                     "mov %[t], %%esp\n\t"
                     LBL(ip_push_fs_hole) ".byte 0x0f, 0xa0\n\t" // push %fs
                     "mov %%esi, %%esp"
                     :: [t] "r"(hole_page + 4096) : "esi", "memory");
}
static void pop_ds_none(void) {
    __asm__ volatile("mov %%esp, %%esi\n\t"
                     "mov %[t], %%esp\n\t"
                     LBL(ip_pop_ds_none) ".byte 0x1f\n\t"     // pop %ds
                     "mov %%esi, %%esp"
                     :: [t] "r"(none_page) : "esi", "memory");
}

// ---- override prefixes ----
static void es_prefix_load(void) {
    uint32_t v;
    __asm__ volatile(".byte 0x26, 0x8b, 0x07" : "=a"(v) : "D"(membuf) : "memory"); // mov %es:(%edi),%eax
    out0 = v;
}
static void ss_prefix_load(void) {
    uint32_t v;
    __asm__ volatile(".byte 0x36, 0x8b, 0x07" : "=a"(v) : "D"(membuf) : "memory"); // mov %ss:(%edi),%eax
    out0 = v;
}
static void es_prefix_store(void) {
    __asm__ volatile(".byte 0x26, 0x89, 0x47, 0x04" :: "D"(membuf), "a"(0x12345678) : "memory"); // mov %eax,%es:4(%edi)
}
static void gs_self(void) {
    uint32_t v;
    __asm__ volatile(".byte 0x65, 0xa1, 0, 0, 0, 0" : "=a"(v));    // mov %gs:0,%eax
    out0 = v;
}

// ---- TLS descriptors ----
static long set_ta(struct tls_desc *d) {
    return syscall(NR_SET_THREAD_AREA, d);
}
static long get_ta(struct tls_desc *d) {
    return syscall(NR_GET_THREAD_AREA, d);
}
static uint32_t tbuf[16], tbuf2[16];
static uint16_t fs_sel;

static void load_fs(void) {
    __asm__ volatile("mov %0, %%fs" :: "r"((uint32_t) fs_sel));
}
static void read_fs(void) {
    uint32_t v;
    __asm__ volatile("mov %%fs, %0" : "=r"(v));
    out0 = v;
}
static void fs_read0(void) {
    uint32_t v;
    __asm__ volatile(".byte 0x64, 0xa1, 0, 0, 0, 0" : "=a"(v));    // mov %fs:0,%eax
    out0 = v;
}
static void fs_write4(void) {
    __asm__ volatile(".byte 0x64, 0x89, 0x0d, 4, 0, 0, 0" :: "c"(0x5a5a1234) : "memory"); // mov %ecx,%fs:4
}
static void fs_8c_store(void) {
    __asm__ volatile(".byte 0x64, 0x8c, 0x0d, 8, 0, 0, 0" ::: "memory"); // mov %cs,%fs:8
}
static void fs_8e_load(void) {
    uint32_t got, old;
    __asm__ volatile("mov %%es, %[o]\n\t"
                     ".byte 0x64, 0x8e, 0x05, 12, 0, 0, 0\n\t"   // mov %fs:12,%es
                     "mov %%es, %[g]\n\t"
                     "mov %[o], %%es"
                     : [g] "=&r"(got), [o] "=&r"(old) :: "memory");
    out0 = got;
}
static void fs_push_mem(void) {
    uint32_t v;
    __asm__ volatile(".byte 0x64, 0xff, 0x35, 0, 0, 0, 0\n\t"      // push %fs:0
                     "pop %0" : "=r"(v) :: "memory");
    out0 = v;
}

static void check_desc(const char *what, struct tls_desc *d, uint32_t entry,
        uint32_t base, uint32_t limit, uint32_t flags) {
    char label[128];
    snprintf(label, sizeof label, "%s: entry", what);
    check_u32(label, d->entry, entry);
    snprintf(label, sizeof label, "%s: base", what);
    check_u32(label, d->base, base);
    snprintf(label, sizeof label, "%s: limit", what);
    check_u32(label, d->limit, limit);
    snprintf(label, sizeof label, "%s: flags", what);
    check_u32(label, d->flags & 0x7f, flags);
}

static int check_get(const char *what, uint32_t entry, uint32_t base, uint32_t limit,
        uint32_t flags) {
    struct tls_desc d = { .entry = entry, .base = 0x11111111, .limit = 0x2222, .flags = 0x7f };
    checks++;
    if (get_ta(&d) != 0) {
        printf("FAIL %s: get_thread_area(%u): %s\n", what, entry, strerror(errno));
        failures_total++;
        return 0;
    }
    check_desc(what, &d, entry, base, limit, flags);
    return 1;
}

static void check_errno(const char *what, long ret, int want) {
    checks++;
    if (ret != -1 || errno != want) {
        printf("FAIL %s: returned %ld (%s), want %s\n", what, ret,
               ret == -1 ? strerror(errno) : "no error", strerror(want));
        failures_total++;
    } else {
        test_logf("  ok   %s -> %s\n", what, strerror(want));
    }
}

static void check_tls(void) {
    struct tls_desc d;

    // Without entry 12 read back as libc set it, set_thread_area(-1) below
    // could hand out libc's own entry and move GS -- and with it the stack
    // protector's canary -- under this test.
    if (!check_get("entry 12 (libc's)", 12, tls_self, 0xfffff, DESC_DATA))
        return;
    check_get("entry 13 before use", 13, 0, 0, DESC_EMPTY);
    d = (struct tls_desc) { .entry = 11 };
    check_errno("get_thread_area(11)", get_ta(&d), EINVAL);
    d = (struct tls_desc) { .entry = 15 };
    check_errno("get_thread_area(15)", get_ta(&d), EINVAL);

    for (unsigned i = 0; i < 16; i++) {
        tbuf[i] = 0x10000000u + i;
        tbuf2[i] = 0x20000000u + i;
    }
    ((unsigned char *) tbuf)[12] = 0x7b;
    ((unsigned char *) tbuf)[13] = 0;

    d = (struct tls_desc) { .entry = (uint32_t) -1, .base = (uintptr_t) tbuf,
                            .limit = 0xfffff, .flags = DESC_DATA };
    checks++;
    if (set_ta(&d) != 0) {
        printf("FAIL set_thread_area(-1): %s\n", strerror(errno));
        failures_total++;
        return;
    }
    check_u32("set_thread_area(-1) picks the first free entry", d.entry, 13);
    check_get("entry 13 as set", 13, (uintptr_t) tbuf, 0xfffff, DESC_DATA);

    // FS on entry 13: its base is tbuf.
    fs_sel = 0x6b;
    if (!run_clean("mov $0x6b,%fs", load_fs))
        return;
    if (run_clean("mov %fs,%eax", read_fs))
        check_u32("fs after mov $0x6b,%fs", out0, 0x6b);
    if (run_clean("mov %fs:0,%eax", fs_read0))
        check_u32("mov %fs:0,%eax", out0, tbuf[0]);
    if (run_clean("mov %ecx,%fs:4", fs_write4))
        check_u32("mov %ecx,%fs:4", tbuf[1], 0x5a5a1234);
    if (run_clean("mov %cs,%fs:8", fs_8c_store)) {
        check_u32("mov %cs,%fs:8 stores two bytes", tbuf[2], 0x10000023);
    }
    if (run_clean("mov %fs:12,%es", fs_8e_load))
        check_u32("mov %fs:12,%es", out0, 0x7b);
    if (run_clean("push %fs:0", fs_push_mem))
        check_u32("push %fs:0", out0, tbuf[0]);

    // Moving entry 13 moves FS with it.
    d = (struct tls_desc) { .entry = 13, .base = (uintptr_t) tbuf2, .limit = 0xfffff,
                            .flags = DESC_DATA };
    checks++;
    if (set_ta(&d) != 0) {
        printf("FAIL set_thread_area(13): %s\n", strerror(errno));
        failures_total++;
    }
    if (run_clean("mov %fs:0,%eax after moving entry 13", fs_read0))
        check_u32("mov %fs:0,%eax after moving entry 13", out0, tbuf2[0]);
    if (run_clean("mov %fs,%eax after moving entry 13", read_fs))
        check_u32("fs after moving entry 13", out0, 0x6b);

    // Entry 14, read-only: loads into ES and FS, not SS.
    d = (struct tls_desc) { .entry = (uint32_t) -1, .base = (uintptr_t) tbuf,
                            .limit = 0xfff, .flags = DESC_RODATA };
    checks++;
    if (set_ta(&d) != 0) {
        printf("FAIL set_thread_area(-1) read-only: %s\n", strerror(errno));
        failures_total++;
    }
    check_u32("second set_thread_area(-1)", d.entry, 14);
    check_get("entry 14 as set", 14, (uintptr_t) tbuf, 0xfff, DESC_RODATA);
    d = (struct tls_desc) { .entry = (uint32_t) -1, .base = (uintptr_t) tbuf,
                            .limit = 0xfffff, .flags = DESC_DATA };
    check_errno("set_thread_area(-1) with none free", set_ta(&d), ESRCH);
    load_sel = 0x73;
    if (run_clean("mov $0x73,%es (read-only TLS)", ld_es))
        check_u32("mov $0x73,%es (read-only TLS)", out0, 0x73);
    run_fault("mov $0x73,%ss (read-only TLS)", ld_ss, ip_ld_ss, SIGSEGV, SI_KERNEL);
    load_sel = 0x6b;
    if (run_clean("mov $0x6b,%ss (writable TLS)", ld_ss))
        check_u32("mov $0x6b,%ss (writable TLS)", out0, 0x6b);
    load_sel = 0x68;
    run_fault("mov $0x68,%ss (RPL 0)", ld_ss, ip_ld_ss, SIGSEGV, SI_KERNEL);
    if (run_clean("mov $0x68,%es (RPL 0)", ld_es))
        check_u32("mov $0x68,%es (RPL 0)", out0, 0x68);

    // What set_thread_area refuses; none of it touches entry 13.
    d = (struct tls_desc) { .entry = 11, .base = 0, .limit = 0xfffff, .flags = DESC_DATA };
    check_errno("set_thread_area(11)", set_ta(&d), EINVAL);
    d = (struct tls_desc) { .entry = 15, .base = 0, .limit = 0xfffff, .flags = DESC_DATA };
    check_errno("set_thread_area(15)", set_ta(&d), EINVAL);
    d = (struct tls_desc) { .entry = 13, .base = 0, .limit = 0xfffff, .flags = DESC_DATA | 4 };
    check_errno("set_thread_area code segment", set_ta(&d), EINVAL);
    d = (struct tls_desc) { .entry = 13, .base = 0, .limit = 0xfffff, .flags = DESC_DATA & ~1 };
    check_errno("set_thread_area 16-bit", set_ta(&d), EINVAL);
    d = (struct tls_desc) { .entry = 13, .base = 0, .limit = 0xfffff, .flags = DESC_DATA | 0x20 };
    check_errno("set_thread_area not present", set_ta(&d), EINVAL);
    check_get("entry 13 after the refusals", 13, (uintptr_t) tbuf2, 0xfffff, DESC_DATA);

    // fork keeps FS and the entries.
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        struct tls_desc c = { .entry = 13 };
        int bad = 0;
        if (run(read_fs) != 0 || out0 != 0x6b)
            bad |= 1;
        if (run(fs_read0) != 0 || out0 != tbuf2[0])
            bad |= 2;
        if (get_ta(&c) != 0 || c.base != (uintptr_t) tbuf2)
            bad |= 4;
        _exit(bad);
    }
    int status = 0;
    checks++;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
        printf("FAIL fork: the child did not inherit fs 0x6b on entry 13 (status %#x)\n",
               status);
        failures_total++;
    }

    // exec resets all of it.
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        static char path[] = "/proc/self/exe", arg[] = "--exec-child";
        char *args[] = { path, arg, NULL };
        extern char **environ;
        long nr = SYS_execve;
        // ES 0x7b cannot be left in ES while C code runs, so the execve is
        // made from here.
        __asm__ volatile("mov %[e], %%es\n\t"
                         "int $0x80\n\t"
                         "push %%ss\n\t"
                         "pop %%es"
                         : "+a"(nr)
                         : "b"(path), "c"(args), "d"(environ),
                           [e] "S"(0x7b)
                         : "memory");
        _exit(100);
    }
    status = 0;
    checks++;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
        printf("FAIL exec: selectors and TLS entries not reset (status %#x)\n", status);
        failures_total++;
    }

    // Clearing entry 14 empties it; clearing entry 13 under FS leaves FS null.
    d = (struct tls_desc) { .entry = 14, .flags = DESC_EMPTY };
    checks++;
    if (set_ta(&d) != 0) {
        printf("FAIL clearing entry 14: %s\n", strerror(errno));
        failures_total++;
    }
    check_get("entry 14 cleared", 14, 0, 0, DESC_EMPTY);
    d = (struct tls_desc) { .entry = 13 };
    checks++;
    if (set_ta(&d) != 0) {
        printf("FAIL clearing entry 13 with zeros: %s\n", strerror(errno));
        failures_total++;
    }
    check_get("entry 13 cleared", 13, 0, 0, DESC_EMPTY);
    if (run_clean("mov %fs,%eax after clearing entry 13", read_fs))
        check_u32("fs after clearing entry 13", out0, 0);
    load_sel = 0x6b;
    run_fault("mov $0x6b,%fs after clearing entry 13", ld_fs, ip_ld_fs, SIGSEGV, SI_KERNEL);
    fs_sel = 0;
    run(load_fs);
}

// What the exec'd image sees. Its libc has loaded GS again.
static int exec_child(void) {
    uint32_t cs, ss, ds, es, fs, gs;
    __asm__ volatile("mov %%cs, %0\n\tmov %%ss, %1\n\tmov %%ds, %2\n\t"
                     "mov %%es, %3\n\tmov %%fs, %4\n\tmov %%gs, %5"
                     : "=r"(cs), "=r"(ss), "=r"(ds), "=r"(es), "=r"(fs), "=r"(gs));
    struct tls_desc d13 = { .entry = 13 }, d14 = { .entry = 14 };
    int bad = 0;
    if (cs != 0x23 || ss != 0x2b || ds != 0x2b || es != 0x2b || fs != 0 || gs != SEL_TLS)
        bad = 1;
    if (get_ta(&d13) != 0 || d13.base != 0 || (d13.flags & 0x7f) != DESC_EMPTY)
        bad = 1;
    if (get_ta(&d14) != 0 || d14.base != 0 || (d14.flags & 0x7f) != DESC_EMPTY)
        bad = 1;
    if (bad)
        printf("exec child: cs %#x ss %#x ds %#x es %#x fs %#x gs %#x; "
               "entry 13 base %#x flags %#x, entry 14 base %#x flags %#x\n",
               cs, ss, ds, es, fs, gs, d13.base, d13.flags, d14.base, d14.flags);
    return bad;
}

// ---- signal delivery and sigreturn ----
// Selectors go in and out through memory, so that ES and FS hold the test's
// values only between the loads and the reads, around a kill() made from the
// same asm. Offsets: 0 es_in, 2 fs_in, 4 es_old, 6 fs_old, 8 es_out,
// 10 fs_out, 12 ds_out.
struct sigsel {
    uint16_t es_in, fs_in, es_old, fs_old, es_out, fs_out, ds_out;
};
static struct sigsel ss_io;
static volatile uint32_t h_live[4];    // es, ds, fs, gs inside the handler
static volatile uint32_t h_greg[6];    // REG_GS, FS, ES, DS, CS, SS
static volatile int h_modify;

static void usr1_handler(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    uint32_t es, ds, fs, gs;
    (void) sig;
    (void) si;
    __asm__ volatile("mov %%es, %0\n\tmov %%ds, %1\n\tmov %%fs, %2\n\tmov %%gs, %3"
                     : "=r"(es), "=r"(ds), "=r"(fs), "=r"(gs));
    h_live[0] = es;
    h_live[1] = ds;
    h_live[2] = fs;
    h_live[3] = gs;
    h_greg[0] = (uint32_t) uc->uc_mcontext.gregs[REG_GS];
    h_greg[1] = (uint32_t) uc->uc_mcontext.gregs[REG_FS];
    h_greg[2] = (uint32_t) uc->uc_mcontext.gregs[REG_ES];
    h_greg[3] = (uint32_t) uc->uc_mcontext.gregs[REG_DS];
    h_greg[4] = (uint32_t) uc->uc_mcontext.gregs[REG_CS];
    h_greg[5] = (uint32_t) uc->uc_mcontext.gregs[REG_SS];
    switch (h_modify) {
    case 1:
        uc->uc_mcontext.gregs[REG_ES] = 0x23;
        uc->uc_mcontext.gregs[REG_FS] = 0x10;   // RPL forced to 3: 0x13, kernel
        break;
    case 2:
        uc->uc_mcontext.gregs[REG_ES] = 0x20;   // RPL forced to 3: 0x23
        uc->uc_mcontext.gregs[REG_FS] = 0x6b;   // an empty TLS entry
        break;
    case 3:
        uc->uc_mcontext.gregs[REG_CS] = 0;
        break;
    case 4:
        uc->uc_mcontext.gregs[REG_SS] = 0;
        break;
    }
}

DECLARE_IP(ip_sig_resume);
static void sig_probe(void) {
    __asm__ volatile("mov %%es, 4(%%esi)\n\t"
                     "mov %%fs, 6(%%esi)\n\t"
                     "mov 0(%%esi), %%es\n\t"
                     "mov 2(%%esi), %%fs\n\t"
                     "int $0x80\n\t"
                     LBL(ip_sig_resume)
                     "mov %%es, 8(%%esi)\n\t"
                     "mov %%fs, 10(%%esi)\n\t"
                     "mov %%ds, 12(%%esi)\n\t"
                     "mov 4(%%esi), %%es\n\t"
                     "mov 6(%%esi), %%fs"
                     : "+a"(out2)
                     : "b"(getpid()), "c"(SIGUSR1), "S"(&ss_io)
                     : "memory");
}

static void check_signal_case(const char *what, uint16_t es_in, uint16_t fs_in, int modify,
        uint16_t want_es, uint16_t want_fs) {
    char label[128];
    memset((void *) h_live, 0, sizeof h_live);
    memset((void *) h_greg, 0, sizeof h_greg);
    ss_io = (struct sigsel) { .es_in = es_in, .fs_in = fs_in };
    h_modify = modify;
    out2 = SYS_kill;
    if (!run_clean(what, sig_probe))
        return;
    snprintf(label, sizeof label, "%s: es in the handler", what);
    check_u32(label, h_live[0], 0x2b);
    snprintf(label, sizeof label, "%s: ds in the handler", what);
    check_u32(label, h_live[1], 0x2b);
    snprintf(label, sizeof label, "%s: fs in the handler", what);
    check_u32(label, h_live[2], fs_in);
    snprintf(label, sizeof label, "%s: gs in the handler", what);
    check_u32(label, h_live[3], SEL_TLS);
    snprintf(label, sizeof label, "%s: REG_GS", what);
    check_u32(label, h_greg[0], SEL_TLS);
    snprintf(label, sizeof label, "%s: REG_FS", what);
    check_u32(label, h_greg[1], fs_in);
    snprintf(label, sizeof label, "%s: REG_ES", what);
    check_u32(label, h_greg[2], es_in);
    snprintf(label, sizeof label, "%s: REG_DS", what);
    check_u32(label, h_greg[3], 0x2b);
    snprintf(label, sizeof label, "%s: REG_CS", what);
    check_u32(label, h_greg[4], 0x23);
    snprintf(label, sizeof label, "%s: REG_SS", what);
    check_u32(label, h_greg[5], 0x2b);
    snprintf(label, sizeof label, "%s: es after sigreturn", what);
    check_u32(label, ss_io.es_out, want_es);
    snprintf(label, sizeof label, "%s: fs after sigreturn", what);
    check_u32(label, ss_io.fs_out, want_fs);
    snprintf(label, sizeof label, "%s: ds after sigreturn", what);
    check_u32(label, ss_io.ds_out, 0x2b);
}

static void check_signals(void) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = usr1_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, &old);

    check_signal_case("signal, es 0x7b fs 0x33", 0x7b, 0x33, 0, 0x7b, 0x33);
    check_signal_case("signal, es 0x23 fs 0x7b", 0x23, 0x7b, 0, 0x23, 0x7b);
    check_signal_case("signal, es 0x2b fs 0", 0x2b, 0, 0, 0x2b, 0);
    check_signal_case("frame es 0x23 fs 0x10", 0x7b, 0x33, 1, 0x23, 0);
    check_signal_case("frame es 0x20 fs 0x6b (empty)", 0x7b, 0x33, 2, 0x23, 0);

    // A frame whose CS or SS cannot be returned to: SIGSEGV where it resumes.
    ss_io = (struct sigsel) { .es_in = 0x2b, .fs_in = 0 };
    h_modify = 3;
    out2 = SYS_kill;
    run_fault("frame cs 0", sig_probe, ip_sig_resume, SIGSEGV, SI_KERNEL);
    ss_io = (struct sigsel) { .es_in = 0x2b, .fs_in = 0 };
    h_modify = 4;
    out2 = SYS_kill;
    run_fault("frame ss 0", sig_probe, ip_sig_resume, SIGSEGV, SI_KERNEL);
    fs_sel = 0;
    run(load_fs);
    sigaction(SIGUSR1, &old, NULL);
}

// ---- ptrace ----
static void check_ptrace(void) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        static struct sigsel s;
        long nr = SYS_kill;
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(3);
        s = (struct sigsel) { .es_in = 0x2b, .fs_in = 0x7b };
        __asm__ volatile("mov %%es, 4(%%esi)\n\t"
                         "mov %%fs, 6(%%esi)\n\t"
                         "mov 0(%%esi), %%es\n\t"
                         "mov 2(%%esi), %%fs\n\t"
                         "int $0x80\n\t"
                         "mov %%es, 8(%%esi)\n\t"
                         "mov %%fs, 10(%%esi)\n\t"
                         "mov %%ds, 12(%%esi)\n\t"
                         "mov 4(%%esi), %%es\n\t"
                         "mov 6(%%esi), %%fs"
                         : "+a"(nr)
                         : "b"(getpid()), "c"(SIGSTOP), "S"(&s)
                         : "memory");
        // The tracer set es to 0x23 and fs to 0x2b, and filled entry 13.
        struct tls_desc c = { .entry = 13 };
        if (get_ta(&c) != 0 || c.base != 0x12345000 || (c.flags & 0x7f) != DESC_DATA)
            _exit(2);
        _exit(s.es_out == 0x23 && s.fs_out == 0x2b && s.ds_out == 0x2b ? 0 : 1);
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
    check_u32("ptrace xcs", (uint32_t) regs.xcs, 0x23);
    check_u32("ptrace xss", (uint32_t) regs.xss, 0x2b);
    check_u32("ptrace xds", (uint32_t) regs.xds, 0x2b);
    check_u32("ptrace xes", (uint32_t) regs.xes, 0x2b);
    check_u32("ptrace xfs", (uint32_t) regs.xfs, 0x7b);
    check_u32("ptrace xgs", (uint32_t) regs.xgs, SEL_TLS);
    errno = 0;
    long peek = ptrace(PTRACE_PEEKUSER, pid,
                       (void *) offsetof(struct user_regs_struct, xgs), 0);
    check_u32("ptrace PEEKUSER xgs", (uint32_t) peek, SEL_TLS);
    struct tls_desc d = { 0 };
    checks++;
    if (ptrace(PTRACE_GET_THREAD_AREA_, pid, (void *) 12, &d) != 0) {
        printf("FAIL ptrace: GET_THREAD_AREA: %s\n", strerror(errno));
        failures_total++;
    } else {
        check_desc("ptrace GET_THREAD_AREA 12", &d, 12, tls_self, 0xfffff, DESC_DATA);
    }

    d = (struct tls_desc) { .entry = 13, .base = 0x12345000, .limit = 0xfffff,
                            .flags = DESC_DATA };
    checks++;
    if (ptrace(PTRACE_SET_THREAD_AREA_, pid, (void *) 13, &d) != 0) {
        printf("FAIL ptrace: SET_THREAD_AREA: %s\n", strerror(errno));
        failures_total++;
    }
    d = (struct tls_desc) { 0 };
    checks++;
    if (ptrace(PTRACE_GET_THREAD_AREA_, pid, (void *) 13, &d) != 0) {
        printf("FAIL ptrace: GET_THREAD_AREA 13: %s\n", strerror(errno));
        failures_total++;
    } else {
        check_desc("ptrace GET_THREAD_AREA 13 after SET", &d, 13, 0x12345000, 0xfffff, DESC_DATA);
    }
    check_errno("ptrace GET_THREAD_AREA 11", ptrace(PTRACE_GET_THREAD_AREA_, pid, (void *) 11, &d),
                EINVAL);

    struct user_regs_struct bad = regs;
    bad.xfs = 0x28;
    check_errno("ptrace SETREGS xfs 0x28 (RPL 0)", ptrace(PTRACE_SETREGS, pid, 0, &bad), EIO);
    bad = regs;
    bad.xcs = 0;
    check_errno("ptrace SETREGS xcs 0", ptrace(PTRACE_SETREGS, pid, 0, &bad), EIO);
    bad = regs;
    bad.xss = 0;
    check_errno("ptrace SETREGS xss 0", ptrace(PTRACE_SETREGS, pid, 0, &bad), EIO);

    regs.xes = 0x23;
    regs.xfs = 0x2b;
    checks++;
    if (ptrace(PTRACE_SETREGS, pid, 0, &regs) != 0) {
        printf("FAIL ptrace: SETREGS: %s\n", strerror(errno));
        failures_total++;
    }
    ptrace(PTRACE_CONT, pid, 0, 0);
    checks++;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL ptrace: after SETREGS the child read other selectors (status %#x)\n",
               status);
        failures_total++;
    }
}

static int loadable(int is_ss, uint32_t sel) {
    unsigned index = (sel & 0xffff) >> 3;
    if (sel & 4)
        return 0;
    if (is_ss)
        return (sel & 0xffff) == 0x2b || (sel & 0xffff) == SEL_TLS;
    return index == 0 || index == 4 || index == 5 || index == 6 || index == 12 ||
           index == 15;
}

int main(int argc, char **argv) {
    char what[96];
    if (argc > 1 && strcmp(argv[1], "--exec-child") == 0)
        return exec_child();
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    setvbuf(stdout, NULL, _IONBF, 0);

    static char altstack[65536];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    // Positive control: the harness must see a known #UD, at its address,
    // before any absence of a signal below means anything.
    if (run(ud_control) != SIGILL || f_eip != (uint32_t) (uintptr_t) ip_ud_control) {
        printf("FAIL harness: ud2 gave %s at %#x (want SIGILL at %#x)\n", signame(f_sig),
               f_eip, (uint32_t) (uintptr_t) ip_ud_control);
        failures_total++;
        return finish_suite(suite);
    }
    unsigned char *pages = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED) {
        printf("FAIL harness: mmap: %s\n", strerror(errno));
        failures_total++;
        return finish_suite(suite);
    }
    ro_page = pages;
    none_page = pages + 4096;
    hole_page = pages + 8192;
    mprotect(ro_page, 4096, PROT_READ);
    mprotect(none_page, 4096, PROT_NONE);
    munmap(hole_page, 4096);
    if (run(gs_self) != 0 || out0 == 0) {
        printf("FAIL harness: mov %%gs:0 gave %s, %#x\n", signame(f_sig), out0);
        failures_total++;
        return finish_suite(suite);
    }
    tls_self = out0;

    // 8C to a register.
    static const struct {
        const char *what;
        void (*fn)(void);
        uint32_t want;
    } reg_reads[] = {
        {"mov %cs,%eax", rd_cs_eax, 0x23},
        {"mov %ss,%eax", rd_ss_eax, 0x2b},
        {"mov %es,%eax", rd_es_eax, 0x2b},
        {"mov %ds,%eax", rd_ds_eax, 0x2b},
        {"mov %fs,%eax", rd_fs_eax, 0},
        {"mov %gs,%eax", rd_gs_eax, SEL_TLS},
        {"mov %cs,%ax (66)", rd_cs_ax, (PATTERN & ~0xffffu) | 0x23},
        {"mov %gs,%ax (66)", rd_gs_ax, (PATTERN & ~0xffffu) | SEL_TLS},
        {"mov %cs,%ecx", rd_cs_ecx, 0x23},
        {"mov %ss,%di (66)", rd_ss_di, (PATTERN & ~0xffffu) | 0x2b},
        {"mov %cs,%eax; add; mov %ss,%ecx; add", rd_cs_then_add, 0x23 + 0x100 + 0x2b},
    };
    for (unsigned i = 0; i < sizeof(reg_reads) / sizeof(reg_reads[0]); i++)
        if (run_clean(reg_reads[i].what, reg_reads[i].fn))
            check_u32(reg_reads[i].what, out0, reg_reads[i].want);

    // 8C to memory.
    if (run_clean("mov %cs,(%edi)", mr_cs))
        check_membuf("mov %cs,(%edi)", 0, 0x23);
    if (run_clean("mov %ss,(%edi) 66", mr_ss_66))
        check_membuf("mov %ss,(%edi) 66", 0, 0x2b);
    if (run_clean("mov %gs,0x38(%edi)", mr_gs_disp8))
        check_membuf("mov %gs,0x38(%edi)", 0x38, SEL_TLS);
    if (run_clean("mov %ds,4(%edi,%ecx,4)", mr_ds_sib))
        check_membuf("mov %ds,4(%edi,%ecx,4)", 24, 0x2b);
    if (run_clean("mov %fs,0x40(%edi)", mr_fs_disp32))
        check_membuf("mov %fs,0x40(%edi)", 0x40, 0);

    // What must fault.
    run_fault("8c /6", ud_8c_reg6, ip_ud_8c_reg6, SIGILL, ILL_ILLOPN);
    run_fault("8c /7", ud_8c_reg7, ip_ud_8c_reg7, SIGILL, ILL_ILLOPN);
    run_fault("8c /6, memory operand", ud_8c_mem6, ip_ud_8c_mem6, SIGILL, ILL_ILLOPN);
    run_fault("lock 8c", ud_8c_lock, ip_ud_8c_lock, SIGILL, ILL_ILLOPN);
    run_fault("mov %eax,%cs", ud_8e_cs, ip_ud_8e_cs, SIGILL, ILL_ILLOPN);
    run_fault("8e /6", ud_8e_reg6, ip_ud_8e_reg6, SIGILL, ILL_ILLOPN);
    run_fault("8e /7", ud_8e_reg7, ip_ud_8e_reg7, SIGILL, ILL_ILLOPN);
    run_fault("lock 8e", ud_8e_lock, ip_ud_8e_lock, SIGILL, ILL_ILLOPN);
    if (run_fault("mov %cs,(NULL)", pf_8c_null, ip_pf_8c_null, SIGSEGV, SEGV_MAPERR))
        check_u32("mov %cs,(NULL): si_addr", f_addr, 0);
    if (run_fault("mov (NULL),%es", pf_8e_null, ip_pf_8e_null, SIGSEGV, SEGV_MAPERR))
        check_u32("mov (NULL),%es: si_addr", f_addr, 0);
    if (run_fault("mov %cs,(read-only)", acc_8c_ro, ip_acc_8c_ro, SIGSEGV, SEGV_ACCERR))
        check_u32("mov %cs,(read-only): si_addr", f_addr, (uintptr_t) ro_page);
    if (run_fault("mov (PROT_NONE),%es", acc_8e_none, ip_acc_8e_none, SIGSEGV, SEGV_ACCERR))
        check_u32("mov (PROT_NONE),%es: si_addr", f_addr, (uintptr_t) none_page);

    // 8E: every selector into every register.
    static const uint16_t sels[] = {
        0, 1, 2, 3, 0x08, 0x0b, 0x10, 0x13, 0x18, 0x1b, 0x20, 0x23, 0x28, 0x2b,
        0x30, 0x33, 0x38, 0x3b, 0x43, 0x4b, 0x53, 0x5b, 0x60, 0x61, 0x62, 0x63,
        0x6b, 0x73, 0x78, 0x7b, 0x80, 0x83, 0x04, 0x07, 0x2f, 0x67, 0x7f, 0xfffb,
    };
    static const struct {
        const char *name;
        void (*fn)(void);
        const char *ip;
        int is_ss;
    } regs[] = {
        {"es", ld_es, ip_ld_es, 0}, {"ds", ld_ds, ip_ld_ds, 0}, {"fs", ld_fs, ip_ld_fs, 0},
        {"gs", ld_gs, ip_ld_gs, 0}, {"ss", ld_ss, ip_ld_ss, 1},
    };
    for (unsigned r = 0; r < sizeof(regs) / sizeof(regs[0]); r++) {
        for (unsigned i = 0; i < sizeof(sels) / sizeof(sels[0]); i++) {
            load_sel = sels[i];
            snprintf(what, sizeof what, "mov $%#x,%%%s", sels[i], regs[r].name);
            if (!loadable(regs[r].is_ss, sels[i])) {
                run_fault(what, regs[r].fn, regs[r].ip, SIGSEGV, SI_KERNEL);
                continue;
            }
            if (run_clean(what, regs[r].fn))
                check_u32(what, out0, sels[i]);
        }
    }
    if (run_clean("mov (%edi),%es, two bytes", ld_es_mem))
        check_u32("mov (%edi),%es, two bytes", out0, 0x2b);
    if (run_clean("mov $0xffff0023,%es", ld_es_high))
        check_u32("mov $0xffff0023,%es", out0, 0x23);
    if (run_clean("mov %ax,%es (66)", ld_es_66))
        check_u32("mov %ax,%es (66)", out0, 0x7b);
    if (run_clean("mov %ecx,%fs; add; mov %fs,%edx; add", ld_fs_ecx_then_add))
        check_u32("mov %ecx,%fs; add; mov %fs,%edx; add", out0, 1 + 0x100 + 0x33);

    // PUSH / POP.
    static const struct {
        const char *what;
        void (*fn)(void);
        uint32_t sel;
    } pushes[] = {
        {"push %es", push_es, 0x2b}, {"push %cs", push_cs, 0x23},
        {"push %ss", push_ss, 0x2b}, {"push %ds", push_ds, 0x2b},
        {"push %fs", push_fs, 0},    {"push %gs", push_gs, SEL_TLS},
    };
    for (unsigned i = 0; i < sizeof(pushes) / sizeof(pushes[0]); i++) {
        if (!run_clean(pushes[i].what, pushes[i].fn))
            continue;
        checks++;
        if (out0 != pushes[i].sel && out0 != (0xffff0000u | pushes[i].sel)) {
            printf("FAIL %s: stored %#x, want %#x (or %#x)\n", pushes[i].what, out0,
                   pushes[i].sel, 0xffff0000u | pushes[i].sel);
            failures_total++;
        }
        snprintf(what, sizeof what, "%s: esp moves", pushes[i].what);
        check_u32(what, out1, 4);
    }
    if (run_clean("pushw %es", pushw_es)) {
        check_u32("pushw %es: two bytes", out0, 0x002bffff);
        check_u32("pushw %es: esp moves", out1, 2);
    }
    if (run_clean("pushw %gs", pushw_gs)) {
        check_u32("pushw %gs: two bytes", out0, (uint32_t) SEL_TLS << 16 | 0xffff);
        check_u32("pushw %gs: esp moves", out1, 2);
    }
    static const struct {
        const char *what;
        void (*fn)(void);
        uint32_t sel;
    } pops[] = {
        {"pop %es of 0xffff002b", pop_es, 0x2b}, {"pop %ds", pop_ds, 0x2b},
        {"pop %ss", pop_ss, 0x2b}, {"pop %fs", pop_fs, 0x7b},
        {"pop %gs", pop_gs, SEL_TLS}, {"popw %es", popw_es, 0x23},
        {"popw %fs", popw_fs, 0x33},
    };
    for (unsigned i = 0; i < sizeof(pops) / sizeof(pops[0]); i++) {
        if (!run_clean(pops[i].what, pops[i].fn))
            continue;
        check_u32(pops[i].what, out0, pops[i].sel);
        snprintf(what, sizeof what, "%s: esp back where it was", pops[i].what);
        check_u32(what, out1, 0);
    }
    if (run_fault("pop %es of 0x10", pop_es_bad, ip_pop_es_bad, SIGSEGV, SI_KERNEL))
        check_u32("esp at a faulting pop %es", f_esp, out1);
    if (run_fault("pop %ss of 0", pop_ss_null, ip_pop_ss_null, SIGSEGV, SI_KERNEL))
        check_u32("esp at a faulting pop %ss", f_esp, out1);
    if (run_fault("push %es to a read-only page", push_es_ro, ip_push_es_ro, SIGSEGV,
                  SEGV_ACCERR)) {
        check_u32("push %es to a read-only page: si_addr", f_addr,
                  (uintptr_t) ro_page + 4092);
        check_u32("push %es to a read-only page: esp", f_esp, (uintptr_t) ro_page + 4096);
    }
    if (run_fault("push %fs to an unmapped page", push_fs_hole, ip_push_fs_hole, SIGSEGV,
                  SEGV_MAPERR)) {
        check_u32("push %fs to an unmapped page: si_addr", f_addr,
                  (uintptr_t) hole_page + 4092);
    }
    if (run_fault("pop %ds from PROT_NONE", pop_ds_none, ip_pop_ds_none, SIGSEGV,
                  SEGV_ACCERR)) {
        check_u32("pop %ds from PROT_NONE: si_addr", f_addr, (uintptr_t) none_page);
        check_u32("pop %ds from PROT_NONE: esp", f_esp, (uintptr_t) none_page);
    }

    // Override prefixes. ES and SS are flat; GS's base is libc's TLS block.
    memset(membuf, 0, sizeof(membuf));
    membuf[0] = 0x44;
    membuf[1] = 0x33;
    membuf[2] = 0x22;
    membuf[3] = 0x11;
    if (run_clean("mov %es:(%edi),%eax", es_prefix_load))
        check_u32("mov %es:(%edi),%eax", out0, 0x11223344);
    if (run_clean("mov %ss:(%edi),%eax", ss_prefix_load))
        check_u32("mov %ss:(%edi),%eax", out0, 0x11223344);
    if (run_clean("mov %eax,%es:4(%edi)", es_prefix_store)) {
        uint32_t v;
        memcpy(&v, membuf + 4, 4);
        check_u32("mov %eax,%es:4(%edi)", v, 0x12345678);
    }

    check_tls();
    check_signals();
    check_ptrace();

    // Every probe above counts itself; a siglongjmp that landed in the wrong
    // place would skip some silently.
    checks++;
    if (checks != 554) {
        printf("FAIL harness: ran %u checks, want 554\n", checks);
        failures_total++;
    }
    test_logf("%u checks\n", checks);
    return finish_suite(suite);
}

#else  /* !__i386__ */

int main(int argc, char **argv) {
    test_init(argc, argv);
    printf("%s: SKIP (i386 guest only)\n", suite);
    return 0;
}

#endif
