// singlestep_fused: PTRACE_SINGLESTEP over instruction pairs the JIT fuses
// into one gadget must still stop after EACH instruction.
//
// The single-step path (jit.c cpu_single_step_arm64/_riscv64) compiles a
// block from one gen_step call -- but a fusing gen_step consumes two (or more)
// instructions, so a step over cmp+b.cond (arm64) or slli+add, li+beq,
// lui+addi, ld+ld (riscv64) ran the whole pair: the pc advanced eight bytes
// and the debugger never saw the state between them.
//
// The child stops, then runs `seq`, a run of such pairs with 4-byte encodings.
// The parent steps to seq's first instruction, then single-steps through it
// and requires the pc to advance by exactly 4 bytes per step, plus one taken
// branch whose target is checked.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PTRACE_GETREGSET
#define PTRACE_GETREGSET 0x4204
#endif
#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif

#if defined(__aarch64__) || defined(__riscv)

extern char seq[], seq_branch[], seq_target[], seq_end[];

#if defined(__aarch64__)
// cmp+b.cond (not taken), movz+movk, ands+b.cond, sub+cbz... each pair is
// one gadget under the arm64 "bcond" pass.
__asm__(".text\n .globl seq, seq_branch, seq_target, seq_end\n .p2align 2\n"
        "seq:\n"
        " mov x9, #7\n"
        " cmp x9, #5\n"
        " b.eq 9f\n"
        " movz x10, #0x1234\n"
        " movk x10, #0x5678, lsl #16\n"
        " ands x11, x9, #3\n"
        " b.eq 9f\n"
        " add x12, x9, x10\n"
        " sub x13, x12, #1\n"
        "seq_branch:\n"
        " cmp x9, #7\n"
        " b.eq 8f\n"
        " nop\n"
        "seq_target:\n8:\n"
        " nop\n"
        "seq_end:\n"
        " nop\n"
        "9: b 9b\n");
#define PC_INDEX 32 // struct user_pt_regs: x0-x30, sp, pc
#define REG_WORDS 34
#else
// .option norvc: every instruction 4 bytes. The pairs are the riscv64
// fusions: lui+addi (fold), slli+srli and slli+add (alu), add+ld (alu),
// li+bne and andi+beqz (br), ld+ld (pair).
__asm__(".text\n .globl seq, seq_branch, seq_target, seq_end\n .p2align 2\n"
        " .option push\n .option norvc\n"
        "seq:\n"
        " lui t0, 0x12345\n"
        " addi t0, t0, 0x678\n"
        " slli t1, t0, 32\n"
        " srli t1, t1, 32\n"
        " slli t2, t0, 3\n"
        " add t2, t2, t1\n"
        " add t3, sp, zero\n"
        " ld t4, 0(t3)\n"
        " ld t5, 8(sp)\n"
        " ld t6, 16(sp)\n"
        " li a6, 5\n"
        " beq t0, a6, 9f\n"
        " andi a7, t0, 0x200\n"
        " beqz a7, 9f\n"
        "seq_branch:\n"
        " li a6, 0x12\n"
        " bne t0, a6, 8f\n" // a local label: a global one is relaxed to beq+j
        " nop\n"
        "seq_target:\n8:\n"
        " nop\n"
        "seq_end:\n"
        " nop\n"
        "9: j 9b\n"
        " .option pop\n");
#define PC_INDEX 0 // user_regs_struct: pc first
#define REG_WORDS 32
#endif

static unsigned long get_pc(pid_t pid) {
    unsigned long regs[REG_WORDS + 4];
    struct iovec iov = { .iov_base = regs, .iov_len = REG_WORDS * sizeof(unsigned long) };
    if (ptrace(PTRACE_GETREGSET, pid, (void *) NT_PRSTATUS, &iov) != 0) {
        perror("GETREGSET");
        exit(2);
    }
    return regs[PC_INDEX];
}

static int step(pid_t pid) {
    int status;
    if (ptrace(PTRACE_SINGLESTEP, pid, 0, 0) != 0)
        return -1;
    if (waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status))
        return -1;
    return 0;
}

int main(void) {
    pid_t pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        ((void (*)(void)) seq)();
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    int fails = 0, steps = 0;
    // Step to seq (through raise's return): bounded.
    for (int i = 0; i < 200000 && get_pc(pid) != (unsigned long) seq; i++) {
        if (step(pid) != 0) {
            printf("FAIL: step failed before reaching seq\n");
            kill(pid, SIGKILL);
            return 1;
        }
    }
    if (get_pc(pid) != (unsigned long) seq) {
        printf("FAIL: never reached seq (pc %#lx)\n", get_pc(pid));
        kill(pid, SIGKILL);
        return 1;
    }
    // Straight-line part: +4 per step.
    unsigned long pc = (unsigned long) seq;
    while (pc < (unsigned long) seq_branch) {
        if (step(pid) != 0) { printf("FAIL: step\n"); fails++; break; }
        unsigned long now = get_pc(pid);
        steps++;
        if (now != pc + 4) {
            printf("FAIL: step at seq+%lu went to seq+%ld (want +%lu)\n", pc - (unsigned long) seq,
                   (long) (now - (unsigned long) seq), pc + 4 - (unsigned long) seq);
            fails++;
        }
        pc = now;
    }
    // The constant compare: one step to the branch, one to the target.
    if (pc == (unsigned long) seq_branch) {
        step(pid);
        unsigned long now = get_pc(pid);
        if (now != pc + 4) {
            printf("FAIL: step over the compare went to seq+%ld\n", (long) (now - (unsigned long) seq));
            fails++;
        }
        step(pid);
        now = get_pc(pid);
        if (now != (unsigned long) seq_target) {
            printf("FAIL: taken branch went to seq+%ld, want seq_target\n", (long) (now - (unsigned long) seq));
            fails++;
        }
        steps += 2;
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    printf("singlestep_fused: %s (%d steps)\n", fails ? "FAIL" : "PASS", steps);
    return fails != 0;
}

#else
int main(void) {
    printf("singlestep_fused: SKIP (arm64/riscv64 only)\n");
    return 0;
}
#endif
