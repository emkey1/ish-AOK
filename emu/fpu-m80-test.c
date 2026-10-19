// Unit test for the x87 80-bit memory helpers in emu/fpu.c. Runs on the host:
// `meson test -C build fpu_m80`.
//
// fpu_ldm80 and fpu_stm80 are handed a host pointer straight into guest
// memory, at the operand's 10 bytes. They must touch those 10 and nothing
// else: fpu_ldm80 used to copy its float80 whole, 16 bytes, and an operand in
// the last 16 bytes of a page read into the next host page -- a host fault
// when that page was unmapped. A guest program cannot arrange that (the host
// page after a guest page's backing is not the guest's to choose), so the
// helpers are called here directly, against a host page whose successor is
// PROT_NONE. An x86_64-host build runs every x87 m80 load through fpu_ldm80;
// an aarch64 host's JIT has its own gadgets, and its amd64 interpreter copies
// the operand out first.
#include <signal.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "emu/cpu.h"
#include "emu/fpu.h"

static int failures;

static void check(bool ok, const char *what, size_t off) {
    if (!ok) {
        printf("FAIL %s at offset %#zx\n", what, off);
        failures++;
    }
}

static sigjmp_buf trap;
static void on_fault(int sig) {
    (void) sig;
    siglongjmp(trap, 1);
}

// 1.75 as an 80-bit real.
static const unsigned char real80[10] = {0, 0, 0, 0, 0, 0, 0, 0xe0, 0xff, 0x3f};

int main(void) {
    size_t page = (size_t) getpagesize();
    unsigned char *mem = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED || mprotect(mem + page, page, PROT_NONE) != 0) {
        printf("FAIL setup\n");
        return 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_fault;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    static struct cpu_state cpu;
    for (size_t off = page - 16; off <= page - 10; off++) {
        // Load: the operand's 10 bytes end at or before the PROT_NONE page.
        memset(mem, 0xa5, page);
        memcpy(mem + off, real80, 10);
        memset(&cpu, 0, sizeof(cpu));
        if (sigsetjmp(trap, 1) == 0) {
            fpu_ldm80(&cpu, (float80 *) (mem + off));
            float80 top = cpu.fp[cpu.top % 8];
            check(memcmp(&top, real80, 10) == 0, "fpu_ldm80 loads the operand", off);
        } else {
            check(false, "fpu_ldm80 reads only its 10 bytes (it faulted)", off);
        }

        // Store: 10 bytes written, the rest of the page as it was. The value
        // comes from an aligned copy, so this does not depend on the load.
        float80 aligned = {0};
        memcpy(&aligned, real80, 10);
        memset(&cpu, 0, sizeof(cpu));
        fpu_ldm80(&cpu, &aligned);
        memset(mem, 0xa5, page);
        if (sigsetjmp(trap, 1) == 0) {
            fpu_stm80(&cpu, (float80 *) (mem + off));
            check(memcmp(mem + off, real80, 10) == 0, "fpu_stm80 stores the value", off);
            bool rest = true;
            for (size_t i = off + 10; i < page; i++)
                rest = rest && mem[i] == 0xa5;
            check(rest, "fpu_stm80 writes nothing past its 10 bytes", off);
        } else {
            check(false, "fpu_stm80 writes only its 10 bytes (it faulted)", off);
        }
    }

    if (failures) {
        printf("fpu_m80: %d failure(s)\n", failures);
        return 1;
    }
    printf("fpu_m80: PASS\n");
    return 0;
}
