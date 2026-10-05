// riscv64_rvv_ptrace.c -- the NT_RISCV_VECTOR regset: a stopped child's
// vector state (vl, vlenb, vcsr, a register) reads back as it set it, and a
// SETREGSET write reaches the child's registers.
#include <elf.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
struct vstate { unsigned long vstart, vl, vtype, vcsr, vlenb; uint8_t v[32 * 16]; };
int main(void) {
    pid_t pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        uint32_t in[4] = {11, 22, 33, 44}, out[4];
        __asm__ volatile(".option push\n.option arch, +v\n vsetivli zero, 4, e32, m1, ta, ma\n vle32.v v3, (%0)\n"
                         "csrwi vxrm, 2\n.option pop" : : "r"(in) : "memory");
        raise(SIGSTOP);
        __asm__ volatile(".option push\n.option arch, +v\n vsetivli zero, 4, e32, m1, ta, ma\n vse32.v v3, (%0)\n.option pop"
                         : : "r"(out) : "memory");
        _exit(out[0] == 99 && out[1] == 22 ? 0 : 1);
    }
    int st; waitpid(pid, &st, 0);
    struct vstate vs; struct iovec iov = {&vs, sizeof(vs)};
    long r = ptrace(PTRACE_GETREGSET, pid, (void *) NT_RISCV_VECTOR, &iov);
    uint32_t v3_0; memcpy(&v3_0, vs.v + 3 * 16, 4);
    int ok = r == 0 && iov.iov_len == sizeof(vs) && vs.vl == 4 && vs.vlenb == 16 && (vs.vcsr >> 1) == 2 && v3_0 == 11;
    uint32_t nv = 99; memcpy(vs.v + 3 * 16, &nv, 4);
    r = ptrace(PTRACE_SETREGSET, pid, (void *) NT_RISCV_VECTOR, &iov);
    ok = ok && r == 0;
    ptrace(PTRACE_CONT, pid, 0, 0);
    waitpid(pid, &st, 0);
    ok = ok && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("riscv64_rvv_ptrace: %s (vl %lu vlenb %lu vcsr %#lx v3[0] %u)\n", ok ? "PASS" : "FAIL", vs.vl, vs.vlenb, vs.vcsr, v3_0);
    return !ok;
}
