// x86_smc.c -- x86 code rewritten by an ordinary store runs as rewritten.
//
// x86 has no instruction-cache maintenance: after a store to code, the next
// fetch sees the new bytes (and, across threads, the SDM 8.1.3 protocol --
// store, publish a flag, serialize -- is all the executing side needs). JITs
// patch call sites and immediates in place and rely on it.
//
// AOK drops a page's translated blocks only when a store to it MISSES the TLB.
// A store through an entry that was already writable when a block was compiled
// from the page reached nothing, so the old translation kept running:
// tests/manual/modify.c printed `after: 1 expected 2` on both x86 roots. That
// is the first case here; the rest are the shapes the fix has to cover.
//
//   static      -- code in .data, made RWX with mprotect (modify.c's case)
//   mmap        -- an RWX mapping, patched and called 256 times in a row
//   span        -- an instruction whose immediate is on the NEXT page; the
//                  block ends on a page it does not start on
//   data        -- plain data stores into a code page between calls; code
//                  that is not rewritten must keep its value
//   thread      -- another thread rewrites the code and publishes a flag;
//                  this thread waits for it, serializes, and calls
//
// Every function is `mov $imm32, %eax; ret`, the same bytes in 32- and 64-bit
// mode, and returns its immediate, so a stale translation is a wrong number.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../test_common.h"

typedef int (*fn_t)(void);

static void emit(unsigned char *p, uint32_t imm) {
    p[0] = 0xb8;
    memcpy(p + 1, &imm, 4);
    p[5] = 0xc3;
}

static void set_imm(unsigned char *p, uint32_t imm) {
    memcpy(p + 1, &imm, 4);
}

static void check(const char *label, uint32_t got, uint32_t want) {
    if (got != want)
        failf(label, got, 0, 0, want, 0, 0);
}

static void serialize(void) {
    unsigned a = 0, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d) : : "memory");
}

static unsigned char static_code[64] __attribute__((aligned(16)));

static void case_static(long pg) {
    uintptr_t start = (uintptr_t) static_code & ~(uintptr_t) (pg - 1);
    uintptr_t end = ((uintptr_t) static_code + sizeof(static_code) + pg - 1) & ~(uintptr_t) (pg - 1);
    if (mprotect((void *) start, end - start, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        perror("mprotect");
        failures_total++;
        return;
    }
    emit(static_code, 1);
    check("static before", ((fn_t) static_code)(), 1);
    static_code[1] = 2;
    check("static after", ((fn_t) static_code)(), 2);
}

static unsigned char *map_rwx(size_t len) {
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE | PROT_EXEC,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    return p;
}

static void case_mmap(long pg) {
    unsigned char *code = map_rwx(pg);
    emit(code, 0);
    unsigned wrong = 0;
    for (uint32_t i = 0; i < 256; i++) {
        set_imm(code, i * 0x01010101u);
        uint32_t got = ((fn_t) code)();
        if (got != i * 0x01010101u && wrong++ == 0)
            check("mmap loop", got, i * 0x01010101u);
    }
    test_logf("mmap: %u of 256 calls stale\n", wrong);
    munmap(code, pg);
}

static void case_span(long pg) {
    unsigned char *code = map_rwx(2 * pg);
    // b8 on the first page, the immediate's last three bytes and the ret on
    // the second.
    unsigned char *insn = code + pg - 2;
    emit(insn, 0x11223344);
    check("span before", ((fn_t) insn)(), 0x11223344);
    insn[3] = 0x77;     // byte 2 of the immediate, on the second page
    check("span after", ((fn_t) insn)(), 0x11773344);
    insn[1] = 0x55;     // byte 0, on the first page
    check("span after first page", ((fn_t) insn)(), 0x11773355);
    munmap(code, 2 * pg);
}

static void case_data(long pg) {
    unsigned char *code = map_rwx(pg);
    volatile uint32_t *counter = (volatile uint32_t *) (code + pg / 2);
    emit(code, 7);
    unsigned char *other = code + 64;
    emit(other, 9);
    uint32_t expect = 7;
    unsigned wrong = 0;
    for (uint32_t i = 0; i < 1000; i++) {
        *counter = i;
        uint32_t got = ((fn_t) code)();
        if (got != expect && wrong++ == 0)
            check("data code", got, expect);
        got = ((fn_t) other)();
        if (got != 9 && wrong++ == 0)
            check("data neighbour", got, 9);
        if (i % 100 == 99) {
            set_imm(code, i);
            expect = i;
        }
    }
    test_logf("data: %u of 2000 calls wrong\n", wrong);
    check("data counter", *counter, 999);
    munmap(code, pg);
}

static unsigned char *thread_code;
static atomic_uint published, acked;
#define THREAD_ROUNDS 2000

static void *patcher(void *arg) {
    (void) arg;
    for (unsigned k = 1; k <= THREAD_ROUNDS; k++) {
        set_imm(thread_code, k);
        atomic_store_explicit(&published, k, memory_order_release);
        while (atomic_load_explicit(&acked, memory_order_acquire) != k)
            ;
    }
    return NULL;
}

static void case_thread(long pg) {
    thread_code = map_rwx(pg);
    emit(thread_code, 0);
    check("thread before", ((fn_t) thread_code)(), 0);
    pthread_t t;
    if (pthread_create(&t, NULL, patcher, NULL) != 0) {
        perror("pthread_create");
        failures_total++;
        return;
    }
    unsigned wrong = 0;
    for (unsigned k = 1; k <= THREAD_ROUNDS; k++) {
        while (atomic_load_explicit(&published, memory_order_acquire) != k)
            ;
        serialize();
        uint32_t got = ((fn_t) thread_code)();
        if (got != k && wrong++ == 0)
            check("thread patched", got, k);
        atomic_store_explicit(&acked, k, memory_order_release);
    }
    pthread_join(t, NULL);
    test_logf("thread: %u of %u calls stale\n", wrong, THREAD_ROUNDS);
    munmap(thread_code, pg);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    long pg = sysconf(_SC_PAGESIZE);
    case_static(pg);
    case_mmap(pg);
    case_span(pg);
    case_data(pg);
    case_thread(pg);
    return finish_suite("x86_smc");
}
