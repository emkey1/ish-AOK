// CMPXCHG8B and CMPXCHG16B (0F C7 /1) on amd64: equal and unequal, LOCK or
// not; only ZF changes; a failed compare loads the operand (CMPXCHG8B's halves
// zero-extended into RAX/RDX), a successful one leaves RAX/RDX whole; a
// misaligned CMPXCHG8B, across a page too; CMPXCHG16B's alignment #GP
// (SIGSEGV); and four threads of increments through each, which lose nothing
// only if the swap is atomic. Answers from an AMD Ryzen (camd).
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

struct r { uint64_t rax, rdx, flags; };

// flags in: CF, SF, OF, PF and AF set (popf), so a change to any shows
static struct r cx8(void *p, uint64_t rax, uint64_t rdx, uint64_t rbx, uint64_t rcx, int lock) {
    struct r o;
    uint64_t f = 0x8d5 & ~0x40;   // OF SF AF PF CF, ZF clear
    if (lock)
        __asm__ volatile("push %[f]\n popfq\n lock cmpxchg8b (%[p])\n pushfq\n pop %[f]"
                         : "+a"(rax), "+d"(rdx), [f] "+r"(f) : [p] "r"(p), "b"(rbx), "c"(rcx) : "memory", "cc");
    else
        __asm__ volatile("push %[f]\n popfq\n cmpxchg8b (%[p])\n pushfq\n pop %[f]"
                         : "+a"(rax), "+d"(rdx), [f] "+r"(f) : [p] "r"(p), "b"(rbx), "c"(rcx) : "memory", "cc");
    o.rax = rax; o.rdx = rdx; o.flags = f & 0x8d5;
    return o;
}
static struct r cx16(void *p, uint64_t rax, uint64_t rdx, uint64_t rbx, uint64_t rcx, int lock) {
    struct r o;
    uint64_t f = 0x8d5 & ~0x40;
    if (lock)
        __asm__ volatile("push %[f]\n popfq\n lock cmpxchg16b (%[p])\n pushfq\n pop %[f]"
                         : "+a"(rax), "+d"(rdx), [f] "+r"(f) : [p] "r"(p), "b"(rbx), "c"(rcx) : "memory", "cc");
    else
        __asm__ volatile("push %[f]\n popfq\n cmpxchg16b (%[p])\n pushfq\n pop %[f]"
                         : "+a"(rax), "+d"(rdx), [f] "+r"(f) : [p] "r"(p), "b"(rbx), "c"(rcx) : "memory", "cc");
    o.rax = rax; o.rdx = rdx; o.flags = f & 0x8d5;
    return o;
}

static sigjmp_buf jb;
static volatile int got_sig;
static void on_sig(int sig) {
    got_sig = sig;
    siglongjmp(jb, 1);
}

enum { THREADS = 4, LOOPS = 20000 };
static uint64_t c8 __attribute__((aligned(16)));
static unsigned char c8m_buf[32] __attribute__((aligned(16)));
static uint64_t c16[2] __attribute__((aligned(16)));
static void *worker(void *arg) {
    (void) arg;
    for (int i = 0; i < LOOPS; i++) {
        struct r o;
        uint64_t old = c8;
        for (;;) {
            uint64_t nw = old + 0x100000001ULL;
            o = cx8(&c8, (uint32_t) old, old >> 32, (uint32_t) nw, nw >> 32, 1);
            if (o.flags & 0x40)
                break;
            old = o.rdx << 32 | (uint32_t) o.rax;
        }
        // misaligned (offset 5 of a 16-byte block: it straddles 8-byte words)
        void *m = c8m_buf + 5;
        uint64_t mo;
        memcpy(&mo, m, 8);
        for (;;) {
            o = cx8(m, (uint32_t) mo, mo >> 32, (uint32_t) (mo + 1), (mo + 1) >> 32, 1);
            if (o.flags & 0x40)
                break;
            mo = o.rdx << 32 | (uint32_t) o.rax;
        }
        uint64_t lo = c16[0], hi = c16[1];
        for (;;) {
            o = cx16(c16, lo, hi, lo + 1, hi + 3, 1);
            if (o.flags & 0x40)
                break;
            lo = o.rax;
            hi = o.rdx;
        }
    }
    return NULL;
}

int main(void) {
    static uint64_t v8 __attribute__((aligned(16)));
    static uint64_t v16[2] __attribute__((aligned(16)));
    struct r o;

    for (int lock = 0; lock < 2; lock++) {
        // CMPXCHG8B, equal: stores ecx:ebx, RAX/RDX untouched (upper halves too)
        v8 = 0x1122334455667788ULL;
        o = cx8(&v8, 0xdead000055667788ULL, 0xbeef000011223344ULL, 0x99aabbccULL, 0xddeeff00ULL, lock);
        CHECK(v8 == 0xddeeff0099aabbccULL && o.rax == 0xdead000055667788ULL && o.rdx == 0xbeef000011223344ULL &&
              o.flags == 0x8d5, "8b equal (lock %d): %llx %llx %llx %llx", lock, (unsigned long long) v8,
              (unsigned long long) o.rax, (unsigned long long) o.rdx, (unsigned long long) o.flags);
        // unequal: loads the halves zero-extended, memory unchanged, ZF clear
        v8 = 0x1122334455667788ULL;
        o = cx8(&v8, 0xdead000055667789ULL, 0xbeef000011223344ULL, 1, 2, lock);
        CHECK(v8 == 0x1122334455667788ULL && o.rax == 0x55667788ULL && o.rdx == 0x11223344ULL &&
              o.flags == 0x895, "8b unequal (lock %d): %llx %llx %llx %llx", lock, (unsigned long long) v8,
              (unsigned long long) o.rax, (unsigned long long) o.rdx, (unsigned long long) o.flags);
        // CMPXCHG16B
        v16[0] = 0x0102030405060708ULL; v16[1] = 0x1112131415161718ULL;
        o = cx16(v16, 0x0102030405060708ULL, 0x1112131415161718ULL, 0xaaaaULL, 0xbbbbULL, lock);
        CHECK(v16[0] == 0xaaaa && v16[1] == 0xbbbb && o.rax == 0x0102030405060708ULL && o.flags == 0x8d5,
              "16b equal (lock %d): %llx %llx %llx", lock, (unsigned long long) v16[0], (unsigned long long) v16[1],
              (unsigned long long) o.flags);
        o = cx16(v16, 0xaaaaULL, 0xbbbcULL, 1, 2, lock);
        CHECK(v16[0] == 0xaaaa && v16[1] == 0xbbbb && o.rax == 0xaaaa && o.rdx == 0xbbbb && o.flags == 0x895,
              "16b unequal (lock %d): %llx %llx %llx", lock, (unsigned long long) o.rax, (unsigned long long) o.rdx,
              (unsigned long long) o.flags);
    }

    // CMPXCHG8B across a page boundary, equal and unequal
    unsigned char *pg = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *x = pg + 4096 - 3;
    uint64_t xv = 0x0807060504030201ULL, xr;
    memcpy(x, &xv, 8);
    o = cx8(x, 0x04030201, 0x08070605, 0xa0b0c0d0, 0xe0f00010, 1);
    memcpy(&xr, x, 8);
    CHECK(xr == 0xe0f00010a0b0c0d0ULL && (o.flags & 0x40), "8b across a page: %llx %llx", (unsigned long long) xr,
          (unsigned long long) o.flags);
    o = cx8(x, 0, 0, 1, 1, 0);
    CHECK(o.rax == 0xa0b0c0d0ULL && o.rdx == 0xe0f00010ULL && !(o.flags & 0x40), "8b across a page, unequal: %llx %llx",
          (unsigned long long) o.rax, (unsigned long long) o.rdx);

    // CMPXCHG16B misaligned: #GP, SIGSEGV, nothing changed
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    uint64_t before[3];
    memcpy(before, pg + 8, 24);
    got_sig = 0;
    if (sigsetjmp(jb, 1) == 0)
        cx16(pg + 8, 0, 0, 1, 1, 1);
    CHECK(got_sig == SIGSEGV && memcmp(before, pg + 8, 24) == 0, "16b misaligned: signal %d", got_sig);
    got_sig = 0;
    if (sigsetjmp(jb, 1) == 0)
        cx16(pg + 1, 0, 0, 1, 1, 0);
    CHECK(got_sig == SIGSEGV, "16b misaligned by 1: signal %d", got_sig);

    // four threads
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, worker, NULL);
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);
    uint64_t m8;
    memcpy(&m8, c8m_buf + 5, 8);
    CHECK(c8 == (uint64_t) THREADS * LOOPS * 0x100000001ULL, "8b contention: %llx", (unsigned long long) c8);
    CHECK(m8 == (uint64_t) THREADS * LOOPS, "8b misaligned contention: %llx", (unsigned long long) m8);
    CHECK(c16[0] == (uint64_t) THREADS * LOOPS && c16[1] == 3ULL * THREADS * LOOPS, "16b contention: %llx %llx",
          (unsigned long long) c16[0], (unsigned long long) c16[1]);

    printf("amd64_cmpxchg16b: %s (%d checks, %d failures)\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
