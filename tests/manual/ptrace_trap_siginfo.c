// The siginfo a SIGTRAP carries, and it is architecture-dependent.
//
// GH #503: on the x86 guests, `break; run; next` in gdb crashed the program
// with a spurious SIGILL (interpreter) or SIGSEGV (JIT) one byte past the
// breakpoint, instead of stepping to the next line. Every ptrace primitive was
// correct -- POKEDATA put the 0xcc in, the trap fired, GETREGS reported
// breakpoint+1, SETREGS rewound it, SINGLESTEP advanced by the original
// instruction's real length. What was wrong was one number.
//
// gdb does not decide "that SIGTRAP was my own breakpoint" from the address.
// linux-nat's save_stop_reason reads PTRACE_GETSIGINFO and switches on si_code;
// only when it concludes TARGET_STOPPED_BY_SW_BREAKPOINT does it rewind the PC
// by one and restore the original instruction byte. AOK sent TRAP_BRKPT, so gdb
// called the stop a "random signal", left the PC one past the int3 and resumed
// into the middle of the clobbered instruction. Hence the crash: not a bad
// breakpoint, a breakpoint gdb never recognised as its own.
//
// MEASURED on Linux 6.12/x86_64 (both -m32 and -m64), the values below:
//
//     int3        si_code=128 (SI_KERNEL)  si_addr=(nil)
//     singlestep  si_code=2   (TRAP_TRACE) si_addr=<pc>
//
// The two differ because the kernel paths differ. x86's exc_int3 calls do_trap
// with sicode 0, and do_trap's `if (!sicode) force_sig(signr)` sends a BARE
// signal -- no fault layout, so no address. The debug exception goes through
// send_sigtrap -> force_sig_fault and does carry TRAP_* with the PC.
//
// arm64's BRK (brk_handler -> send_user_sigtrap) and riscv64's EBREAK
// (do_trap_break) both use force_sig_fault, so on those guests the breakpoint
// trap correctly reports TRAP_BRKPT with the PC. Same interrupt in the
// emulator, deliberately different answers -- which is why this test asserts
// per-architecture values rather than one shared pair.
//
// Also covers what gdb hit on the way in: reading an address the tracee has not
// mapped through /proc/<pid>/task/<tid>/mem is EIO on Linux, and used to be
// EPERM here. gdb probes unmapped addresses as a matter of course and logged
// "accessing fd 15 for pid 9 failed: Operation not permitted".

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef SI_KERNEL
#define SI_KERNEL 0x80
#endif
#ifndef TRAP_BRKPT
#define TRAP_BRKPT 1
#endif
#ifndef TRAP_TRACE
#define TRAP_TRACE 2
#endif

#if defined(__i386__) || defined(__x86_64__)
#define GUEST_IS_X86 1
#define BREAKPOINT_INSN() __asm__ __volatile__("int3")
#define BREAKPOINT_NAME "int3"
#elif defined(__aarch64__)
#define GUEST_IS_X86 0
#define BREAKPOINT_INSN() __asm__ __volatile__("brk #0")
#define BREAKPOINT_NAME "brk #0"
#elif defined(__riscv)
#define GUEST_IS_X86 0
#define BREAKPOINT_INSN() __asm__ __volatile__("ebreak")
#define BREAKPOINT_NAME "ebreak"
#elif defined(__powerpc64__)
#define GUEST_IS_X86 0
#define BREAKPOINT_INSN() __asm__ __volatile__("trap")
#define BREAKPOINT_NAME "trap"
#else
#define NO_BREAKPOINT_INSN 1
#endif

// The tracee runs to its breakpoint instruction; the tracer reports what the
// stop looked like. Returns 0 on success, or -1 if the child never got there.
static int trap_siginfo(int single_step, siginfo_t *out, int *stopsig) {
    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL fork: %s\n", strerror(errno));
        failures_total++;
        return -1;
    }
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
#ifndef NO_BREAKPOINT_INSN
        if (!single_step)
            BREAKPOINT_INSN();
#endif
        for (volatile int i = 0; i < 3; i++) {}
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status)) {
        printf("FAIL tracee did not reach its initial stop (status %#x)\n", status);
        failures_total++;
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return -1;
    }

    ptrace(single_step ? PTRACE_SINGLESTEP : PTRACE_CONT, pid, 0, 0);
    if (waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status)) {
        printf("FAIL tracee did not stop for the %s (status %#x)\n",
               single_step ? "single-step" : "breakpoint", status);
        failures_total++;
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return -1;
    }
    *stopsig = WSTOPSIG(status);

    // Poison first: a GETSIGINFO that writes nothing must not read as zeros
    // that happen to match an expected code.
    memset(out, 0xAA, sizeof *out);
    errno = 0;
    if (ptrace(PTRACE_GETSIGINFO, pid, 0, out) != 0 || errno != 0) {
        printf("FAIL PTRACE_GETSIGINFO: %s\n", strerror(errno));
        failures_total++;
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return -1;
    }

    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return 0;
}

static void check_breakpoint_trap(void) {
#ifdef NO_BREAKPOINT_INSN
    printf("ptrace_trap_siginfo: SKIP (no breakpoint instruction known for this "
           "architecture)\n");
#else
    siginfo_t si;
    int stopsig = 0;
    if (trap_siginfo(0, &si, &stopsig) != 0)
        return;

    if (stopsig != SIGTRAP || si.si_signo != SIGTRAP) {
        printf("FAIL %s raised signal %d (si_signo %d), expected SIGTRAP\n",
               BREAKPOINT_NAME, stopsig, si.si_signo);
        failures_total++;
        return;
    }

#if GUEST_IS_X86
    // A BARE signal: do_trap's sicode-0 path. si_addr is not part of that
    // layout and Linux leaves it zero -- and gdb's GDB_ARCH_IS_TRAP_BRKPT is
    // what turns this number into a recognised breakpoint.
    if (si.si_code != SI_KERNEL) {
        printf("FAIL %s si_code=%d, expected SI_KERNEL (%d); gdb will call this "
               "a random signal and resume past the trap\n",
               BREAKPOINT_NAME, si.si_code, SI_KERNEL);
        failures_total++;
    }
    if (si.si_addr != NULL) {
        printf("FAIL %s si_addr=%p, expected NULL (a bare signal carries no "
               "fault address)\n", BREAKPOINT_NAME, si.si_addr);
        failures_total++;
    }
#else
    // force_sig_fault, so the fault layout IS used here.
    if (si.si_code != TRAP_BRKPT) {
        printf("FAIL %s si_code=%d, expected TRAP_BRKPT (%d)\n",
               BREAKPOINT_NAME, si.si_code, TRAP_BRKPT);
        failures_total++;
    }
    if (si.si_addr == NULL) {
        printf("FAIL %s si_addr is NULL, expected the faulting PC\n",
               BREAKPOINT_NAME);
        failures_total++;
    }
#endif
    test_log_if(0, "%s: si_code=%d si_addr=%p\n", BREAKPOINT_NAME, si.si_code,
                si.si_addr);
#endif
}

static void check_single_step_trap(void) {
    // Every guest is expected to single-step, riscv64 included. It was the
    // odd one out until jit/jit.c grew cpu_single_step_riscv64: its frontend
    // ignored cpu->tf, so this call ran the tracee to completion like
    // PTRACE_CONT and the check below caught it as "did not stop".
    siginfo_t si;
    int stopsig = 0;
    if (trap_siginfo(1, &si, &stopsig) != 0)
        return;

    if (stopsig != SIGTRAP || si.si_signo != SIGTRAP) {
        printf("FAIL single-step raised signal %d (si_signo %d), expected SIGTRAP\n",
               stopsig, si.si_signo);
        failures_total++;
        return;
    }
    // Every architecture routes the debug exception through force_sig_fault.
    // This is the half that was already right, and it is here so that a change
    // which collapses the two SIGTRAP sources back into one is caught: setting
    // both to SI_KERNEL, or both to TRAP_BRKPT, fails exactly one of these two.
    if (si.si_code != TRAP_TRACE) {
        printf("FAIL single-step si_code=%d, expected TRAP_TRACE (%d)\n",
               si.si_code, TRAP_TRACE);
        failures_total++;
    }
    if (si.si_addr == NULL) {
        printf("FAIL single-step si_addr is NULL, expected the PC\n");
        failures_total++;
    }
    test_log_if(0, "single-step: si_code=%d si_addr=%p\n", si.si_code, si.si_addr);
}

// gdb reads and writes the inferior through /proc/<pid>/task/<tid>/mem, and
// probes addresses that are not mapped. Linux answers EIO.
static void check_proc_mem_unmapped(void) {
    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL fork: %s\n", strerror(errno));
        failures_total++;
        return;
    }
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        _exit(0);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status)) {
        printf("FAIL tracee did not stop for the /proc mem check (status %#x)\n",
               status);
        failures_total++;
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return;
    }

    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task/%d/mem", (int) pid, (int) pid);
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        printf("FAIL open(%s): %s\n", path, strerror(errno));
        failures_total++;
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return;
    }

    // Page zero: never mapped, on any guest, in any layout.
    char buf[64];
    errno = 0;
    ssize_t n = pread(fd, buf, sizeof buf, 0);
    if (n >= 0) {
        printf("FAIL pread of unmapped address 0 returned %zd, expected an error\n", n);
        failures_total++;
    } else if (errno != EIO) {
        printf("FAIL pread of unmapped address 0 gave %s (%d), expected EIO; "
               "EPERM reads as \"not allowed at all\" rather than \"nothing "
               "there\"\n", strerror(errno), errno);
        failures_total++;
    } else {
        test_log_if(0, "/proc/<pid>/task/<tid>/mem unmapped read: EIO\n");
    }

    // ...and a mapped address still has to work, or the check above would pass
    // against a mem file that refused everything.
    errno = 0;
    n = pread(fd, buf, sizeof buf, (off_t) (uintptr_t) &check_proc_mem_unmapped);
    if (n != (ssize_t) sizeof buf) {
        printf("FAIL pread of the tracee's own text returned %zd (%s), expected %zu\n",
               n, n < 0 ? strerror(errno) : "short", sizeof buf);
        failures_total++;
    }

    close(fd);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));

    check_breakpoint_trap();
    check_single_step_trap();
    check_proc_mem_unmapped();

    return finish_suite("ptrace_trap_siginfo");
}
