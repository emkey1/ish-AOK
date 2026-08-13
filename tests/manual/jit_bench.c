// jit_bench.c -- the two compute workloads jit_fuse_ab.sh drives, in the two
// shapes the JIT's control-flow machinery is actually sensitive to.
//
//   fib <n>   deep recursion: a call and a return per node, which is what a
//             return cache exists for. Guest-neutral C, so the same source
//             sizes the lever on every guest.
//   ind <n>   a dispatch loop through a function-pointer table: an INDIRECT
//             call per iteration (jalr with rd!=0 on riscv64, blr on arm64,
//             call *reg on x86), with a return straight back.
//
// The pair discriminates. The riscv64 return cache measured +6.1% on fib and
// +70.5% on ind on the A9, because caching by TARGET serves indirect calls and
// tail calls, not just returns -- fib alone would have understated it by 10x.
//
// Deliberately no libc work in the timed region beyond the calls themselves,
// and the accumulator is masked so the arithmetic cannot overflow into
// undefined behaviour or into a libc big-number path.
//
// Build for the guest under test (needs a compiler in that guest):
//     cc -O2 -static -o ~/bench/jitbench-<arch> /AOK/tests/jit_bench.c
// Static so it can be dropped into any root, including one with a different
// libc from the one it was built in.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((noinline))
static unsigned long fib(unsigned n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static long f0(long x) { return x + 1; }
static long f1(long x) { return x ^ 3; }
static long f2(long x) { return x * 3; }
static long f3(long x) { return x - 7; }
static long (*const tab[4])(long) = { f0, f1, f2, f3 };

int main(int argc, char **argv) {
    const char *what = argc > 1 ? argv[1] : "fib";
    long n = argc > 2 ? atol(argv[2]) : 35;

    if (strcmp(what, "fib") == 0) {
        printf("%lu\n", fib((unsigned) n));
        return 0;
    }
    if (strcmp(what, "ind") == 0) {
        long acc = 1;
        for (long i = 0; i < n; i++)
            acc = tab[i & 3](acc) & 0xffffff;
        printf("%ld\n", acc);
        return 0;
    }
    fprintf(stderr, "usage: %s fib|ind <n>\n", argv[0]);
    return 2;
}
