// Every load and store form at each offset that straddles a page boundary,
// for a diff between an oracle (qemu-ppc64le, a POWER machine) and the AOK
// guest: the accesses that take the TLB's crosspage path.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static unsigned char *buf;
static uint64_t h(void) {
    uint64_t x = 1469598103934665603ULL;
    for (int i = 4096 - 32; i < 4096 + 32; i++) { x ^= buf[i]; x *= 1099511628211ULL; }
    return x;
}
static void fill(void) { for (int i = 0; i < 8192; i++) buf[i] = (unsigned char) (i * 7 + 3); }
#define L(name, ins) { uint64_t r; fill(); __asm__ volatile(ins " %0,0,%1" : "=r"(r) : "r"(p) : "memory"); printf("%s %d %016llx\n", name, off, (unsigned long long) r); }
#define S(name, ins) { fill(); __asm__ volatile(ins " %1,0,%0" : : "r"(p), "r"(v) : "memory"); printf("%s %d %016llx\n", name, off, (unsigned long long) h()); }
#define LU(name, ins) { uint64_t r, q = (uint64_t) p - 8; fill(); __asm__ volatile(ins " %0,8(%1)" : "=r"(r), "+b"(q) : : "memory"); printf("%s %d %016llx %lld\n", name, off, (unsigned long long) r, (long long) (q - (uint64_t) p)); }
#define SU(name, ins) { uint64_t q = (uint64_t) p - 8; fill(); __asm__ volatile(ins " %1,8(%0)" : "+b"(q) : "r"(v) : "memory"); printf("%s %d %016llx %lld\n", name, off, (unsigned long long) h(), (long long) (q - (uint64_t) p)); }
int main(void) {
    buf = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint64_t v = 0x1122334455667788ULL;
    for (int off = 4096 - 17; off <= 4096; off++) {
        unsigned char *p = buf + off;
        L("lhzx", "lhzx") L("lhax", "lhax") L("lwzx", "lwzx") L("lwax", "lwax") L("ldx", "ldx")
        L("lhbrx", "lhbrx") L("lwbrx", "lwbrx") L("ldbrx", "ldbrx")
        S("sthx", "sthx") S("stwx", "stwx") S("stdx", "stdx")
        S("sthbrx", "sthbrx") S("stwbrx", "stwbrx") S("stdbrx", "stdbrx")
        LU("lhzu", "lhzu") LU("lwzu", "lwzu") LU("ldu", "ldu")
        SU("sthu", "sthu") SU("stwu", "stwu") SU("stdu", "stdu")
        { double d; fill(); __asm__ volatile("lfdx %0,0,%1" : "=d"(d) : "r"(p) : "memory"); printf("lfdx %d %016llx\n", off, *(unsigned long long *) &d); }
        { double d; fill(); __asm__ volatile("lfsx %0,0,%1" : "=d"(d) : "r"(p) : "memory"); printf("lfsx %d %016llx\n", off, *(unsigned long long *) &d); }
        { double d = 1.25; fill(); __asm__ volatile("stfdx %1,0,%0" : : "r"(p), "d"(d) : "memory"); printf("stfdx %d %016llx\n", off, (unsigned long long) h()); }
        { double d = 1.25; fill(); __asm__ volatile("stfsx %1,0,%0" : : "r"(p), "d"(d) : "memory"); printf("stfsx %d %016llx\n", off, (unsigned long long) h()); }
        { uint64_t a, b; fill(); __asm__ volatile("lxvd2x 0,0,%2\n\tmfvsrd %0,0\n\txxswapd 0,0\n\tmfvsrd %1,0" : "=r"(a), "=r"(b) : "r"(p) : "memory", "vs0"); printf("lxvd2x %d %016llx %016llx\n", off, (unsigned long long) a, (unsigned long long) b); }
        { uint64_t a, b; fill(); __asm__ volatile("lxvw4x 0,0,%2\n\tmfvsrd %0,0\n\txxswapd 0,0\n\tmfvsrd %1,0" : "=r"(a), "=r"(b) : "r"(p) : "memory", "vs0"); printf("lxvw4x %d %016llx %016llx\n", off, (unsigned long long) a, (unsigned long long) b); }
        { fill(); __asm__ volatile("mtvsrd 0,%1\n\tmtvsrd 1,%2\n\txxpermdi 0,0,1,0\n\tstxvd2x 0,0,%0" : : "r"(p), "r"(v), "r"(~v) : "memory", "vs0", "vs1"); printf("stxvd2x %d %016llx\n", off, (unsigned long long) h()); }
        { fill(); __asm__ volatile("mtvsrd 0,%1\n\tmtvsrd 1,%2\n\txxpermdi 0,0,1,0\n\tstxvw4x 0,0,%0" : : "r"(p), "r"(v), "r"(~v) : "memory", "vs0", "vs1"); printf("stxvw4x %d %016llx\n", off, (unsigned long long) h()); }
        { uint64_t a; fill(); __asm__ volatile("lxvdsx 0,0,%1\n\tmfvsrd %0,0" : "=r"(a) : "r"(p) : "memory", "vs0"); printf("lxvdsx %d %016llx\n", off, (unsigned long long) a); }
    }
    return 0;
}
