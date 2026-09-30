// Misaligned 16- and 32-bit LOCK operations on the i386 engine: every locked
// read-modify-write form, at an offset inside a 16-byte block (1, 8) and one
// straddling two (15), must produce the right memory value -- and must not
// take the host down. The JIT's fast path is an ARM exclusive pair, which
// faults the HOST on a misaligned address; before helper_atomic_unaligned
// (jit/helpers.c) the first `lock addl` here killed the whole process with a
// bus error. Then four threads increment one straddling word, which must come
// out exact. Flags of the same forms are checked by alu_flags.c; values
// verified natively on camd (gcc -m32).
#define _GNU_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static unsigned char buf[64] __attribute__((aligned(64)));
static int fails;
#define T32(name, insn, init, arg, want) do { for (int off = 1; off < 16; off += 7) { \
    uint32_t v = init, r; memcpy(buf + off, &v, 4); uint32_t a = arg; \
    __asm__ volatile(insn : "+r"(a) : "r"(buf + off) : "memory", "cc", "eax"); \
    memcpy(&r, buf + off, 4); if (r != (uint32_t)(want)) { printf("FAIL %s off=%d got %#x want %#x\n", name, off, r, (uint32_t)(want)); fails++; } } } while (0)
#define T16(name, insn, init, arg, want) do { for (int off = 1; off < 16; off += 14) { \
    uint16_t v = init, r; memcpy(buf + off, &v, 2); uint32_t a = arg; \
    __asm__ volatile(insn : "+r"(a) : "r"(buf + off) : "memory", "cc", "eax"); \
    memcpy(&r, buf + off, 2); if (r != (uint16_t)(want)) { printf("FAIL %s off=%d got %#x want %#x\n", name, off, r, (uint16_t)(want)); fails++; } } } while (0)
static sigjmp_buf jb;
static char *volatile fault_addr;
static volatile int fault_code;
static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void) sig; (void) ctx;
    fault_addr = si->si_addr;
    fault_code = si->si_code;
    siglongjmp(jb, 1);
}

static void *incr(void *p) {
    for (int i = 0; i < 50000; i++)
        __asm__ volatile("lock incl (%0)" :: "r"(p) : "memory", "cc");
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    T32("lock addl", "lock addl %0, (%1)", 0x11111111, 0x01010101, 0x12121212);
    T32("lock subl", "lock subl %0, (%1)", 0x11111111, 1, 0x11111110);
    T32("lock orl", "lock orl %0, (%1)", 0x11110000, 0x0101, 0x11110101);
    T32("lock andl", "lock andl %0, (%1)", 0xffff00ff, 0x0ff0ff0f, 0x0ff0000f);
    T32("lock xorl", "lock xorl %0, (%1)", 0xf0f0f0f0, 0xffffffff, 0x0f0f0f0f);
    T32("lock adcl", "clc\n lock adcl %0, (%1)", 5, 6, 11);
    T32("lock sbbl", "clc\n lock sbbl %0, (%1)", 9, 4, 5);
    T32("lock incl", "lock incl (%1)", 0xfffffffe, 0, 0xffffffff);
    T32("lock decl", "lock decl (%1)", 1, 0, 0);
    T32("lock notl", "lock notl (%1)", 0x12345678, 0, ~0x12345678u);
    T32("lock negl", "lock negl (%1)", 5, 0, -5);
    T32("lock xaddl", "lock xaddl %0, (%1)", 100, 23, 123);
    T32("xchgl", "xchgl %0, (%1)", 100, 23, 23);
    T32("lock cmpxchgl", "mov $100, %%eax\n lock cmpxchgl %0, (%1)", 100, 7, 7);
    T32("lock btsl", "lock btsl $3, (%1)", 0, 0, 8);
    T32("lock btrl", "lock btrl $3, (%1)", 0xff, 0, 0xf7);
    T32("lock btcl", "lock btcl $0, (%1)", 1, 0, 0);
    T16("lock addw", "lock addw %w0, (%1)", 0x1111, 0x0101, 0x1212);
    T16("lock incw", "lock incw (%1)", 0x00ff, 0, 0x0100);
    T16("lock xaddw", "lock xaddw %w0, (%1)", 100, 23, 123);
    T16("xchgw", "xchgw %w0, (%1)", 100, 23, 23);
    T16("lock cmpxchgw", "mov $100, %%eax\n lock cmpxchgw %w0, (%1)", 100, 7, 7);
    T16("lock btsw", "lock btsw $3, (%1)", 0, 0, 8);
    {
        uint32_t zero = 0, got;
        memcpy(buf + 15, &zero, 4);
        pthread_t t[4];
        for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, incr, buf + 15);
        for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
        memcpy(&got, buf + 15, 4);
        printf("4 threads x 50000 lock incl at a straddling word: %u\n", got);
        if (got != 200000) { printf("FAIL lost %u increments\n", 200000 - got); fails++; }
    }
    // A straddling operand whose second page is PROT_NONE: SIGSEGV with
    // si_addr at that page's first byte and SEGV_ACCERR, as Linux reports it
    // (camd, i386 and x86_64) -- not the operand's last byte.
    {
        char *page = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        mprotect(page + 4096, 4096, PROT_NONE);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = on_segv;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        if (sigsetjmp(jb, 1) == 0) {
            __asm__ volatile("lock incl (%0)" :: "r"(page + 4094) : "memory", "cc");
            printf("FAIL no fault from a lock incl into a PROT_NONE page\n");
            fails++;
        } else {
            printf("fault at page+%#lx code %d\n", (unsigned long) (fault_addr - page), fault_code);
            if (fault_addr != page + 4096 || fault_code != SEGV_ACCERR) {
                printf("FAIL want page+0x1000 code %d\n", SEGV_ACCERR);
                fails++;
            }
        }
    }
    printf("lock_misaligned: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
