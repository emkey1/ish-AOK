// exec_i386_fault_addr.c -- an i386 image must report its own page faults with
// a clean high half, even when the task it was exec'd into descends from a
// 64-bit guest.
//
// cpu->segfault_addr is a 64-bit guest_addr_t shared by every guest ABI, but
// the i386 JIT's fault path wrote it with a 32-bit store (`str _addr` on the
// aarch64 host, `movl %_addr` on the x86_64 one). That writes only the low
// word and leaves the high word holding whatever was there before -- and
// nothing ever clears it across the boundary where the ABI changes: exec
// resets the register file field by field without touching segfault_addr,
// while fork's task_create_ copies the whole struct.
//
// So an i386 image exec'd by a task descended from a 64-bit one reported every
// fault with the ancestor's high half glued on. arm64 guest mappings live at
// 0x7fff'xxxxxxxx, so an ordinary stack-growth fault at 0xffff9e50 surfaced as
// 0x7fffffff9e50. handle_page_fault_interrupt() then retried GROWSDOWN at that
// address, found nothing mapped there, and killed the guest.
//
// The user-visible shape: every statically linked ET_EXEC i386 binary died
// with SIGSEGV inside libc startup (glibc's _dl_get_origin does
// `sub $0x101c,%esp` then a `call`, whose pushed return address is the first
// write past the one-page initial stack) when run from an aarch64 root -- but
// the same binary ran fine when exec'd in place (`exec ./prog`, or a shell's
// last-command optimization), because then no fork had copied a 64-bit
// ancestor's fault address in. It looked filesystem- and root-specific and was
// neither: the discriminator was only ever whether a fork sat between the
// 64-bit ancestor and the i386 image. PIE binaries hid it too -- they are the
// common case in a distro, and their loader touches the stack differently.
//
// Reproducing it needs two things to line up, and getting the FIRST one right
// is the whole difficulty of this test:
//
//   1. The last unsatisfiable guest fault before execve is at a >4GB address.
//      "Last" is literal: every fault overwrites the field, and the initial
//      stack sits at 0xffff'd000, so any stack growth on the run-up to execve
//      puts the high half back to zero. A real shell trips this because it is
//      dynamically linked -- its writable data lives in the high mmap region,
//      so its post-fork copy-on-write faults are high. A static test binary
//      faults on its own data down at ~0x4a0000 instead and cannot reproduce
//      the bug at all, which is why the poison here is a deliberate
//      copy-on-write write to a high anonymous mapping, performed in the CHILD
//      as the last thing before the exec, after a warm-up pass that un-shares
//      every page the exec path itself touches.
//   2. The i386 child writes past its initial one-page stack, so it takes a
//      GROWSDOWN fault of its own.
//
// The child is a ~110 byte i386 ELF built here rather than compiled, so the
// test needs no cross toolchain and no second binary: it grows its stack
// 16 KiB, stores a sentinel there, reads it back, and exits with it.
//
// On an i386 guest the high half is always zero, so this cannot fail -- it
// still runs there as a plain exec + stack-growth guard. On a host that cannot
// execute i386 binaries at all (a non-x86 real Linux) it skips.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

extern char **environ;

#define TRIALS 8
#define CHILD_SENTINEL 42
// Grown by the child before it stores. Four guest pages past the initial
// one-page stack: far enough that the store cannot land on an already-mapped
// page, small enough to stay a normal stack expansion.
#define CHILD_STACK_GROWTH 0x4000

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}

// mov %esp,%ecx / sub $CHILD_STACK_GROWTH,%esp / movl $42,(%esp) /
// mov (%esp),%ebx / mov %ecx,%esp / mov $1,%eax / int $0x80
//
// The store and the load-back are separate so the exit status proves the grown
// page is really readable and writable, not merely that the store was skipped.
// esp is restored before the exit syscall so the guest never leaves it below
// the region it just grew.
static const uint8_t i386_code[] = {
    0x89, 0xe1,
    0x81, 0xec, (uint8_t) CHILD_STACK_GROWTH, (uint8_t) (CHILD_STACK_GROWTH >> 8), 0x00, 0x00,
    0xc7, 0x04, 0x24, CHILD_SENTINEL, 0x00, 0x00, 0x00,
    0x8b, 0x1c, 0x24,
    0x89, 0xcc,
    0xb8, 0x01, 0x00, 0x00, 0x00,
    0xcd, 0x80,
};

#define ELF_LOAD_ADDR 0x08048000u
#define EHDR_SIZE 52
#define PHDR_SIZE 32

// A single r-x PT_LOAD covering the whole file, filesz == memsz so there is no
// BSS and the loader's tail/zero-fill paths play no part in what is measured.
static size_t build_i386_elf(uint8_t *out, size_t out_size) {
    size_t hdrs = EHDR_SIZE + PHDR_SIZE;
    size_t total = hdrs + sizeof i386_code;
    if (out_size < total)
        return 0;
    memset(out, 0, hdrs);
    memcpy(out, "\177ELF", 4);
    out[4] = 1;                                 // ELFCLASS32
    out[5] = 1;                                 // ELFDATA2LSB
    out[6] = 1;                                 // EV_CURRENT
    put16(out + 16, 2);                         // ET_EXEC (non-PIE: fixed load address)
    put16(out + 18, 3);                         // EM_386
    put32(out + 20, 1);                         // e_version
    put32(out + 24, ELF_LOAD_ADDR + (uint32_t) hdrs);   // e_entry
    put32(out + 28, EHDR_SIZE);                 // e_phoff
    put16(out + 40, EHDR_SIZE);                 // e_ehsize
    put16(out + 42, PHDR_SIZE);                 // e_phentsize
    put16(out + 44, 1);                         // e_phnum

    uint8_t *ph = out + EHDR_SIZE;
    put32(ph + 0, 1);                           // PT_LOAD
    put32(ph + 4, 0);                           // p_offset
    put32(ph + 8, ELF_LOAD_ADDR);               // p_vaddr
    put32(ph + 12, ELF_LOAD_ADDR);              // p_paddr
    put32(ph + 16, (uint32_t) total);           // p_filesz
    put32(ph + 20, (uint32_t) total);           // p_memsz
    put32(ph + 24, 5);                          // PF_R | PF_X
    put32(ph + 28, 0x1000);                     // p_align

    memcpy(out + hdrs, i386_code, sizeof i386_code);
    return total;
}

static int write_i386_helper(const char *path) {
    uint8_t image[EHDR_SIZE + PHDR_SIZE + sizeof i386_code];
    size_t size = build_i386_elf(image, sizeof image);
    if (size == 0)
        return -1;
    // Must be closed before the exec, or the child gets ETXTBSY.
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, image, size);
    if (close(fd) < 0 || n != (ssize_t) size)
        return -1;
    return chmod(path, 0755);
}

// Touch the whole stack the child is about to use, so that nothing between the
// poison and the execve takes a GROWSDOWN fault of its own.
static void pregrow_stack(void) {
    volatile char pad[128 * 1024];
    memset((void *) pad, 0, sizeof pad);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    char helper[64];
    snprintf(helper, sizeof helper, "./exec_i386_fault_addr_child.%ld",
             (long) getpid());
    if (write_i386_helper(helper) < 0) {
        printf("exec_i386_fault_addr: SKIP (cannot write helper: %s)\n",
               strerror(errno));
        return 0;
    }

    // The page the child will fault on. Mapped and touched HERE so that after
    // each fork it is shared-and-clean in the child, and the child's first
    // write to it is a copy-on-write fault -- at whatever address the guest
    // places anonymous mappings, which on every 64-bit guest is above 4GB.
    volatile unsigned char *high = (volatile unsigned char *)
            mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (high == MAP_FAILED) {
        printf("exec_i386_fault_addr: SKIP (mmap: %s)\n", strerror(errno));
        unlink(helper);
        return 0;
    }
    high[0] = 1;
    int can_poison = sizeof(void *) > 4 && ((uint64_t) (uintptr_t) high >> 32) != 0;
    test_logf("poison page at %#llx (discriminating=%d)\n",
              (unsigned long long) (uintptr_t) high, can_poison);

    for (int trial = 0; trial < TRIALS; trial++) {
        char *const child_argv[] = { helper, NULL };

        pid_t pid = fork();
        if (pid < 0) {
            printf("FAIL fork: %s\n", strerror(errno));
            failures_total++;
            break;
        }
        if (pid == 0) {
            // Everything here exists to make the poison the LAST fault.
            pregrow_stack();
            // Warm-up exec: fails, but un-shares every page libc's execve path
            // and errno touch, so the real one below faults nowhere.
            syscall(SYS_execve, "", child_argv, environ);
            high[0] = 2;                        // the poison: COW at a high address
            syscall(SYS_execve, helper, child_argv, environ);
            _exit(126);                         // host cannot run i386 at all
        }

        int st = 0;
        if (waitpid(pid, &st, 0) != pid) {
            printf("FAIL waitpid: %s\n", strerror(errno));
            failures_total++;
            break;
        }
        if (WIFEXITED(st) && WEXITSTATUS(st) == 126) {
            printf("exec_i386_fault_addr: SKIP (host cannot execute i386 binaries)\n");
            unlink(helper);
            return 0;
        }
        if (WIFSIGNALED(st)) {
            printf("FAIL trial %d: i386 child killed by signal %d "
                   "(stack growth faulted at an address with a stale high half)\n",
                   trial, WTERMSIG(st));
            failures_total++;
            continue;
        }
        if (!WIFEXITED(st) || WEXITSTATUS(st) != CHILD_SENTINEL) {
            printf("FAIL trial %d: i386 child exited %d, expected %d\n",
                   trial, WIFEXITED(st) ? WEXITSTATUS(st) : -1, CHILD_SENTINEL);
            failures_total++;
            continue;
        }
        test_logf("trial %d: i386 child grew its stack and exited %d\n",
                  trial, CHILD_SENTINEL);
    }

    unlink(helper);
    if (!can_poison)
        test_logf("no mapping above 4GB on this guest: the fault address has no "
                  "high half to inherit, so this run is an exec + stack-growth "
                  "guard only\n");
    return finish_suite("exec_i386_fault_addr");
}
