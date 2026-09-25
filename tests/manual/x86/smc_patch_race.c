// smc_patch_race -- code patched by another thread, the way HotSpot patches
// it, never runs as a mixture of old and new instruction bytes.
//
// Making a compiled method not entrant, HotSpot rewrites its entry while
// other threads may be calling it: a jmp-to-self (EB FE EB FE) over the first
// aligned word, then the new jump's fifth byte, then the jump's first four
// bytes as one aligned store. That is safe on x86 because a fetch sees an
// aligned word whole: a caller runs the old code, spins, or takes the jump.
//
// AOK's JIT decodes a field at a time, and a compile that read the old opcode
// and then the new ModRM built a block from half of each instruction. A
// 32-bit JVM on the i386 guest died of exactly that (SIGILL on the jump's
// displacement byte), and the SMC race check that catches some of these still
// ran the torn block once.
//
// Here a caller thread calls a function whose entry another thread flips
// between two versions with that same three-step protocol:
//     old: 0f 1f 80 00 00 00 00   nop dword [eax+0]   (7 bytes)
//          b8 01 00 00 00 c3      mov eax, 1; ret
//     new: e9 <rel32>             jmp to "mov eax, 2; ret"
// As in HotSpot, the old entry's first instruction covers all five patched
// bytes, so no caller can be stopped between two instructions inside them.
// The jump's fifth byte is 00, the same as the NOP's, so the only byte that
// really changes is the aligned first word -- and flipping back is as safe as
// flipping forward, which HotSpot never needs. On Linux every call returns 1
// or 2. A torn decode returns something else or faults, and a fault is
// reported with its signal and address. (A first version put a 4-byte NOP and
// a 1-byte NOP where the 7-byte one is; Linux faulted on it too, because a
// caller between the two sees the fifth byte change.)
//
// x86 only; builds and runs on both guests.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "../test_common.h"

#if !defined(__i386__) && !defined(__x86_64__)
int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    printf("smc_patch_race: SKIP (x86 only)\n");
    return 0;
}
#else

static uint8_t *code;               // the entry, 8-byte aligned
static volatile int stop;
static volatile unsigned long calls, ones, twos, others;

static void on_fault(int sig, siginfo_t *info, void *uc) {
    (void) uc;
    char msg[160];
    int n = snprintf(msg, sizeof msg,
                     "FAIL %s at %p after %lu calls (entry %p)\nsmc_patch_race: FAIL failures=1\n",
                     sig == SIGILL ? "SIGILL" : sig == SIGSEGV ? "SIGSEGV" : "signal",
                     info->si_addr, (unsigned long) calls, (void *) code);
    if (n > 0)
        write(1, msg, (size_t) n);
    _exit(1);
}

static void *caller(void *arg) {
    (void) arg;
    int (*f)(void) = (int (*)(void)) (void *) code;
    while (!stop) {
        int r = f();
        calls++;
        if (r == 1)
            ones++;
        else if (r == 2)
            twos++;
        else
            others++;
    }
    return NULL;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void store_word(uint32_t w) {
    __atomic_store_n((uint32_t *) (void *) code, w, __ATOMIC_RELEASE);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    uint8_t *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        printf("FAIL mmap RWX\n");
        return finish_suite("smc_patch_race");
    }
    code = page + 64;
    uint8_t *target = page + 256;   // well past anything a decode of the entry reads
    static const uint8_t old_code[] = {0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00,
                                       0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};
    static const uint8_t target_code[] = {0xb8, 0x02, 0x00, 0x00, 0x00, 0xc3};
    memcpy(code, old_code, sizeof old_code);
    memcpy(target, target_code, sizeof target_code);

    int32_t rel = (int32_t) (target - (code + 5));
    uint8_t jmp[5] = {0xe9};
    memcpy(jmp + 1, &rel, 4);
    if (jmp[4] != old_code[4]) {
        printf("FAIL test layout: the jump's fifth byte must match the NOP's\n");
        return finish_suite("smc_patch_race");
    }
    uint32_t old_word, new_word;
    memcpy(&old_word, old_code, 4);
    memcpy(&new_word, jmp, 4);
    const uint32_t spin = 0xfeebfeeb;   // eb fe eb fe

    pthread_t t;
    pthread_create(&t, NULL, caller, NULL);

    // Flip for a fixed time rather than a count: the guest can be slow.
    unsigned long flips = 0;
    double end = now() + 3.0;
    while (now() < end) {
        // old -> new
        store_word(spin);
        __atomic_store_n(&code[4], jmp[4], __ATOMIC_RELEASE);
        store_word(new_word);
        // new -> old, the same way
        store_word(spin);
        __atomic_store_n(&code[4], old_code[4], __ATOMIC_RELEASE);
        store_word(old_word);
        flips++;
    }
    stop = 1;
    pthread_join(t, NULL);

    test_logf("flips %lu, calls %lu: %lu returned 1, %lu returned 2, %lu other\n",
              flips, (unsigned long) calls, (unsigned long) ones, (unsigned long) twos,
              (unsigned long) others);
    if (others != 0)
        failf("calls returning neither 1 nor 2", others, 0, 0, 0, 0, 0);
    if (calls == 0)
        failf("the caller ran", 0, 0, 0, 1, 0, 0);
    return finish_suite("smc_patch_race");
}
#endif
