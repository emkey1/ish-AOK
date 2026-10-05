// FEAT_MOPS on the arm64 JIT (the mops gadget, jit/guest-arm64/control.S): the CPYF/CPY memcpy and
// memmove triples and the SET memset triple, which glibc 2.41 picks for
// memcpy/memmove/memset when AT_HWCAP2 has MOPS. What must hold:
//   - HWCAP2_MOPS, ID_AA64ISAR2.MOPS and /proc/cpuinfo's "mops" agree;
//   - the bytes, across page boundaries, and nothing outside the range;
//   - CPY is a memmove in both overlap directions;
//   - the end registers: Xd and Xs advanced by the count, Xn = 0;
//   - a zero count touches nothing;
//   - a fault part way reports one of the triple's instructions, and once
//     the handler maps the page, returning restarts it and the whole copy
//     (or set) comes out right -- the progress-in-registers design --
//     including a backward CPY, and a fault on the destination side;
//   - a "negative" count does nothing; SET from XZR stores zeros;
//   - 600 random lengths, alignments and overlaps against memmove/memset.
// The instructions are .inst words, so any assembler builds this. SKIPs
// unless arm64 with HWCAP2_MOPS.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "test_common.h"

#if defined(__aarch64__)

#define HWCAP2_MOPS_BIT (1ull << 43)

// Rd = x5, Rs = x6, Rn = x7; SET's value register is x9.
#define CPYFP ".inst 0x190604e5\n"
#define CPYFM ".inst 0x194604e5\n"
#define CPYFE ".inst 0x198604e5\n"
#define CPYP ".inst 0x1d0604e5\n"
#define CPYM ".inst 0x1d4604e5\n"
#define CPYE ".inst 0x1d8604e5\n"
#define SETP ".inst 0x19c904e5\n"
#define SETM ".inst 0x19c944e5\n"
#define SETE ".inst 0x19c984e5\n"
// SET with Rs = XZR: the value 0.
#define SETPZ ".inst 0x19df04e5\n"
#define SETMZ ".inst 0x19df44e5\n"
#define SETEZ ".inst 0x19df84e5\n"

struct regs { uint64_t d, s, n; };

static struct regs run_cpyf(void *dst, const void *src, uint64_t n) {
    register uint64_t x5 __asm__("x5") = (uint64_t) dst;
    register uint64_t x6 __asm__("x6") = (uint64_t) src;
    register uint64_t x7 __asm__("x7") = n;
    __asm__ volatile(CPYFP CPYFM CPYFE : "+r"(x5), "+r"(x6), "+r"(x7) :: "memory", "cc");
    return (struct regs) { x5, x6, x7 };
}

static struct regs run_cpy(void *dst, const void *src, uint64_t n) {
    register uint64_t x5 __asm__("x5") = (uint64_t) dst;
    register uint64_t x6 __asm__("x6") = (uint64_t) src;
    register uint64_t x7 __asm__("x7") = n;
    __asm__ volatile(CPYP CPYM CPYE : "+r"(x5), "+r"(x6), "+r"(x7) :: "memory", "cc");
    return (struct regs) { x5, x6, x7 };
}

static struct regs run_set(void *dst, uint64_t n, uint64_t v) {
    register uint64_t x5 __asm__("x5") = (uint64_t) dst;
    register uint64_t x7 __asm__("x7") = n;
    register uint64_t x9 __asm__("x9") = v;
    __asm__ volatile(SETP SETM SETE : "+r"(x5), "+r"(x7) : "r"(x9) : "memory", "cc");
    return (struct regs) { x5, 0, x7 };
}

static struct regs run_setz(void *dst, uint64_t n) {
    register uint64_t x5 __asm__("x5") = (uint64_t) dst;
    register uint64_t x7 __asm__("x7") = n;
    __asm__ volatile(SETPZ SETMZ SETEZ : "+r"(x5), "+r"(x7) :: "memory", "cc");
    return (struct regs) { x5, 0, x7 };
}

static uint64_t rs = 0x2545f4914f6cdd1dull;
static uint64_t rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}

static void ck(const char *label, uint64_t got, uint64_t want) {
    if (got != want)
        failf(label, got, 0, 0, want, 0, 0);
    test_logf("  %-56s got=%#llx want=%#llx\n", label, (unsigned long long) got,
              (unsigned long long) want);
}

static void fill(uint8_t *p, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t) (i * 131 + seed);
}

// The first index where a and b differ, or n.
static size_t first_diff(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return i;
    return n;
}

static long pg;
static uint8_t *hole;           // the page the fault tests leave unmapped
static volatile uintptr_t fault_pc, fault_addr;
static volatile uint64_t fault_x7;
static volatile int faults;

// Map the hole (filled with a pattern) and return: the instruction restarts.
static void on_segv(int sig, siginfo_t *info, void *uc_) {
    (void) sig;
    ucontext_t *uc = uc_;
    fault_pc = uc->uc_mcontext.pc;
    fault_addr = (uintptr_t) info->si_addr;
    fault_x7 = uc->uc_mcontext.regs[7];
    faults++;
    if (faults > 4)
        _exit(99);
    mmap(hole, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    fill(hole, pg, 7);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(test_watchdog_secs(30));

    uint64_t hwcap2 = getauxval(AT_HWCAP2);
    if (!(hwcap2 & HWCAP2_MOPS_BIT)) {
        printf("arm64_mops: SKIP (no HWCAP2_MOPS)\n");
        return 0;
    }
    uint64_t isar2;
    __asm__ volatile("mrs %0, S3_0_C0_C6_2" : "=r"(isar2));
    ck("ID_AA64ISAR2.MOPS", (isar2 >> 16) & 0xf, 1);
    char line[512];
    int saw_mops = 0;
    FILE *f = fopen("/proc/cpuinfo", "r");
    while (f != NULL && fgets(line, sizeof(line), f) != NULL)
        if (strncmp(line, "Features", 8) == 0 && strstr(line, " mops") != NULL)
            saw_mops = 1;
    if (f != NULL)
        fclose(f);
    ck("/proc/cpuinfo Features has mops", saw_mops, 1);

    pg = sysconf(_SC_PAGESIZE);
    size_t area = 4 * pg;
    uint8_t *a = mmap(NULL, area, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *b = mmap(NULL, area, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *want = mmap(NULL, area, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    // CPYF: sizes and misalignments that cross pages.
    static const size_t sizes[] = { 1, 7, 64, 4095, 4096, 4097, 9000 };
    static const size_t offs[] = { 0, 3, 4093 };
    int bad = 0;
    for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        for (size_t oi = 0; oi < 3; oi++) {
            size_t n = sizes[si], so = offs[oi], dofs = offs[2 - oi];
            fill(a, area, 1);
            fill(b, area, 2);
            memcpy(want, b, area);
            memcpy(want + dofs, a + so, n);
            struct regs r = run_cpyf(b + dofs, a + so, n);
            if (first_diff(b, want, area) != area || r.d != (uint64_t) (b + dofs + n) ||
                    r.s != (uint64_t) (a + so + n) || r.n != 0) {
                test_logf("  cpyf n=%zu src+%zu dst+%zu wrong\n", n, so, dofs);
                bad++;
            }
        }
    }
    ck("cpyf: 21 size/alignment cases (bytes and end registers)", bad, 0);

    struct regs r = run_cpyf(b, a, 0);
    ck("cpyf n=0: Xd unchanged", r.d, (uint64_t) b);
    ck("cpyf n=0: Xn", r.n, 0);

    // CPY is a memmove, both overlap directions, across pages.
    bad = 0;
    for (int dir = 0; dir < 2; dir++) {
        for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
            size_t n = sizes[si], shift = 5 + si * 37;
            uint8_t *src = dir ? a + pg - 3 : a + pg - 3 + shift;
            uint8_t *dst = dir ? src + shift : src - shift;
            fill(a, area, 3);
            memcpy(want, a, area);
            memmove(want + (dst - a), want + (src - a), n);
            r = run_cpy(dst, src, n);
            if (first_diff(a, want, area) != area || r.d != (uint64_t) (dst + n) ||
                    r.s != (uint64_t) (src + n) || r.n != 0) {
                test_logf("  cpy dir=%d n=%zu shift=%zu wrong\n", dir, n, shift);
                bad++;
            }
        }
    }
    ck("cpy: 14 overlapping memmove cases, both directions", bad, 0);

    // Every size the JIT's in-gadget small path takes (1-64, one page each
    // side) and the first it hands to C, with overlaps both ways, for CPY,
    // CPYF (no overlap) and SET.
    bad = 0;
    for (size_t n = 1; n <= 65; n++) {
        static const int shifts[] = { 1, 2, 7, 16, 33 };
        for (int dir = 0; dir < 2; dir++) {
            for (int k = 0; k < 5; k++) {
                size_t shift = (size_t) shifts[k];
                uint8_t *src = a + 200 + (dir ? 0 : shift), *dst = dir ? src + shift : src - shift;
                fill(a, 2 * pg, 8 + (unsigned) n);
                memcpy(want, a, 2 * pg);
                memmove(want + (dst - a), want + (src - a), n);
                r = run_cpy(dst, src, n);
                if (first_diff(a, want, 2 * pg) != 2 * pg || r.d != (uint64_t) (dst + n) ||
                        r.s != (uint64_t) (src + n) || r.n != 0) {
                    test_logf("  small cpy n=%zu dir=%d shift=%zu wrong\n", n, dir, shift);
                    bad++;
                }
            }
        }
        fill(a, 2 * pg, 9);
        fill(b, 2 * pg, 10);
        memcpy(want, b, 2 * pg);
        memcpy(want + 100, a + 300, n);
        r = run_cpyf(b + 100, a + 300, n);
        if (first_diff(b, want, 2 * pg) != 2 * pg || r.d != (uint64_t) (b + 100 + n) ||
                r.s != (uint64_t) (a + 300 + n) || r.n != 0) {
            test_logf("  small cpyf n=%zu wrong\n", n);
            bad++;
        }
        memcpy(want, b, 2 * pg);
        memset(want + 50, 0xc3, n);
        r = run_set(b + 50, n, 0x1c3);
        if (first_diff(b, want, 2 * pg) != 2 * pg || r.d != (uint64_t) (b + 50 + n) || r.n != 0) {
            test_logf("  small set n=%zu wrong\n", n);
            bad++;
        }
    }
    ck("small sizes 1-65: cpy overlaps, cpyf, set", bad, 0);

    // SET: value is the low byte of Xs, across pages.
    bad = 0;
    for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        size_t n = sizes[si];
        fill(b, area, 4);
        memcpy(want, b, area);
        memset(want + 4093, 0xab, n);
        r = run_set(b + 4093, n, 0x12345678abull);
        if (first_diff(b, want, area) != area || r.d != (uint64_t) (b + 4093 + n) || r.n != 0) {
            test_logf("  set n=%zu wrong\n", n);
            bad++;
        }
    }
    ck("set: 7 sizes (bytes and end registers)", bad, 0);

    // Faults. The source's third page is unmapped; the handler maps it with
    // a known pattern and returns, and the copy must restart and finish.
    struct sigaction sa = { .sa_sigaction = on_segv, .sa_flags = SA_SIGINFO };
    sigaction(SIGSEGV, &sa, NULL);
    hole = a + 2 * pg;
    fill(a, area, 5);
    munmap(hole, pg);
    fill(b, area, 6);
    size_t n = 2 * pg + 100, start = pg / 2;
    uintptr_t pc_p, pc_e;
    faults = 0;
    {
        register uint64_t x5 __asm__("x5") = (uint64_t) b;
        register uint64_t x6 __asm__("x6") = (uint64_t) (a + start);
        register uint64_t x7 __asm__("x7") = n;
        __asm__ volatile("adr %3, 1f\n adr %4, 2f\n"
                         "1: " CPYFP CPYFM "2: " CPYFE
                         : "+r"(x5), "+r"(x6), "+r"(x7), "=&r"(pc_p), "=&r"(pc_e)
                         :: "memory", "cc");
        r = (struct regs) { x5, x6, x7 };
    }
    ck("cpyf fault: one fault", faults, 1);
    ck("cpyf fault: pc is one of the triple's",
       fault_pc >= pc_p && fault_pc <= pc_e, 1);
    ck("cpyf fault: address is the hole", fault_addr, (uintptr_t) hole);
    ck("cpyf fault: Xn is what is left from the hole", fault_x7 <= n - (size_t) (hole - (a + start)), 1);
    memcpy(want, a + start, n);   // a now has the hole's pattern mapped in
    ck("cpyf fault: restarted copy is whole", first_diff(b, want, n), n);
    ck("cpyf fault: end Xd", r.d, (uint64_t) (b + n));
    ck("cpyf fault: end Xs", r.s, (uint64_t) (a + start + n));
    ck("cpyf fault: end Xn", r.n, 0);

    hole = b + 2 * pg;
    munmap(hole, pg);
    faults = 0;
    {
        register uint64_t x5 __asm__("x5") = (uint64_t) (b + start);
        register uint64_t x7 __asm__("x7") = n;
        register uint64_t x9 __asm__("x9") = 0x5a;
        __asm__ volatile("adr %2, 1f\n adr %3, 2f\n"
                         "1: " SETP SETM "2: " SETE
                         : "+r"(x5), "+r"(x7), "=&r"(pc_p), "=&r"(pc_e)
                         : "r"(x9) : "memory", "cc");
        r = (struct regs) { x5, 0, x7 };
    }
    ck("set fault: one fault", faults, 1);
    ck("set fault: pc is one of the triple's",
       fault_pc >= pc_p && fault_pc <= pc_e, 1);
    ck("set fault: write fault address is the hole", fault_addr, (uintptr_t) hole);
    memset(want, 0x5a, n);
    ck("set fault: restarted set is whole", first_diff(b + start, want, n), n);
    ck("set fault: end Xd", r.d, (uint64_t) (b + start + n));
    ck("set fault: end Xn", r.n, 0);

    // A backward CPY (destination above an overlapping source) faulting part
    // way: what it leaves is the untouched prefix, and the restart finishes
    // it. The hole is in both ranges, so either side may fault first.
    hole = a + 2 * pg;
    munmap(hole, pg);
    fill(a, 2 * pg, 11);
    fill(a + 3 * pg, pg, 11);
    memcpy(want, a, 2 * pg);
    fill(want + 2 * pg, pg, 7);     // what the handler maps in
    memcpy(want + 3 * pg, a + 3 * pg, pg);
    {
        uint8_t *src = a + start, *dst = a + start + 100;
        memmove(want + (dst - a), want + (src - a), n);
        faults = 0;
        register uint64_t x5 __asm__("x5") = (uint64_t) dst;
        register uint64_t x6 __asm__("x6") = (uint64_t) src;
        register uint64_t x7 __asm__("x7") = n;
        __asm__ volatile(CPYP CPYM CPYE : "+r"(x5), "+r"(x6), "+r"(x7) :: "memory", "cc");
        r = (struct regs) { x5, x6, x7 };
        ck("cpy backward fault: one fault", faults, 1);
        ck("cpy backward fault: address is the hole", fault_addr >= (uintptr_t) hole &&
           fault_addr < (uintptr_t) hole + pg, 1);
        ck("cpy backward fault: restarted memmove is whole", first_diff(a, want, area), area);
        ck("cpy backward fault: end Xd", r.d, (uint64_t) (dst + n));
        ck("cpy backward fault: end Xs", r.s, (uint64_t) (src + n));
        ck("cpy backward fault: end Xn", r.n, 0);
    }

    // CPYF whose destination faults: a write fault at the hole.
    hole = b + 2 * pg;
    munmap(hole, pg);
    fill(a, area, 12);
    faults = 0;
    r = run_cpyf(b + start, a + start, n);
    ck("cpyf write fault: one fault", faults, 1);
    ck("cpyf write fault: address is the hole", fault_addr, (uintptr_t) hole);
    ck("cpyf write fault: restarted copy is whole", first_diff(b + start, a + start, n), n);
    ck("cpyf write fault: end Xn", r.n, 0);

    // A "negative" count does nothing and leaves Xn 0; SET from XZR stores 0.
    fill(b, area, 13);
    memcpy(want, b, area);
    r = run_cpyf(b + 10, a, (uint64_t) -5);
    ck("cpyf n<0: nothing written", first_diff(b, want, area), area);
    ck("cpyf n<0: Xd unchanged", r.d, (uint64_t) (b + 10));
    ck("cpyf n<0: Xn", r.n, 0);
    r = run_set(b + 10, (uint64_t) INT64_MIN, 0x77);
    ck("set n=INT64_MIN: nothing written", first_diff(b, want, area), area);
    memset(want + 3000, 0, 5000);
    r = run_setz(b + 3000, 5000);
    ck("set xzr: zeros across a page", first_diff(b, want, area), area);
    ck("set xzr: end Xd", r.d, (uint64_t) (b + 8000));

    // Random cases against memmove/memset: lengths to three pages, any
    // alignment, CPY overlapping either way, CPYF without overlap.
    bad = 0;
    for (int i = 0; i < 600; i++) {
        size_t len = (size_t) (rnd() % 4 == 0 ? rnd() % (3 * pg) : rnd() % 300);
        size_t so = (size_t) (rnd() % (area - len)), dof = (size_t) (rnd() % (area - len));
        int op = (int) (rnd() % 3);
        fill(a, area, (unsigned) i);
        memcpy(want, a, area);
        if (op == 1 && (so < dof + len && dof < so + len))
            op = 0;                  // CPYF overlapping is not a memmove
        if (op == 2) {
            uint64_t v = rnd();
            memset(want + dof, (int) (v & 0xff), len);
            r = run_set(a + dof, len, v);
            if (first_diff(a, want, area) != area || r.d != (uint64_t) (a + dof + len) || r.n != 0)
                bad++;
            continue;
        }
        memmove(want + dof, want + so, len);
        r = op ? run_cpyf(a + dof, a + so, len) : run_cpy(a + dof, a + so, len);
        if (first_diff(a, want, area) != area || r.d != (uint64_t) (a + dof + len) ||
                r.s != (uint64_t) (a + so + len) || r.n != 0) {
            test_logf("  random op=%d len=%zu src+%zu dst+%zu wrong\n", op, len, so, dof);
            bad++;
        }
    }
    ck("600 random cpy/cpyf/set cases", bad, 0);

    return finish_suite("arm64_mops");
}

#else

int main(void) {
    printf("arm64_mops: SKIP (arm64 only)\n");
    return 0;
}

#endif
