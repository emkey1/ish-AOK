// checkpoint_fpenv/witness.c -- see tests/manual/checkpoint_fpenv.sh.
// Sets a rounding mode and raises a flag, reports them, sleeps through the
// save, and reports them again: a restored copy must say the same thing.
#include <fenv.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void report(const char *tag) {
    volatile double one = 1.0, three = 3.0;
    int flags = fetestexcept(FE_ALL_EXCEPT) & (FE_DIVBYZERO | FE_INVALID | FE_OVERFLOW | FE_UNDERFLOW);
    double third = one / three;
    uint64_t bits;
    memcpy(&bits, &third, sizeof(bits));
    printf("%s mode=%#x flags=%#x third=%016llx\n", tag, fegetround(), flags, (unsigned long long) bits);
    fflush(stdout);
}

int main(void) {
    volatile double one = 1.0, zero = 0.0, r;
    fesetround(FE_UPWARD);
    feclearexcept(FE_ALL_EXCEPT);
    r = one / zero;
    (void) r;
    report("BEFORE");
    sleep(8);
    report("AFTER");
    return 0;
}
