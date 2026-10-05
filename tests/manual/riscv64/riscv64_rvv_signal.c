// riscv64_rvv_signal.c -- V state across a signal: the signal arrives while
// vector state is live (kill is issued inside the asm block, so the compiler
// cannot spill around it), the handler clobbers every vector register, vl
// and vtype, and sigreturn must put all of it back; the handler must also
// find Linux's V record header (RISCV_V_MAGIC) in sc_extdesc.hdr.
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <ucontext.h>
static volatile int hits; static int saw_magic;
static void handler(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    uint32_t *hdr = (uint32_t *) ((char *) &uc->uc_mcontext + sizeof(uc->uc_mcontext) - 8);
    saw_magic = hdr[0] == 0x53465457;
    __asm__ volatile(".option push\n.option arch, +v\n"
                     "vsetvli t0, zero, e8, m8, ta, ma\n vmv.v.i v0, -1\n vmv.v.i v8, -1\n"
                     "vmv.v.i v16, -1\n vmv.v.i v24, -1\n vsetivli zero, 1, e64, m1, ta, ma\n.option pop"
                     ::: "t0", "memory");
    hits++;
}
int main(void) {
    struct sigaction sa = {0}; sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, 0);
    uint32_t in[12], out[12];
    for (int i = 0; i < 12; i++) in[i] = 0x1000 + i;
    unsigned long vl, vt;
    register long a0 __asm__("a0") = getpid();
    __asm__ volatile(".option push\n.option arch, +v\n"
                     "vsetivli zero, 12, e32, m4, tu, mu\n vle32.v v4, (%[in])\n"
                     "li a1, %[sig]\n li a7, 129\n ecall\n"   // kill(getpid(), SIGUSR1)
                     "csrr %[vl], vl\n csrr %[vt], vtype\n vse32.v v4, (%[out])\n.option pop"
                     : [vl] "=&r"(vl), [vt] "=&r"(vt), "+r"(a0)
                     : [in] "r"(in), [out] "r"(out), [sig] "i"(SIGUSR1)
                     : "a1", "a7", "memory");
    int ok = hits == 1 && saw_magic && vl == 12 && vt == 0x12 && memcmp(in, out, sizeof(in)) == 0;
    printf("riscv64_rvv_signal: %s (hits %d magic %d vl %lu vtype %#lx data %s)\n", ok ? "PASS" : "FAIL", hits,
           saw_magic, vl, vt, memcmp(in, out, sizeof(in)) ? "clobbered" : "intact");
    return !ok;
}
