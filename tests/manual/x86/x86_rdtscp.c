// x86_rdtscp.c -- RDTSCP and RDPID read IA32_TSC_AUX, which Linux sets to
// (node << 12) | cpu: the same CPU and node the getcpu system call reports.
// RDTSCP is RDTSC as well, the counter not behind an RDTSC before it, and
// the upper halves of RAX, RDX and RCX zero; RDPID r64 zero-extends. The
// process pins itself to one CPU first, so a migration on real hardware
// cannot separate the two readings. 64- and 32-bit builds (CPUID's RDTSCP
// bit is in leaf 0x80000001, which only a 64-bit task sees; RDPID's in leaf
// 7; RDPID is checked where CPUID has it). Checked on an AMD Ryzen (camd).
#define _GNU_SOURCE
#include <cpuid.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures, checks, has_rdpid;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned getcpu_aux(void) {
    unsigned cpu = 0, node = 0;
    syscall(SYS_getcpu, &cpu, &node, NULL);
    return node << 12 | cpu;
}

static void check_once(const char *who) {
    unsigned want = getcpu_aux();
    unsigned long a, d, c;
    unsigned lo0, hi0;
    __asm__ volatile("rdtsc" : "=a"(lo0), "=d"(hi0));
#if defined(__x86_64__)
    a = d = c = ~0ul;
    __asm__ volatile("rdtscp" : "+a"(a), "+d"(d), "+c"(c));
    CHECK((a >> 32) == 0 && (d >> 32) == 0 && (c >> 32) == 0, "%s: rdtscp left upper halves: %#lx %#lx %#lx", who, a, d, c);
    if (has_rdpid) {
        unsigned long pid = ~0ul;
        __asm__ volatile("rdpid %0" : "+r"(pid));
        CHECK(pid == want, "%s: rdpid %#lx, getcpu %#x", who, pid, want);
    }
#else
    unsigned aa, dd, cc;
    __asm__ volatile("rdtscp" : "=a"(aa), "=d"(dd), "=c"(cc));
    a = aa; d = dd; c = cc;
    if (has_rdpid) {
        unsigned pid;
        __asm__ volatile("rdpid %0" : "=r"(pid));
        CHECK(pid == want, "%s: rdpid %#x, getcpu %#x", who, pid, want);
    }
#endif
    CHECK((unsigned) c == want, "%s: rdtscp ecx %#x, getcpu %#x", who, (unsigned) c, want);
    uint64_t t0 = (uint64_t) hi0 << 32 | lo0, t1 = (uint64_t) (d & 0xffffffff) << 32 | (a & 0xffffffff);
    CHECK(t1 >= t0, "%s: rdtscp %llu behind rdtsc %llu", who, (unsigned long long) t1, (unsigned long long) t0);
}

int main(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    sched_setaffinity(0, sizeof set, &set);
    unsigned a, b, c, d;
    __cpuid_count(7, 0, a, b, c, d);
    has_rdpid = (c >> 22) & 1;                   // (cpuid_xsave.c holds AOK to advertising it)
#if defined(__x86_64__)
    __cpuid(0x80000001, a, b, c, d);
    CHECK(d & (1u << 27), "CPUID 0x80000001 EDX has no RDTSCP bit");
#endif
    for (int i = 0; i < 4; i++)
        check_once("parent");
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        check_once("child");
        _exit(failures != 0);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the child's readings: status %#x", status);
    printf("x86_rdtscp: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
