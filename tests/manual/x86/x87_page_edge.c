// x87_page_edge.c -- x87 memory operands that end at, or near, the end of a
// page touch exactly their own bytes.
//
// Until the x87 gadget port (2026-10-06) an 80-bit load went through a C
// helper, fpu_ldm80(cpu, float80 *f), that copied *f whole: 16 bytes, since
// float80 is a uint64_t and a uint16_t. The read-prep gadget staged a
// cross-page copy only when the operand's 10 bytes crossed, so an FLD m80 at
// page offsets 0xff1-0xff6 read 1-6 bytes of the next host page. When that
// page was not guest memory the access faulted, the stale-TLB retry flushed
// and tried eight times, and the guest got SIGSEGV; under concurrent_exec_tlb
// that read as "exec leaves stale TLB entries" (36 runs in 40 on 2026-10-04).
// It was cc1's `fld tbyte [ebp-0x64]` at offset 0xff4. A store through the
// same kind of helper writes past its operand instead: silent corruption.
//
// Each operand here sits at every offset from where its last byte is 6 short
// of the page end to where it ends exactly there. Loads run with the next
// page PROT_NONE, and must not fault. Stores run with the next page readable
// and filled with a pattern, and must leave it, and the bytes after the
// operand on its own page, as they were. The control: an 80-bit load one byte
// further on, crossing into the PROT_NONE page, must fault.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096
#define SLACK 6          // offsets tried: end - SLACK .. end
#define FILL 0xa5

static unsigned long checks, bad;

static void check(int ok, const char *what, unsigned off) {
    checks++;
    if (!ok) {
        bad++;
        printf("FAIL %s at offset %#x\n", what, off);
    }
}

static sigjmp_buf trap;
static volatile sig_atomic_t faulted;

static void on_segv(int sig) {
    (void) sig;
    faulted = 1;
    siglongjmp(trap, 1);
}

static unsigned char *pages;    // two pages: the operand's, and the next

static void next_page(int prot) {
    if (prot != PROT_NONE) {
        mprotect(pages + PAGE, PAGE, PROT_READ | PROT_WRITE);
        memset(pages + PAGE, FILL, PAGE);
    }
    mprotect(pages + PAGE, PAGE, prot);
}

// Everything after [off, off + n) on both pages is still FILL.
static int untouched_after(unsigned off, unsigned n) {
    for (unsigned i = off + n; i < 2 * PAGE; i++)
        if (pages[i] != FILL)
            return 0;
    return 1;
}

// One x87 operation on `p`, with the FPU state around it set up and torn down
// by the caller's choice of `op`.
enum op { FLD80, FSTP80, FBLD, FBSTP, FLDENV, FNSTENV, FRSTOR, FNSAVE };

// 1.75 as an 80-bit real, and 1234567890 as packed BCD.
static const unsigned char real80[10] = {0, 0, 0, 0, 0, 0, 0, 0xe0, 0xff, 0x3f};
static const unsigned char bcd80[10] = {0x90, 0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0, 0};

static int run(enum op op, void *p, unsigned char out[108]) {
    faulted = 0;
    if (sigsetjmp(trap, 1) != 0)
        return 0;
    switch (op) {
    case FLD80:
        __asm__ volatile("fninit\n\tfldt (%0)\n\tfstpt (%1)" :: "r"(p), "r"(out) : "memory");
        break;
    case FSTP80:
        __asm__ volatile("fninit\n\tfldt (%0)\n\tfstpt (%1)" :: "r"(real80), "r"(p) : "memory");
        break;
    case FBLD:
        __asm__ volatile("fninit\n\tfbld (%0)\n\tfbstp (%1)" :: "r"(p), "r"(out) : "memory");
        break;
    case FBSTP:
        __asm__ volatile("fninit\n\tfbld (%0)\n\tfbstp (%1)" :: "r"(bcd80), "r"(p) : "memory");
        break;
    case FLDENV:
        __asm__ volatile("fninit\n\tfldenv (%0)\n\tfnstenv (%1)\n\tfninit" :: "r"(p), "r"(out) : "memory");
        break;
    case FNSTENV:
        __asm__ volatile("fninit\n\tfnstenv (%0)\n\tfninit" :: "r"(p) : "memory");
        break;
    case FRSTOR:
        __asm__ volatile("fninit\n\tfrstor (%0)\n\tfnsave (%1)" :: "r"(p), "r"(out) : "memory");
        break;
    case FNSAVE:
        __asm__ volatile("fninit\n\tfldt (%1)\n\tfnsave (%0)" :: "r"(p), "r"(real80) : "memory");
        break;
    }
    return 1;
}

struct shape {
    const char *name;
    enum op load, store;
    unsigned n;
};

// A valid image to load: what a store of the same kind writes. For the
// environment and the full state that is FNSTENV's and FNSAVE's own output,
// taken from an aligned buffer first, so the load sees bytes the hardware
// would accept.
static void image_for(const struct shape *s, unsigned char img[108]) {
    memset(img, 0, 108);
    if (s->load == FLD80) {
        memcpy(img, real80, 10);
    } else if (s->load == FBLD) {
        memcpy(img, bcd80, 10);
    } else {
        run(s->store, img, NULL);
    }
}

static void edge(const struct shape *s) {
    unsigned char img[108], out[108];
    image_for(s, img);
    char what[96];

    for (unsigned off = PAGE - s->n - SLACK; off <= PAGE - s->n; off++) {
        // Load: the next page is PROT_NONE, so a read past the operand faults.
        next_page(PROT_NONE);
        memset(pages, FILL, PAGE);
        memcpy(pages + off, img, s->n);
        memset(out, 0, sizeof(out));
        snprintf(what, sizeof(what), "%s load does not fault", s->name);
        int ran = run(s->load, pages + off, out);
        check(ran, what, off);
        if (ran && (s->load == FLD80 || s->load == FBLD)) {
            snprintf(what, sizeof(what), "%s load reads the operand's value", s->name);
            check(memcmp(out, img, 10) == 0, what, off);
        }

        // Store: the next page readable and patterned.
        next_page(PROT_READ | PROT_WRITE);
        memset(pages, FILL, PAGE);
        snprintf(what, sizeof(what), "%s store does not fault", s->name);
        ran = run(s->store, pages + off, NULL);
        check(ran, what, off);
        snprintf(what, sizeof(what), "%s store writes nothing past its %u bytes", s->name, s->n);
        check(ran && untouched_after(off, s->n), what, off);
        if (ran && (s->store == FSTP80 || s->store == FBSTP)) {
            snprintf(what, sizeof(what), "%s store writes the value", s->name);
            check(memcmp(pages + off, s->store == FSTP80 ? real80 : bcd80, 10) == 0, what, off);
        }
    }
}

// An argument names the shapes to run by prefix ("m80 real"), for a build that
// lacks one of the others' instructions.
int main(int argc, char **argv) {
    pages = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pages == MAP_FAILED) {
        printf("FAIL mmap\n");
        return 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_segv;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    static const struct shape shapes[] = {
        {"m80 real (FLD/FSTP)", FLD80, FSTP80, 10},
        {"m80 BCD (FBLD/FBSTP)", FBLD, FBSTP, 10},
        {"environment (FLDENV/FNSTENV)", FLDENV, FNSTENV, 28},
        {"state (FRSTOR/FNSAVE)", FRSTOR, FNSAVE, 108},
    };
    for (unsigned i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++)
        if (argc < 2 || strncmp(shapes[i].name, argv[1], strlen(argv[1])) == 0)
            edge(&shapes[i]);

    // Control: one byte further, the 80-bit load really crosses into the
    // PROT_NONE page, and must fault -- or the checks above prove nothing.
    unsigned char out[108];
    next_page(PROT_NONE);
    memcpy(pages + PAGE - 9, real80, 9);
    check(!run(FLD80, pages + PAGE - 9, out) && faulted,
          "control: an m80 load crossing into PROT_NONE faults", PAGE - 9);

    if (bad) {
        printf("x87_page_edge: FAIL failures=%lu (of %lu)\n", bad, checks);
        return 1;
    }
    printf("x87_page_edge: PASS (%lu checks)\n", checks);
    return 0;
}
