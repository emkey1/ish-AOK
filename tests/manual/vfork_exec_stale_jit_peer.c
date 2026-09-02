// vfork_exec_stale_jit_peer.c -- the program vfork_exec_stale_jit.c execs.
//
// Not a test: it is built alongside that one purely to be a second image for
// it to exec into, and is never run by the suite on its own.
//
// Two properties are what matter, and both come from being built by the same
// harness rule as its parent test:
//
//   - DIFFERENT code. Re-execing the test itself would put identical code back
//     at identical addresses, so every stale translation would be accidentally
//     correct and the bug would be invisible.
//   - The SAME load address. AOK places an image from its mapped size, so only
//     a binary in the same size class as the test lands on top of it -- which
//     is the whole point, since the per-thread block cache is keyed by guest
//     address. Measured on a Devuan riscv64 root: this peer and the test share
//     an AT_PHDR, while /bin/sh (much larger) does not and reproduces nothing.
//
// So keep it tiny and keep it linked exactly like the tests. The exit status
// is checked by the parent, so don't change it casually.
#ifndef STALE_JIT_PAD_PAGES
#define STALE_JIT_PAD_PAGES 0
#endif

#if STALE_JIT_PAD_PAGES > 0
static volatile char stale_jit_pad[STALE_JIT_PAD_PAGES * 4096];
#endif

int main(void) {
#if STALE_JIT_PAD_PAGES > 0
    stale_jit_pad[0] = 1;
#endif
    return 7;
}
