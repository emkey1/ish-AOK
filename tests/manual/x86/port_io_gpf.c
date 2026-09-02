// IN/OUT: the port-I/O opcodes e4/e5 (in al/eax, imm8), e6/e7 (out imm8,
// al/eax), ec/ed (in al/eax, dx) and ee/ef (out dx, al/eax).
//
// None of the eight were in the amd64 frontend or the i386 one-byte decode
// table, so every one of them fell through to the generic unrecognized-opcode
// path and raised SIGILL. Real hardware raises #GP(0) instead -- these are
// ring-0 instructions and a user process has neither IOPL(3) nor an ioperm
// bitmap bit -- which Linux turns into SIGSEGV with si_code SI_KERNEL.
//
// The difference is not academic. Probing for a hypervisor by faulting on a
// port access is a deliberate userspace idiom, and the fault it expects is
// SIGSEGV. util-linux's lscpu detects VMware with the "VMXh"/port-0x5658
// backdoor: sys-utils/lscpu-virt.c arms a SIGSEGV handler, runs
// `mov $0x5658,%dx; in (%dx),%eax`, and siglongjmps out of the fault to
// conclude "not VMware". It does not handle SIGILL, so on the amd64 guest
// `lscpu` died outright -- exit 132, not one line of output -- which also
// meant the primary consumer of /sys/devices/system/cpu's topology and cache
// attributes could not run at all.
//
// A fault, unlike a trap, reports the address of the instruction that caused
// it rather than the one after it. That is checked here without any label
// arithmetic (which would need different spellings for i386 PIC and x86_64
// RIP-relative): read the byte AT si_addr out of the process's own text and
// require it to be the opcode that faulted. If si_addr had been advanced past
// the instruction it would point at the next one's first byte instead. For
// the 66-prefixed form the instruction starts at the prefix, so 0x66 is the
// byte to expect -- which also pins down that the fault is reported at the
// prefix and not at the opcode.
//
// x86 only. Builds for both i386 and x86_64 guests; both frontends had the
// same gap, and the amd64 one is where lscpu found it.
#define _GNU_SOURCE
#include <stdio.h>
#include <signal.h>
#include <setjmp.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/utsname.h>
#include "../test_common.h"

#ifndef SI_KERNEL
#define SI_KERNEL 0x80
#endif

static int on_ish;

static sigjmp_buf fault_env;
static volatile int fault_signo;
static volatile int fault_code;
static volatile uintptr_t fault_addr;

static void fault_handler(int sig, siginfo_t *info, void *uc) {
    (void) uc;
    fault_signo = sig;
    fault_code = info->si_code;
    fault_addr = (uintptr_t) info->si_addr;
    siglongjmp(fault_env, 1);
}

// Run one port-I/O instruction under handlers for BOTH plausible outcomes, so
// a build that still raises SIGILL is reported as a wrong signal rather than
// killing the test process with no diagnosis.
//
// want_byte is the instruction's first byte, checked at si_addr; 0 skips it.
static void probe(const char *what, void (*run)(void), unsigned want_byte) {
    struct sigaction sa, old_segv, old_ill;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGILL, &sa, &old_ill);

    fault_signo = 0;
    fault_code = 0;
    fault_addr = 0;
    if (sigsetjmp(fault_env, 1) == 0)
        run();

    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGILL, &old_ill, NULL);

    if (fault_signo != SIGSEGV) {
        printf("FAIL: %s raised %s, want SIGSEGV\n", what,
               fault_signo == 0 ? "no signal" : strsignal(fault_signo));
        failures_total++;
        return;
    }
    test_logf("  ok   %s -> SIGSEGV si_code=%d si_addr=%p\n",
              what, fault_code, (void *) fault_addr);

    // si_code: Linux reports SI_KERNEL for a #GP that is not a memory fault.
    // Asserted only under AOK -- it is the emulator's own classification, and
    // a real kernel is entitled to describe the same #GP differently.
    if (on_ish && fault_code != SI_KERNEL) {
        printf("FAIL: %s si_code=%d, want SI_KERNEL(%d)\n",
               what, fault_code, SI_KERNEL);
        failures_total++;
    }

    if (want_byte != 0) {
        if (fault_addr == 0) {
            printf("FAIL: %s si_addr is NULL, want the faulting instruction\n",
                   what);
            failures_total++;
        } else {
            unsigned got = *(const unsigned char *) fault_addr;
            if (got != want_byte) {
                printf("FAIL: %s si_addr=%p holds %02x, want %02x "
                       "(si_addr must be the instruction, not past it)\n",
                       what, (void *) fault_addr, got, want_byte);
                failures_total++;
            }
        }
    }
}

// ed: in eax, dx -- the exact instruction lscpu's VMware backdoor executes,
// with the same register setup ("VMXh" in eax, port 0x5658 in dx).
static void run_in_dx_32(void) {
    unsigned val;
    __asm__ volatile("movl $0x564d5868,%%eax\n\t"
                     "movl $0x5658,%%edx\n\t"
                     "inl %%dx,%%eax"
                     : "=a"(val) :: "edx");
    (void) val;
}
// ec: in al, dx
static void run_in_dx_8(void) {
    unsigned char val;
    __asm__ volatile("movl $0x5658,%%edx\n\t"
                     "inb %%dx,%%al"
                     : "=a"(val) :: "edx");
    (void) val;
}
// e4 ib: in al, imm8
static void run_in_imm_8(void) {
    unsigned char val;
    __asm__ volatile("inb $0x80,%%al" : "=a"(val));
    (void) val;
}
// e5 ib: in eax, imm8
static void run_in_imm_32(void) {
    unsigned val;
    __asm__ volatile("inl $0x80,%%eax" : "=a"(val));
    (void) val;
}
// ef: out dx, eax
static void run_out_dx_32(void) {
    __asm__ volatile("xorl %%eax,%%eax\n\t"
                     "movl $0x5658,%%edx\n\t"
                     "outl %%eax,%%dx" ::: "eax", "edx");
}
// ee: out dx, al
static void run_out_dx_8(void) {
    __asm__ volatile("xorl %%eax,%%eax\n\t"
                     "movl $0x5658,%%edx\n\t"
                     "outb %%al,%%dx" ::: "eax", "edx");
}
// e6 ib: out imm8, al
static void run_out_imm_8(void) {
    __asm__ volatile("xorl %%eax,%%eax\n\t"
                     "outb %%al,$0x80" ::: "eax");
}
// e7 ib: out imm8, eax
static void run_out_imm_32(void) {
    __asm__ volatile("xorl %%eax,%%eax\n\t"
                     "outl %%eax,$0x80" ::: "eax");
}
// 66 ed: in ax, dx. The operand size has no bearing on the fault, so the
// prefixed encoding must fault exactly like the bare one; a decoder that only
// matched unprefixed opcodes would regress here.
static void run_in_dx_16(void) {
    unsigned short val;
    __asm__ volatile("movl $0x5658,%%edx\n\t"
                     "inw %%dx,%%ax"
                     : "=a"(val) :: "edx");
    (void) val;
}

// The recovery pattern lscpu actually uses: fault, longjmp out, keep running.
// A guest that dies here is one where `lscpu` cannot start.
static int vmware_backdoor_probe(void) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old);
    int detected = 1;
    if (sigsetjmp(fault_env, 1) != 0)
        detected = 0;               // faulted: not a VMware guest
    else
        run_in_dx_32();
    sigaction(SIGSEGV, &old, NULL);
    return detected;
}

int main(int argc, char **argv) {
    struct utsname uts;

    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    if (uname(&uts) == 0 && strstr(uts.release, "ish") != NULL)
        on_ish = 1;
    test_logf("running on %s (AOK=%d)\n", uts.release, on_ish);

    probe("in eax, dx (ed)",    run_in_dx_32,   0xed);
    probe("in al, dx (ec)",     run_in_dx_8,    0xec);
    probe("in al, imm8 (e4)",   run_in_imm_8,   0xe4);
    probe("in eax, imm8 (e5)",  run_in_imm_32,  0xe5);
    probe("out dx, eax (ef)",   run_out_dx_32,  0xef);
    probe("out dx, al (ee)",    run_out_dx_8,   0xee);
    probe("out imm8, al (e6)",  run_out_imm_8,  0xe6);
    probe("out imm8, eax (e7)", run_out_imm_32, 0xe7);
    probe("in ax, dx (66 ed)",  run_in_dx_16,   0x66);

    // Faulting nine times in a row must leave the process fully usable: the
    // #GP path has to unwind cleanly, not just deliver one signal.
    if (vmware_backdoor_probe() != 0) {
        printf("FAIL: VMware backdoor probe reported a hypervisor\n");
        failures_total++;
    } else {
        test_logf("  ok   lscpu-style backdoor probe faulted and recovered\n");
    }

    return finish_suite("port_io_gpf");
}
