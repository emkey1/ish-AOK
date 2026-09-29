// Back-to-back 64-bit loads or stores off one base run as ONE gadget on the
// riscv64 JIT (gen_riscv64_try_pair): the ldp/stp RV64 lacks. What must stay
// exactly as unfused:
//   - the values, for ld/ld, sd/sd and the fld/fsd mixes;
//   - a first load that replaces the base (not fused: its second address
//     uses the NEW base);
//   - a fault in the SECOND access reports the second instruction's pc and
//     address, with the first access already done;
//   - a fault in the first access reports the first.
// SKIPs on other architectures.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "test_common.h"

#if defined(__riscv) && __riscv_xlen == 64

static volatile uintptr_t fault_pc, fault_addr;
static volatile int faults;

// Record the fault and step over the faulting instruction (4 bytes: the
// asm below is assembled with .option norvc).
static void on_segv(int sig, siginfo_t *info, void *uc_) {
    (void) sig;
    ucontext_t *uc = uc_;
    fault_pc = uc->uc_mcontext.__gregs[0]; // REG_PC
    fault_addr = (uintptr_t) info->si_addr;
    faults++;
    uc->uc_mcontext.__gregs[0] += 4;
}

static void ck(const char *label, uint64_t got, uint64_t want) {
    if (got != want)
        failf(label, got, 0, 0, want, 0, 0);
    test_logf("  %-50s got=%#llx want=%#llx\n", label, (unsigned long long) got,
              (unsigned long long) want);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(test_watchdog_secs(30));

    uint64_t src[4] = { 0x1111, 0x2222, 0x3333, 0x4444 }, dst[4] = { 0 };
    uint64_t a, b;
    __asm__ volatile("ld %0, 0(%2)\n ld %1, 8(%2)" : "=&r"(a), "=&r"(b) : "r"(src) : "memory");
    ck("ld/ld values", a, 0x1111);
    ck("ld/ld second value", b, 0x2222);

    __asm__ volatile("sd %0, 0(%2)\n sd %1, 24(%2)" :: "r"(a), "r"(b), "r"(dst) : "memory");
    ck("sd/sd first", dst[0], 0x1111);
    ck("sd/sd second", dst[3], 0x2222);

    double d;
    __asm__ volatile("fld %0, 16(%2)\n ld %1, 24(%2)" : "=&f"(d), "=&r"(b) : "r"(src) : "memory");
    uint64_t dbits;
    memcpy(&dbits, &d, 8);
    ck("fld/ld bits", dbits, 0x3333);
    ck("fld/ld value", b, 0x4444);
    __asm__ volatile("fsd %0, 8(%1)\n sd %2, 16(%1)" :: "f"(d), "r"(dst), "r"(a) : "memory");
    ck("fsd/sd first", dst[1], 0x3333);
    ck("fsd/sd second", dst[2], 0x1111);

    // First load replaces the base: the second must use the loaded value.
    uint64_t table[2] = { 0, 0x5555 };
    uint64_t chain[2] = { (uint64_t) (uintptr_t) &table[0], 0x6666 };
    register uint64_t base __asm__("a5") = (uint64_t) (uintptr_t) chain;
    uint64_t second;
    __asm__ volatile("ld a5, 0(a5)\n ld %0, 8(a5)" : "=r"(second), "+r"(base) :: "memory");
    ck("first load replaces the base", second, 0x5555);

    // Faults. The last 8 bytes of a mapped page, then an unmapped page.
    long pg = sysconf(_SC_PAGESIZE);
    uint8_t *map = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(map + pg, pg);
    uint64_t *last = (uint64_t *) (map + pg - 8);
    *last = 0x7777;
    struct sigaction sa = { .sa_sigaction = on_segv, .sa_flags = SA_SIGINFO };
    sigaction(SIGSEGV, &sa, NULL);

    uintptr_t pc_first, pc_second;
    uint64_t x = 0, y = 0;
    faults = 0;
    __asm__ volatile(".option push\n.option norvc\n"
                     "lla %2, 1f\n lla %3, 2f\n"
                     "1: ld %0, 0(%4)\n2: ld %1, 8(%4)\n"
                     ".option pop"
                     : "+&r"(x), "+&r"(y), "=&r"(pc_first), "=&r"(pc_second)
                     : "r"(last) : "memory");
    ck("second load faulted once", faults, 1);
    ck("load fault pc is the second load's", fault_pc, pc_second);
    ck("load fault address", fault_addr, (uintptr_t) (map + pg));
    ck("the first load happened", x, 0x7777);

    uint64_t v1 = 0x8888, v2 = 0x9999;
    faults = 0;
    __asm__ volatile(".option push\n.option norvc\n"
                     "lla %0, 1f\n lla %1, 2f\n"
                     "1: sd %2, 0(%4)\n2: sd %3, 8(%4)\n"
                     ".option pop"
                     : "=&r"(pc_first), "=&r"(pc_second)
                     : "r"(v1), "r"(v2), "r"(last) : "memory");
    ck("second store faulted once", faults, 1);
    ck("store fault pc is the second store's", fault_pc, pc_second);
    ck("store fault address", fault_addr, (uintptr_t) (map + pg));
    ck("the first store happened", *last, 0x8888);

    x = 0;
    faults = 0;
    __asm__ volatile(".option push\n.option norvc\n"
                     "lla %2, 1f\n lla %3, 2f\n"
                     "1: ld %0, 8(%4)\n2: ld %1, 0(%4)\n"
                     ".option pop"
                     : "+&r"(x), "+&r"(y), "=&r"(pc_first), "=&r"(pc_second)
                     : "r"(last) : "memory");
    ck("first load faulted once", faults, 1);
    ck("first-access fault pc is the first load's", fault_pc, pc_first);
    ck("then the second load ran", y, 0x8888);

    return finish_suite("riscv64_ldst_pair");
}

#else

int main(void) {
    printf("riscv64_ldst_pair: SKIP (riscv64 only)\n");
    return 0;
}

#endif
