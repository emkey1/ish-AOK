// fpu_state_span -- the x87/SSE state-area instructions must validate EVERY
// byte of their memory operand, not just the first four.
//
// FXSAVE moves 512 bytes, FNSAVE/FRSTOR 108, FNSTENV/FLDENV 28. The i386 JIT
// reaches all six through the generic memory-helper gadget, which runs a TLB
// check of a width the frontend chooses and then hands the helper a host
// pointer. jit/gen.c used to choose that width from the helper's NAME suffix
// -- fpu_save32, fpu_fxsave32 -- which is the x87 mode, not the operand size.
// Every one of them therefore got a FOUR BYTE check in front of a much wider
// access, and four bytes is a check that always passes when the wide access
// would not:
//
//   * a 512-byte FXSAVE at page offset 0xf00 passed the check, got a pointer
//     valid for the 256 bytes left in that page, and wrote 512 -- straight
//     through the second page's permissions (no P_WRITE check), through its
//     copy-on-write sharing (no break, so a forked child wrote into the
//     parent's page), and off the end of the host region entirely when the
//     guest page was the last one mapped;
//   * where the four-byte check DID say "page-straddling", the gadget staged
//     four bytes into jit_frame's crosspage buffer, let the helper write 108
//     or 512 bytes into a 32-byte buffer (over last_block and chain_budget),
//     and then flushed four of them back to the guest.
//
// The load forms had a second problem on top of that width: FLDENV and FRSTOR
// were emitted as WRITES, so they demanded write permission on memory they
// only read, and wrote the staging buffer back over their own source.
//
// The oracle for all of this is the amd64 guest, which is an interpreter and
// goes through tlb_read/tlb_write with the true size (emu/amd64_interp.c), so
// this test is written to run on both x86 guests and to hold them to the same
// answer.
//
// Memory operands go in through a "D" constraint with the opcode as .byte,
// which sidesteps the (%edi) vs (%rdi) spelling difference and keeps one
// source valid for both guest ABIs -- the same trick x86/cpuid_xsave.c and
// x86/atomic_cmpxchg8b.c use. ModRM is mod=00 rm=111 ([e/rdi]) plus the
// opcode's /digit: FXSAVE /0 = 0x07, FXRSTOR /1 = 0x0f, FRSTOR /4 = 0x27,
// FNSAVE /6 = 0x37, FLDENV /4 = 0x27, FNSTENV /6 = 0x37.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "../test_common.h"

#if !defined(__i386__) && !defined(__x86_64__)

int main(int argc, char **argv) {
    (void) argc; (void) argv;
    printf("fpu_state_span: SKIP (x86 guests only)\n");
    return 0;
}

#else

#define PAGE 4096u
#define FILL 0x11

#define FXSAVE_AT(p)  __asm__ volatile(".byte 0x0f, 0xae, 0x07" :: "D"(p) : "memory")
#define FXRSTOR_AT(p) __asm__ volatile(".byte 0x0f, 0xae, 0x0f" :: "D"(p) : "memory")
#define FNSAVE_AT(p)  __asm__ volatile(".byte 0xdd, 0x37" :: "D"(p) : "memory")
#define FRSTOR_AT(p)  __asm__ volatile(".byte 0xdd, 0x27" :: "D"(p) : "memory")
#define FNSTENV_AT(p) __asm__ volatile(".byte 0xd9, 0x37" :: "D"(p) : "memory")
#define FLDENV_AT(p)  __asm__ volatile(".byte 0xd9, 0x27" :: "D"(p) : "memory")

#define FXSAVE_SIZE 512
#define FNSAVE_SIZE 108
#define FSTENV_SIZE 28

// aligned(512) on a 512-byte object, and aligned(128) on the smaller ones,
// guarantees the reference areas do not straddle a page themselves -- which
// is the whole thing being tested, so a reference that straddled would be
// comparing the bug against itself.
static unsigned char ref512[FXSAVE_SIZE] __attribute__((aligned(512)));
static unsigned char ref108[128] __attribute__((aligned(128)));
static unsigned char ref28[128] __attribute__((aligned(128)));
static unsigned char scratch108[128] __attribute__((aligned(128)));
static unsigned char scratch28[128] __attribute__((aligned(128)));

// ---------------------------------------------------------------- fault guard

static sigjmp_buf fault_jmp;
static volatile sig_atomic_t fault_armed;

static void fault_handler(int sig) {
    (void) sig;
    if (!fault_armed)
        _exit(97);
    fault_armed = 0;
    siglongjmp(fault_jmp, 1);
}

// Returns 1 if the call raised SIGSEGV/SIGBUS, 0 if it completed. The action
// goes in as a function pointer so nothing the checks care about has to live
// in a local across sigsetjmp, where a register-allocated local would be
// indeterminate after the longjmp.
static int faults(void (*fn)(void *), void *arg) {
    if (sigsetjmp(fault_jmp, 1) != 0)
        return 1;
    fault_armed = 1;
    fn(arg);
    fault_armed = 0;
    return 0;
}

static void act_fxsave(void *p) { FXSAVE_AT(p); }
static void act_fxrstor(void *p) { FXRSTOR_AT(p); }
static void act_frstor(void *p) { FRSTOR_AT(p); }
static void act_fldenv(void *p) { FLDENV_AT(p); }

// ---------------------------------------------------------------- utilities

static unsigned char *map_filled(size_t pages) {
    unsigned char *p = mmap(NULL, pages * PAGE, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL: mmap of %zu pages: %s\n", pages, strerror(errno));
        failures_total++;
        return NULL;
    }
    memset(p, FILL, pages * PAGE);
    return p;
}

static unsigned count_written(const unsigned char *p, size_t n) {
    unsigned changed = 0;
    for (size_t i = 0; i < n; i++)
        if (p[i] != FILL)
            changed++;
    return changed;
}

// Put the x87 stack into a state that is distinctive but reproducible, so a
// save/restore round trip has something to be wrong about.
static void seed_fpu(void) {
    __asm__ volatile("fninit\n\tfld1\n\tfldpi\n\tfldl2e" ::: "memory");
}

// Bytes 12..27 of the environment are the x87 instruction and data pointers.
// emu/fpu.c models them as zero for both guests ("hope nobody looks at
// these"), which is what makes a byte-for-byte comparison of two saves of the
// same state valid here. If they are ever modelled for real, compare only the
// control/status/tag words and the register file instead.
static void report_diff(const char *what, const unsigned char *a,
                        const unsigned char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            printf("FAIL: %s differs at byte %zu of %zu (%02x vs %02x)\n",
                   what, i, n, a[i], b[i]);
            failures_total++;
            return;
        }
    }
}

// ---------------------------------------------------------------- the checks

// A 512-byte FXSAVE at +0xf00 puts 256 bytes into a second page the guest has
// made read-only. It must fault, and the read-only page must be untouched.
static void check_fxsave_into_readonly_tail(void) {
    unsigned char *p = map_filled(2);
    if (p == NULL)
        return;
    if (mprotect(p + PAGE, PAGE, PROT_READ) != 0) {
        printf("FAIL: mprotect: %s\n", strerror(errno));
        failures_total++;
        munmap(p, 2 * PAGE);
        return;
    }
    seed_fpu();
    int faulted = faults(act_fxsave, p + 0xf00);
    unsigned leaked = count_written(p + PAGE, 256);

    if (!faulted) {
        printf("FAIL: fxsave straddling into a PROT_READ page did not fault\n");
        failures_total++;
    }
    if (leaked != 0) {
        printf("FAIL: fxsave wrote %u bytes into a PROT_READ page\n", leaked);
        failures_total++;
    }
    // Not asserted: whether the writable first page was partially written.
    // x86 permits a partial commit when a later part of the operand faults,
    // and both AOK engines happen to resolve every page before copying.
    test_logf("fxsave_readonly_tail: faulted=%d leaked=%u first_page_written=%u\n",
              faulted, leaked, count_written(p + 0xf00, 256));
    munmap(p, 2 * PAGE);
}

// A one-page mmap says nothing about what follows it -- the guest's allocator
// is free to hand out the next page to the next caller, and does. Take two and
// give the second one back, so the hole after the page under test is real.
static unsigned char *map_one_page_then_a_hole(void) {
    unsigned char *p = map_filled(2);
    if (p == NULL)
        return NULL;
    if (munmap(p + PAGE, PAGE) != 0) {
        printf("FAIL: munmap: %s\n", strerror(errno));
        failures_total++;
        munmap(p, 2 * PAGE);
        return NULL;
    }
    return p;
}

// The same 512 bytes, but the tail has no mapping at all -- it runs off the
// end of the last mapped guest page, which is where the unchecked version
// could take the HOST down with it.
static void check_fxsave_off_the_end(void) {
    unsigned char *p = map_one_page_then_a_hole();
    if (p == NULL)
        return;
    seed_fpu();
    int faulted = faults(act_fxsave, p + 0xf00);
    if (!faulted) {
        printf("FAIL: fxsave off the end of a one-page mapping did not fault\n");
        failures_total++;
    }
    test_logf("fxsave_off_the_end: faulted=%d\n", faulted);
    munmap(p, PAGE);
}

// FXRSTOR reads 512 bytes and has the same span to validate.
static void check_fxrstor_off_the_end(void) {
    unsigned char *p = map_one_page_then_a_hole();
    if (p == NULL)
        return;
    seed_fpu();
    FXSAVE_AT(ref512);
    memcpy(p + 0xf00, ref512, 256);
    int faulted = faults(act_fxrstor, p + 0xf00);
    __asm__ volatile("fninit" ::: "memory");
    if (!faulted) {
        printf("FAIL: fxrstor off the end of a one-page mapping did not fault\n");
        failures_total++;
    }
    test_logf("fxrstor_off_the_end: faulted=%d\n", faulted);
    munmap(p, PAGE);
}

// 108 bytes at +0xffe: the old four-byte check called this page-straddling,
// staged four bytes, and let the helper write the other 104 into the 32-byte
// jit_frame buffer -- over last_block and chain_budget. Both pages are
// writable here, so the correct answer is "no fault, all 108 bytes land, and
// they are the same 108 bytes a non-straddling save produces".
static void check_fnsave_crosspage(void) {
    unsigned char *p = map_filled(2);
    if (p == NULL)
        return;
    unsigned char *dst = p + 0xffe;

    seed_fpu();
    FNSAVE_AT(ref108);          // FNSAVE also reinitializes, so restore...
    FRSTOR_AT(ref108);          // ...to save the identical state again.
    FNSAVE_AT(dst);

    unsigned written = count_written(dst, FNSAVE_SIZE);
    if (written == 0) {
        printf("FAIL: crosspage fnsave wrote nothing\n");
        failures_total++;
    }
    report_diff("crosspage fnsave", ref108, dst, FNSAVE_SIZE);
    // The bytes past the operand must be untouched.
    if (count_written(dst + FNSAVE_SIZE, 20) != 0) {
        printf("FAIL: crosspage fnsave wrote past its 108-byte operand\n");
        failures_total++;
    }
    test_logf("fnsave_crosspage: bytes_differing_from_fill=%u of 108\n", written);
    munmap(p, 2 * PAGE);
}

// The read side of the same straddle. Before the fix FRSTOR was emitted as a
// write of four bytes, so it restored 104 bytes of jit_frame internals as FPU
// state and then wrote four bytes of the staging buffer back over its source.
static void check_frstor_crosspage(void) {
    unsigned char *p = map_filled(2);
    if (p == NULL)
        return;
    unsigned char *src = p + 0xffe;

    seed_fpu();
    FNSAVE_AT(ref108);
    FRSTOR_AT(ref108);
    memcpy(src, ref108, FNSAVE_SIZE);

    __asm__ volatile("fninit" ::: "memory");
    int faulted = faults(act_frstor, src);
    if (faulted) {
        printf("FAIL: crosspage frstor from writable memory faulted\n");
        failures_total++;
        munmap(p, 2 * PAGE);
        return;
    }
    memset(scratch108, 0, sizeof scratch108);
    FNSAVE_AT(scratch108);
    report_diff("crosspage frstor round trip", ref108, scratch108, FNSAVE_SIZE);
    munmap(p, 2 * PAGE);
}

// FRSTOR and FLDENV only read. Asking for write permission made them fault on
// a read-only mapping that x86 is perfectly happy to load from.
static void check_load_forms_from_readonly(void) {
    unsigned char *p = map_filled(1);
    if (p == NULL)
        return;

    seed_fpu();
    FNSAVE_AT(ref108);
    FRSTOR_AT(ref108);
    FNSTENV_AT(ref28);
    memcpy(p, ref108, FNSAVE_SIZE);
    memcpy(p + 256, ref28, FSTENV_SIZE);

    if (mprotect(p, PAGE, PROT_READ) != 0) {
        printf("FAIL: mprotect: %s\n", strerror(errno));
        failures_total++;
        munmap(p, PAGE);
        return;
    }

    __asm__ volatile("fninit" ::: "memory");
    if (faults(act_frstor, p)) {
        printf("FAIL: frstor from a PROT_READ page faulted\n");
        failures_total++;
    }
    if (faults(act_fldenv, p + 256)) {
        printf("FAIL: fldenv from a PROT_READ page faulted\n");
        failures_total++;
    }
    __asm__ volatile("fninit" ::: "memory");
    munmap(p, PAGE);
}

// 28 bytes at +0xff0 straddles by 12. Same staging-buffer truncation as
// fnsave, at the other width.
static void check_fnstenv_crosspage(void) {
    unsigned char *p = map_filled(2);
    if (p == NULL)
        return;
    unsigned char *dst = p + 0xff0;

    seed_fpu();
    // FNSTENV masks the exceptions it reports as a side effect on real x86,
    // so take one throwaway store first and compare the two that follow it.
    FNSTENV_AT(scratch28);
    FNSTENV_AT(ref28);
    FNSTENV_AT(dst);

    report_diff("crosspage fnstenv", ref28, dst, FSTENV_SIZE);
    if (count_written(dst + FSTENV_SIZE, 20) != 0) {
        printf("FAIL: crosspage fnstenv wrote past its 28-byte operand\n");
        failures_total++;
    }
    test_logf("fnstenv_crosspage: bytes_differing_from_fill=%u of 28\n",
              count_written(dst, FSTENV_SIZE));
    munmap(p, 2 * PAGE);
}

// The copy-on-write consequence, which is the one that corrupts a bystander.
// After fork both pages are shared. A straddling FXSAVE in the child breaks
// COW on the first page and must break it on the second as well; the version
// that checked four bytes broke only the first and ran the remaining 256
// bytes off the end of the child's fresh private page. So: the child must be
// able to read its own 512 bytes back, and the parent's copy of the second
// page must be untouched.
static void check_fxsave_cow_after_fork(void) {
    unsigned char *p = map_filled(2);
    if (p == NULL)
        return;

    seed_fpu();

    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL: fork: %s\n", strerror(errno));
        failures_total++;
        munmap(p, 2 * PAGE);
        return;
    }
    if (pid == 0) {
        // Both saves back to back, with nothing in between that could touch
        // an XMM register or MXCSR -- FXSAVE does not disturb the state it
        // reports, so the straddling area and the aligned one must agree byte
        // for byte. (A reference taken before the fork would NOT: fflush and
        // fork are entitled to use XMM, and the amd64 guest's area carries
        // all sixteen of them.)
        FXSAVE_AT(p + 0xf00);
        FXSAVE_AT(ref512);
        _exit(memcmp(p + 0xf00, ref512, FXSAVE_SIZE) == 0 ? 0 : 1);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        printf("FAIL: waitpid: %s\n", strerror(errno));
        failures_total++;
        munmap(p, 2 * PAGE);
        return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL: child's straddling fxsave did not land all 512 bytes "
               "(status %d)\n", status);
        failures_total++;
    }
    unsigned parent_changed = count_written(p + PAGE, 256);
    if (parent_changed != 0) {
        printf("FAIL: child's fxsave wrote %u bytes through COW into the "
               "parent's page\n", parent_changed);
        failures_total++;
    }
    munmap(p, 2 * PAGE);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    if (sysconf(_SC_PAGESIZE) != (long) PAGE) {
        printf("fpu_state_span: SKIP (page size is %ld, this test assumes %u)\n",
               sysconf(_SC_PAGESIZE), PAGE);
        return 0;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fault_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    check_fxsave_into_readonly_tail();
    check_fxsave_off_the_end();
    check_fxrstor_off_the_end();
    check_fnsave_crosspage();
    check_frstor_crosspage();
    check_load_forms_from_readonly();
    check_fnstenv_crosspage();
    check_fxsave_cow_after_fork();

    return finish_suite("fpu_state_span");
}

#endif
