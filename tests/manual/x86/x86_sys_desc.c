// x86_sys_desc.c -- the system-descriptor instructions a user may run.
//
// SGDT, SIDT, SMSW, SLDT, STR: on a UMIP processor Linux traps them and
// spoofs the result (arch/x86/kernel/umip.c, 5.10 and on): a zero limit and
// a base of 0xfffffffffffe0000 (GDT) or 0xffffffffffff0000 (IDT), the low 32
// bits of it for a 32-bit task; CR0_STATE 0x80050033 for SMSW, 0 for SLDT
// (no LDT), GDT_ENTRY_TSS * 8 = 0x40 for STR -- two bytes to memory, the
// operand size's bytes memcpy'd into a register (so a 32-bit one in a 64-bit
// task keeps its upper half), and a store that faults is SIGSEGV/SEGV_MAPERR
// at the operand's first byte, the bytes before the faulting page written.
// Without UMIP the hardware runs them: the same SMSW/SLDT/STR values, a
// 32-bit register write zero-extended, and the real table registers. CPUID
// says which applies; AOK advertises UMIP, camd (a Ryzen 3500U) has none.
//
// LAR, LSL, VERR, VERW are not trapped on either: they read Linux's GDT --
// entry 4 0x23 32-bit code, 5 0x2b data, 6 0x33 64-bit code, 12-14 the TLS
// entries set_thread_area fills (fill_ldt's descriptor), 15 0x7b CPUNODE
// (read-only data whose limit is getcpu's (node << 12) | cpu); nothing else
// a user's privilege passes. ZF only; LAR and LSL write the destination only
// when they set it. LAR's 32-bit result is descriptor bits 8-23, limit 19:16
// included (the SDM leaves those undefined; camd returns them). Every selector, every operand size, register and memory
// sources. SWAPGS is #GP in 64-bit code, #UD in 32; 0F 00 /6 is #UD.
// Checked on camd, 32- and 64-bit builds.
#define _GNU_SOURCE
#include <cpuid.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__i386__)
#include <asm/ldt.h>
#endif

static int failures, checks, umip;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { if (failures++ < 40) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

#define ZF 0x40ul
#define OTHER_FLAGS 0x8d5ul                   // CF PF AF ZF SF OF
static unsigned cpunode;

// ---- the GDT model ----
struct tls_set { unsigned entry; struct { unsigned base, limit, flags; } d; };
static struct tls_set tls[3];                 // entries 12-14 as this test set them (i386)

static int model(unsigned sel, unsigned *ar, unsigned *limit) {
    if (sel & 4)
        return 0;
    switch (sel >> 3) {
    case 4: *ar = 0xcffb00; *limit = 0xffffffff; return 1;
    case 5: *ar = 0xcff300; *limit = 0xffffffff; return 1;
    case 6: *ar = 0xaffb00; *limit = 0xffffffff; return 1;
    case 15: *ar = 0x40f500 | (cpunode >> 16 & 15) << 16; *limit = cpunode; return 1;
    case 12: case 13: case 14: {
        struct tls_set *t = &tls[(sel >> 3) - 12];
        if (t->d.flags == 0)
            return 0;
        unsigned f = t->d.flags;      // user_desc: seg_32bit, contents:2, read_exec_only, limit_in_pages, seg_not_present, useable
        unsigned type = 1 | (f & 8 ? 0 : 2) | ((f >> 1) & 3) << 2;
        *ar = type << 8 | 0x7000 | (f & 0x20 ? 0 : 0x8000) | (f & 0x40 ? 0x100000 : 0) |
              (f & 1 ? 0x400000 : 0) | (f & 0x10 ? 0x800000 : 0) | (t->d.limit >> 16 & 15) << 16;
        *limit = f & 0x10 ? t->d.limit << 12 | 0xfff : t->d.limit;
        return 1;
    }
    }
    return 0;
}
static int verr_model(unsigned sel) {
    unsigned ar, lim;
    if (!model(sel, &ar, &lim))
        return 0;
    unsigned type = ar >> 8 & 15;
    return !(type & 8) || (type & 2);
}
static int verw_model(unsigned sel) {
    unsigned ar, lim;
    if (!model(sel, &ar, &lim))
        return 0;
    unsigned type = ar >> 8 & 15;
    return !(type & 8) && (type & 2);
}

#if defined(__x86_64__)
#define FL_IN "push %[fi]\n popfq\n"
#define FL_OUT "\n pushfq\n pop %[fo]"
#else
#define FL_IN "push %[fi]\n popfl\n"
#define FL_OUT "\n pushfl\n pop %[fo]"
#endif
// dest r starts as `init`; flags in fi
#define SEGOP(name, text, ...) \
static unsigned long name(unsigned short sel, unsigned long init, unsigned long fi, unsigned long *fo) { \
    unsigned long r = init, f; unsigned short m = sel; \
    __asm__ volatile(FL_IN text FL_OUT : [r] "+r"(r), [fo] "=&r"(f) : [s] "r"((unsigned long) sel), [m] "m"(m), [fi] "r"(fi) : "cc"); \
    *fo = f; return r; \
}
SEGOP(lar32, "lar %w[s], %k[r]")
SEGOP(lar16, "lar %w[s], %w[r]")
SEGOP(lsl32, "lsl %w[s], %k[r]")
SEGOP(lsl16, "lsl %w[s], %w[r]")
SEGOP(lar32m, "lar %[m], %k[r]")
SEGOP(lsl32m, "lsl %[m], %k[r]")
SEGOP(verr_r, "verr %w[s]")
SEGOP(verw_r, "verw %w[s]")
SEGOP(verr_m, "verr %[m]")
SEGOP(verw_m, "verw %[m]")
#if defined(__x86_64__)
SEGOP(lar64, "lar %w[s], %q[r]")
SEGOP(lsl64, "lsl %w[s], %q[r]")
#endif

static void check_sel(unsigned sel) {
    unsigned ar = 0, lim = 0;
    int found = model(sel, &ar, &lim);
    const unsigned long junk = (unsigned long) 0x5555555555555555ull;
    for (int z = 0; z < 2; z++) {                // flags in: ZF clear, then the rest set
        unsigned long fi = z ? (OTHER_FLAGS & ~ZF) | 2 : ZF | 2;
        unsigned long fo, r;
#define EXPECT(name, op, want_found, want) do { \
        r = op(sel, junk, fi, &fo); \
        CHECK(!!(fo & ZF) == (want_found) && (fo & OTHER_FLAGS & ~ZF) == (fi & OTHER_FLAGS & ~ZF) && \
              r == ((want_found) ? (unsigned long) (want) : junk), \
              "%s %#x: zf %d r %#lx (want zf %d r %#lx), flags %#lx from %#lx", name, sel, \
              !!(fo & ZF), r, (want_found), (want_found) ? (unsigned long) (want) : junk, fo, fi); \
    } while (0)
        EXPECT("lar32", lar32, found, ar);
        EXPECT("lar32 m16", lar32m, found, ar);
        EXPECT("lar16", lar16, found, (junk & ~0xfffful) | (ar & 0xff00));
        EXPECT("lsl32", lsl32, found, lim);
        EXPECT("lsl32 m16", lsl32m, found, lim);
        EXPECT("lsl16", lsl16, found, (junk & ~0xfffful) | (lim & 0xffff));
#if defined(__x86_64__)
        EXPECT("lar64", lar64, found, ar);
        EXPECT("lsl64", lsl64, found, lim);
#endif
        EXPECT("verr", verr_r, verr_model(sel), junk);
        EXPECT("verr m16", verr_m, verr_model(sel), junk);
        EXPECT("verw", verw_r, verw_model(sel), junk);
        EXPECT("verw m16", verw_m, verw_model(sel), junk);
    }
}

// ---- SGDT and friends ----
static sigjmp_buf jb;
static volatile int sig, code;
static void *volatile fault_addr;
static void on_sig(int s, siginfo_t *si, void *ctx) {
    (void) ctx;
    sig = s; code = si->si_code; fault_addr = si->si_addr;
    siglongjmp(jb, 1);
}

#if defined(__x86_64__)
#define BASE_BYTES 8
#else
#define BASE_BYTES 4
#endif

static void check_table(const char *name, int idt, unsigned char *at) {
    unsigned char before[16];
    memset(at, 0xaa, 16);
    memcpy(before, at, 16);
    if (idt)
        __asm__ volatile("sidt %0" : "=m"(*(char (*)[10]) at));
    else
        __asm__ volatile("sgdt %0" : "=m"(*(char (*)[10]) at));
    unsigned n = 2 + BASE_BYTES;
    CHECK(memcmp(at + n, before + n, 16 - n) == 0, "%s wrote past its %u bytes", name, n);
    if (umip) {
        unsigned long long base = idt ? 0xffffffffffff0000ull : 0xfffffffffffe0000ull;
        unsigned char want[10] = {0, 0};
        memcpy(want + 2, &base, BASE_BYTES);
        CHECK(memcmp(at, want, n) == 0, "%s: %02x %02x %02x %02x %02x %02x (spoofed: limit 0, base %#llx)",
              name, at[0], at[1], at[2], at[3], at[4], at[5], base);
    } else {
        unsigned limit = at[0] | at[1] << 8;
        CHECK(limit == (idt ? 0xfff : 0x7f), "%s limit %#x", name, limit);
    }
}

#define WORD_STORE(name, text, value) do { \
    unsigned char b[4] = {0xaa, 0xaa, 0xaa, 0xaa}; \
    __asm__ volatile(text " %0" : "=m"(*(unsigned short *) b)); \
    CHECK(b[0] == ((value) & 0xff) && b[1] == ((value) >> 8 & 0xff) && b[2] == 0xaa, \
          "%s m16: %02x %02x %02x", name, b[0], b[1], b[2]); \
} while (0)

static void check_regs(void) {
    const unsigned long junk = (unsigned long) 0x1111111111111111ull;
    unsigned long r;
#define REG_STORE(name, text, value) do { \
    r = junk; __asm__ volatile(text " %w0" : "+r"(r)); \
    CHECK(r == ((junk & ~0xfffful) | ((value) & 0xffff)), "%s r16: %#lx", name, r); \
    r = junk; __asm__ volatile(text " %k0" : "+r"(r)); \
    CHECK(r == (sizeof(long) == 8 && umip ? (junk & ~0xfffffffful) | (value) : (unsigned long) (value)), \
          "%s r32: %#lx", name, r); \
} while (0)
    REG_STORE("smsw", "smsw", 0x80050033u);
    REG_STORE("sldt", "sldt", 0u);
    REG_STORE("str", "str", 0x40u);
#if defined(__x86_64__)
    r = junk; __asm__ volatile("smsw %q0" : "+r"(r));
    CHECK(r == 0x80050033ul, "smsw r64: %#lx", r);
    r = junk; __asm__ volatile(".byte 0x48, 0x0f, 0x00, 0xc8" : "+a"(r));    // str %rax (gas drops the REX.W)
    CHECK(r == 0x40ul, "str r64: %#lx", r);
    r = junk; __asm__ volatile(".byte 0x48, 0x0f, 0x00, 0xc0" : "+a"(r));    // sldt %rax
    CHECK(r == 0, "sldt r64: %#lx", r);
#endif
    WORD_STORE("smsw", "smsw", 0x0033);
    WORD_STORE("sldt", "sldt", 0);
    WORD_STORE("str", "str", 0x40);
}

// A store that faults: onto PROT_NONE, onto PROT_READ, straddling into PROT_NONE.
static void check_faults(void) {
    long page = sysconf(_SC_PAGESIZE);
    unsigned char *p = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(p + page, page, PROT_NONE);
    mprotect(p + 2 * page, page, PROT_READ);
    struct { const char *what; unsigned char *at; } cases[] = {
        {"onto PROT_NONE", p + page},
        {"onto PROT_READ", p + 2 * page + 8},
        {"straddling into PROT_NONE", p + page - 4},
    };
    for (unsigned i = 0; i < 3; i++) {
        unsigned char *at = cases[i].at;
        if (i == 2)
            memset(p + page - 16, 0xaa, 16);
        sig = 0;
        if (sigsetjmp(jb, 1) == 0)
            __asm__ volatile("sgdt %0" : "=m"(*(char (*)[10]) at));
        CHECK(sig == SIGSEGV, "sgdt %s: signal %d", cases[i].what, sig);
        if (umip) {
            CHECK(code == SEGV_MAPERR && fault_addr == at, "sgdt %s: code %d at %p (want SEGV_MAPERR at %p)",
                  cases[i].what, code, fault_addr, (void *) at);
            if (i == 2)                           // the four bytes on the first page are written
                CHECK(at[0] == 0 && at[1] == 0 && at[2] == 0 && at[3] == 0, "sgdt %s: first page %02x %02x %02x %02x",
                      cases[i].what, at[0], at[1], at[2], at[3]);
        }
        sig = 0;
        if (sigsetjmp(jb, 1) == 0)
            __asm__ volatile("smsw %0" : "=m"(*(unsigned short *) (p + page)));
        CHECK(sig == SIGSEGV && (!umip || (code == SEGV_MAPERR && fault_addr == p + page)),
              "smsw onto PROT_NONE: signal %d code %d at %p", sig, code, fault_addr);
    }
    munmap(p, 3 * page);
}

static void check_swapgs(void) {
    sig = 0; code = 0;
    if (sigsetjmp(jb, 1) == 0)
        __asm__ volatile(".byte 0x0f, 0x01, 0xf8");
#if defined(__x86_64__)
    CHECK(sig == SIGSEGV && code == 0x80, "swapgs: signal %d code %#x (want SIGSEGV SI_KERNEL)", sig, code);
#else
    CHECK(sig == SIGILL, "swapgs: signal %d (want SIGILL)", sig);
#endif
    sig = 0;
    if (sigsetjmp(jb, 1) == 0)
        __asm__ volatile(".byte 0x0f, 0x00, 0xf0");        // 0F 00 /6
    CHECK(sig == SIGILL, "0f 00 /6: signal %d (want SIGILL)", sig);
}

#if defined(__i386__)
static int set_tls(unsigned *entry, unsigned base, unsigned limit, unsigned flags) {
    struct user_desc d = {0};
    d.entry_number = *entry;
    d.base_addr = base;
    d.limit = limit;
    d.seg_32bit = flags & 1;
    d.contents = flags >> 1 & 3;
    d.read_exec_only = flags >> 3 & 1;
    d.limit_in_pages = flags >> 4 & 1;
    d.seg_not_present = flags >> 5 & 1;
    d.useable = flags >> 6 & 1;
    if (syscall(SYS_set_thread_area, &d) != 0)
        return -1;
    *entry = d.entry_number;
    return 0;
}
#endif

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    unsigned a, b, c, d;
    __cpuid_count(7, 0, a, b, c, d);
    umip = (c >> 2) & 1;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    sched_setaffinity(0, sizeof set, &set);
    unsigned cpu = 0, node = 0;
    syscall(SYS_getcpu, &cpu, &node, NULL);
    cpunode = node << 12 | cpu;

    struct sigaction sa = {0};
    sa.sa_sigaction = on_sig;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);

#if defined(__i386__)
    // what libc already put in the TLS entries (its own, in 12)
    for (unsigned e = 12; e <= 14; e++) {
        struct user_desc u = {0};
        u.entry_number = e;
        if (syscall(SYS_get_thread_area, &u) != 0 || (u.seg_not_present && u.read_exec_only && !u.base_addr && !u.limit))
            continue;
        tls[e - 12].entry = e;
        tls[e - 12].d.limit = u.limit;
        tls[e - 12].d.flags = u.seg_32bit | u.contents << 1 | u.read_exec_only << 3 | u.limit_in_pages << 4 |
                              u.seg_not_present << 5 | u.useable << 6;
    }
#endif
    for (unsigned sel = 0; sel < 0x100; sel++)
        check_sel(sel);
    check_sel(0xfffb);
#if defined(__i386__)
    // TLS entries: the free ones, with each kind of descriptor set_thread_area takes
    static const struct { unsigned limit, flags; } kinds[] = {
        {0xfffff, 0x51},                          // 32-bit data, pages, useable
        {0x1234, 0x01},                           // byte-granular data
        {0xabcde, 0x09},                          // read-only
        {0x1000, 0x03},                           // expand-down
        {0xfedcb, 0x59},                          // read-only, pages, useable
    };
    unsigned entry = (unsigned) -1;
    unsigned code_entry = (unsigned) -1;           // code: tls_desc_okay takes data only
    CHECK(set_tls(&code_entry, 0, 0x2000, 0x15) != 0, "set_thread_area took a code segment");
    for (unsigned k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        if (set_tls(&entry, 0x10000 * k, kinds[k].limit, kinds[k].flags) != 0) {
            CHECK(0, "set_thread_area kind %u failed", k);
            continue;
        }
        tls[entry - 12].entry = entry;
        tls[entry - 12].d.limit = kinds[k].limit;
        tls[entry - 12].d.flags = kinds[k].flags;
        for (unsigned rpl = 0; rpl < 4; rpl++)
            check_sel(entry << 3 | rpl);
    }
#endif
    check_table("sgdt", 0, (unsigned char[16]) {0});
    check_table("sidt", 1, (unsigned char[16]) {0});
    check_regs();
    check_faults();
    check_swapgs();
    printf("x86_sys_desc: %s (%d checks, %d failures; %s)\n", failures ? "FAIL" : "PASS", checks, failures,
           umip ? "UMIP" : "no UMIP");
    return failures != 0;
}
