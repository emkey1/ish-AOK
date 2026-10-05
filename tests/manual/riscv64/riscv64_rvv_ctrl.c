// riscv64_rvv_ctrl.c -- PR_RISCV_V_SET_CONTROL / PR_RISCV_V_GET_CONTROL as
// Linux has them (arch/riscv/kernel/vector.c): V reads as on; a running
// program cannot turn it off (EPERM), but can choose what the next exec gets,
// and whether that is inherited past it. An exec with V off gets no AT_HWCAP
// 'v' and an illegal instruction for vsetvli. Also riscv_hwprobe's V bits.
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define SET 69
#define GET 70
static int failures;
static void check(const char *what, long got, long want) {
    if (got != want) {
        printf("FAIL %s: %ld, want %ld\n", what, got, want);
        failures++;
    }
}

static const char *self; // argv[0]: a chroot may have no /proc/self/exe
static sigjmp_buf jb;
static void on_ill(int sig) { (void) sig; siglongjmp(jb, 1); }
__attribute__((target("arch=rv64gcv"), noinline)) static int v_works(void) {
    signal(SIGILL, on_ill);
    if (sigsetjmp(jb, 1))
        return 0;
    unsigned long vl;
    __asm__ volatile("vsetvli %0, zero, e8, m1, ta, ma" : "=r"(vl) : : "vl", "vtype");
    return vl == 16;
}

// run this program again and read its "ctrl hwcap-v works" line
static void child(const char *what, long want_ctrl, int want_hwcap, int want_works) {
    int p[2];
    if (pipe(p) != 0) { perror("pipe"); exit(1); }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        execl(self, "riscv64_rvv_ctrl", "child", (char *) NULL);
        _exit(127);
    }
    close(p[1]);
    char buf[64] = "";
    ssize_t n = read(p[0], buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = 0;
    close(p[0]);
    waitpid(pid, NULL, 0);
    long ctrl = -1;
    int hw = -1, works = -1;
    sscanf(buf, "%ld %d %d", &ctrl, &hw, &works);
    char name[128];
    snprintf(name, sizeof(name), "%s: the exec's GET_CONTROL", what);
    check(name, ctrl, want_ctrl);
    snprintf(name, sizeof(name), "%s: the exec's AT_HWCAP 'v'", what);
    check(name, hw, want_hwcap);
    snprintf(name, sizeof(name), "%s: vsetvli runs in the exec", what);
    check(name, works, want_works);
}

int main(int argc, char **argv) {
    self = argv[0];
    if (argc > 1) {
        printf("%ld %d %d\n", (long) prctl(GET, 0, 0, 0, 0), (int) (getauxval(AT_HWCAP) >> ('v' - 'a') & 1), v_works());
        return 0;
    }
    struct { int64_t key; uint64_t value; } hp[] = {{4, 0}, {10, 0}};
    check("hwprobe", syscall(258, hp, 2, 0, NULL, 0), 0);
    uint64_t want = 1 << 2 | 1 << 17 | 1 << 19 | 1 << 26 | 1ull << 31; // V Zvbb Zvkb Zvkt Zvfhmin
    check("hwprobe ima_ext_0 V bits", (long) (hp[0].value & want), (long) want);
    check("hwprobe misaligned_vector_perf (unknown)", (long) hp[1].value, 0);
    check("AT_HWCAP 'v'", (long) (getauxval(AT_HWCAP) >> ('v' - 'a') & 1), 1);
    check("GET_CONTROL: on", prctl(GET, 0, 0, 0, 0), 2);
    check("vsetvli runs", v_works(), 1);
    errno = 0;
    check("SET_CONTROL off: refused", prctl(SET, 1, 0, 0, 0), -1);
    check("SET_CONTROL off: EPERM", errno, EPERM);
    errno = 0;
    check("SET_CONTROL cur 3: refused", prctl(SET, 3, 0, 0, 0), -1);
    check("SET_CONTROL cur 3: EINVAL", errno, EINVAL);
    errno = 0;
    check("SET_CONTROL bit 5: refused", prctl(SET, 0x20, 0, 0, 0), -1);
    check("SET_CONTROL bit 5: EINVAL", errno, EINVAL);
    check("SET_CONTROL default", prctl(SET, 0, 0, 0, 0), 0);
    check("GET_CONTROL after default", prctl(GET, 0, 0, 0, 0), 2);
    // the next exec off, once
    check("SET_CONTROL next off", prctl(SET, 2 | 1 << 2, 0, 0, 0), 0);
    check("GET_CONTROL next off", prctl(GET, 0, 0, 0, 0), 2 | 1 << 2);
    check("vsetvli still runs here", v_works(), 1);
    child("next off", 1, 0, 0);
    // ... and inherited past that exec
    check("SET_CONTROL next off, inherited", prctl(SET, 2 | 1 << 2 | 16, 0, 0, 0), 0);
    child("next off, inherited", 1 | 1 << 2 | 16, 0, 0);
    // back to the default
    check("SET_CONTROL next default", prctl(SET, 2, 0, 0, 0), 0);
    child("next default", 2, 1, 1);
    printf("riscv64_rvv_ctrl: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
