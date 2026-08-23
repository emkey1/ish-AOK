// dc_zva.c — DC ZVA (data cache zero by VA) on an arm64 guest.
//
// Regression test for issue #542: the JVM would not start on an aarch64
// guest. iSH-AOK used to report DCZID_EL0 with DZP=1 ("DC ZVA prohibited")
// so it would not have to emulate the instruction, and made DC ZVA itself
// raise SIGILL. No Linux kernel sets DZP for EL0, so that was a value real
// hardware never produces, and software is entitled to assume it never
// sees it: HotSpot leaves VM_Version::_zva_length at 0 when DZP is set but
// still honours UseBlockZeroing (on by default), then generates
// `and Xd, Xn, #(zva_length - 1)` -- a mask of all ones, which is not an
// encodable AArch64 logical immediate. `java -version` died inside HotSpot's
// own assembler ("Field too big for insn") before running any Java code.
//
// So the fix is to tell the truth and implement the instruction, and the
// two halves have to agree -- which is what most of this test checks.
// DCZID_EL0.BS is log2 of the block size in 4-byte words; the guest sizes
// its own loops from it (glibc's memset and HotSpot's zero_blocks stub both
// do), so a block size that does not match what DC ZVA actually zeroes is
// silent memory corruption, not a fault.
//
// Covered: DZP/BS agreement with observed behaviour, alignment-down of the
// operand, neighbouring bytes left alone, the last block of a page (the
// address that would straddle if the granule were wrong), a sweep across
// many pages so the TLB-miss path runs, a copy-on-write fault taken by the
// instruction itself, and a genuinely unmapped write still faulting.
//
// A/B: before the fix, every DC ZVA here raises SIGILL. Also passes on real
// aarch64 Linux, where all of this is plain hardware behaviour.

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define PAGE 4096

static void fail(const char *what, unsigned long long got, unsigned long long want) {
    printf("FAIL %s got=%#llx want=%#llx\n", what, got, want);
    failures_total++;
}

static inline uint64_t read_dczid(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, dczid_el0" : "=r"(v));
    return v;
}

static inline void dc_zva(void *p) {
    __asm__ volatile("dc zva, %0" :: "r"(p) : "memory");
}

// Block size the guest is TOLD to use, from DCZID_EL0.BS.
static size_t advertised_block;

// Every byte of [base, base+len) must equal want.
static int all_are(const unsigned char *base, size_t len, unsigned char want) {
    for (size_t i = 0; i < len; i++)
        if (base[i] != want)
            return 0;
    return 1;
}

static void check_dczid(void) {
    uint64_t dczid = read_dczid();
    test_logf("DCZID_EL0 = %#llx\n", (unsigned long long) dczid);

    if (dczid & (1u << 4)) {
        // DZP=1 is the state that broke the JVM. It is architecturally
        // legal, so this is a policy check, not a conformance one: nothing
        // that runs on Linux expects it.
        printf("FAIL dczid: DZP=1, DC ZVA reported as prohibited\n");
        failures_total++;
        advertised_block = 0;
        return;
    }
    advertised_block = (size_t) 4 << (dczid & 0xf);
    if (advertised_block < 16 || advertised_block > 2048 ||
        (advertised_block & (advertised_block - 1)) != 0) {
        fail("dczid block size", advertised_block, 64);
        advertised_block = 0;
        return;
    }
    // HotSpot's zero_blocks stub requires this, and every real core reports
    // 64. Not fatal on its own, but worth saying out loud.
    if (advertised_block % 16 != 0)
        fail("dczid block size not a multiple of 16", advertised_block, 64);
    test_logf("advertised block: %zu bytes\n", advertised_block);
}

// Zero one block in a 0xAA-filled buffer and check that exactly the
// naturally-aligned block containing `at` went to zero.
static void check_one(unsigned char *buf, size_t buflen, size_t at, const char *label) {
    memset(buf, 0xAA, buflen);
    dc_zva(buf + at);

    size_t start = at & ~(advertised_block - 1);
    if (!all_are(buf + start, advertised_block, 0x00)) {
        printf("FAIL %s: block at +%zu not zeroed\n", label, start);
        failures_total++;
        return;
    }
    if (start > 0 && !all_are(buf, start, 0xAA)) {
        printf("FAIL %s: bytes below +%zu were clobbered\n", label, start);
        failures_total++;
        return;
    }
    size_t after = start + advertised_block;
    if (after < buflen && !all_are(buf + after, buflen - after, 0xAA)) {
        printf("FAIL %s: bytes above +%zu were clobbered\n", label, after);
        failures_total++;
        return;
    }
    test_logf("%s: ok (zeroed [+%zu,+%zu))\n", label, start, after);
}

// The granule DC ZVA actually zeroes, measured rather than assumed: fill,
// zero at offset 0, and count the leading run of zero bytes. Must equal
// what DCZID_EL0 advertised -- a mismatch is the corruption case.
static void check_granule_matches(unsigned char *buf, size_t buflen) {
    memset(buf, 0xAA, buflen);
    dc_zva(buf);
    size_t observed = 0;
    while (observed < buflen && buf[observed] == 0x00)
        observed++;
    if (observed != advertised_block)
        fail("observed granule != DCZID_EL0.BS", observed, advertised_block);
    else
        test_logf("observed granule: %zu bytes\n", observed);
}

// Walk several pages, zeroing every block. Each new page is a TLB write
// miss the first time it is touched, which is the path that resolves a
// host pointer for the page -- a block-sized access must not be treated as
// straddling it.
#define SWEEP_PAGES 24

static void check_page_sweep(void) {
    size_t len = SWEEP_PAGES * PAGE;
    unsigned char *m = mmap(NULL, len, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) {
        printf("FAIL page sweep: mmap failed\n");
        failures_total++;
        return;
    }
    memset(m, 0xAA, len);
    for (size_t off = 0; off < len; off += advertised_block)
        dc_zva(m + off);
    if (!all_are(m, len, 0x00)) {
        printf("FAIL page sweep: %d pages not fully zeroed\n", SWEEP_PAGES);
        failures_total++;
    } else {
        test_logf("page sweep: %d pages ok\n", SWEEP_PAGES);
    }

    // Last block of a page, with the next page mapped and filled: this is
    // the highest offset a block can start at, and the one that would spill
    // into the neighbouring page if the granule were larger than advertised.
    memset(m, 0xAA, len);
    unsigned char *last = m + PAGE - advertised_block;
    dc_zva(last);
    if (!all_are(last, advertised_block, 0x00)) {
        printf("FAIL last block of page: not zeroed\n");
        failures_total++;
    } else if (!all_are(m + PAGE, advertised_block, 0xAA)) {
        printf("FAIL last block of page: spilled into the next page\n");
        failures_total++;
    } else if (!all_are(m, PAGE - advertised_block, 0xAA)) {
        printf("FAIL last block of page: clobbered earlier bytes\n");
        failures_total++;
    } else {
        test_logf("last block of page: ok\n");
    }
    munmap(m, len);
}

// DC ZVA is a write, so it takes the copy-on-write fault itself on a page
// the parent has written. The kernel resolves the fault and the instruction
// is re-executed; if the restart address were wrong the child would resume
// somewhere else entirely.
static void check_cow_fault(void) {
    unsigned char *m = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) {
        printf("FAIL cow: mmap failed\n");
        failures_total++;
        return;
    }
    memset(m, 0xAA, PAGE);

    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL cow: fork failed\n");
        failures_total++;
        munmap(m, PAGE);
        return;
    }
    if (pid == 0) {
        size_t at = advertised_block;   // second block, so offset 0 stays 0xAA
        dc_zva(m + at);
        int bad = !all_are(m + at, advertised_block, 0x00) ||
                  !all_are(m, at, 0xAA) ||
                  !all_are(m + at + advertised_block, PAGE - at - advertised_block, 0xAA);
        _exit(bad ? 1 : 0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL cow: child status %#x (signal %d)\n", status,
               WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        failures_total++;
    } else if (!all_are(m, PAGE, 0xAA)) {
        printf("FAIL cow: the child's zeroing was visible in the parent\n");
        failures_total++;
    } else {
        test_logf("copy-on-write fault: ok\n");
    }
    munmap(m, PAGE);
}

static sigjmp_buf fault_jmp;
static volatile sig_atomic_t caught;

static void on_fault(int sig) {
    caught = sig;
    siglongjmp(fault_jmp, 1);
}

// An unwritable target must still fault. Getting this wrong in the other
// direction -- a DC ZVA that silently does nothing -- would leave stale
// data where the guest believes it wrote zeroes.
static void check_faults(void) {
    struct sigaction sa, old_segv, old_bus, old_ill;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fault;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);
    sigaction(SIGILL, &sa, &old_ill);

    unsigned char *ro = mmap(NULL, PAGE, PROT_READ,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ro != MAP_FAILED) {
        caught = 0;
        if (sigsetjmp(fault_jmp, 1) == 0) {
            dc_zva(ro);
            printf("FAIL read-only page: DC ZVA did not fault\n");
            failures_total++;
        } else if (caught != SIGSEGV) {
            fail("read-only page signal", (unsigned) caught, SIGSEGV);
        } else {
            test_logf("read-only page: SIGSEGV as expected\n");
        }
        munmap(ro, PAGE);
    }

    // Xt = 31 is XZR in this encoding, not SP: DC ZVA of address 0 is a
    // well-formed instruction that faults like any other unmapped write.
    caught = 0;
    if (sigsetjmp(fault_jmp, 1) == 0) {
        __asm__ volatile("dc zva, xzr" ::: "memory");
        printf("FAIL dc zva, xzr: did not fault\n");
        failures_total++;
    } else if (caught != SIGSEGV) {
        fail("dc zva, xzr signal", (unsigned) caught, SIGSEGV);
    } else {
        test_logf("dc zva, xzr: SIGSEGV as expected\n");
    }

    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
    sigaction(SIGILL, &old_ill, NULL);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    check_dczid();
    if (advertised_block == 0) {
        // Nothing below can run without a block size, and every one of them
        // would fail for the same reason.
        return finish_suite("dc_zva");
    }

    // Aligned to the largest granule the test tolerates, so the offsets
    // below mean what they say.
    static _Alignas(2048) unsigned char buf[4096];

    check_granule_matches(buf, sizeof buf);
    check_one(buf, sizeof buf, 0, "aligned");
    check_one(buf, sizeof buf, 1, "unaligned +1");
    check_one(buf, sizeof buf, advertised_block - 1, "unaligned, last byte of block");
    check_one(buf, sizeof buf, advertised_block, "second block");
    check_one(buf, sizeof buf, 3 * advertised_block + 7, "fourth block, +7");
    check_page_sweep();
    check_cow_fault();
    check_faults();

    return finish_suite("dc_zva");
}
