// An empty loop of 100 million iterations: the JIT's cost per instruction on a
// tight loop.
#include <unistd.h>
#include <stdio.h>

#define nop() __asm__ volatile("")

int main() {
    int loops = 100000000;
    printf("looping %d times\n", loops);
    for (int i = 0; i < loops; i++)
        nop();
    printf("done looping\n");
}
