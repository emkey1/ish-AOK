// FEAT_MOPS (ARMv8.8): the CPY/CPYF memcpy/memmove and SET memset
// instructions. The arm64 JIT's mops gadget (jit/guest-arm64/control.S)
// runs them; this file keeps the model it follows and the switch that
// decides whether guests are told about them.
//
// Each operation is a prologue/main/epilogue triple, e.g.
//     cpyfp [x0]!, [x1]!, x2!
//     cpyfm [x0]!, [x1]!, x2!
//     cpyfe [x0]!, [x1]!, x2!
// and the architecture lets the prologue do any amount of the work, so here
// it does all of it, a host page span at a time, and the main and epilogue
// find Xn = 0 and do nothing. glibc 2.41's __memcpy_mops/__memmove_mops/
// __memset_mops are exactly such triples; one guest instruction then replaces
// a loop of dozens per 64 bytes, each of which costs a dispatch.
//
// What software sees is only the end state, the same under both of the
// architecture's register options: Xd and (for a copy) Xs advanced by the
// original Xn, Xn = 0. NZCV is set as option A leaves it after a forward
// operation (0000); nothing reads it but a MOPS exception handler, and this
// implementation never raises one.
//
// A fault part way leaves the registers describing what is left and reports
// the instruction that was running, so a handler that fixes the page and
// returns restarts it and it finishes the rest -- as the hardware's own
// progress-in-registers design intends. A forward copy advances Xd/Xs and
// shrinks Xn; a backward one (CPY with dst above an overlapping src) copies
// from the end, so it shrinks Xn only, and what is left is the prefix, whose
// source the tail copy did not touch.
//
// Memory is reached through the TLB one page span at a time. A host fault on
// a file page past EOF is taken by the JIT's host fault handlers as for any
// guest access (jit_translate_host_fault).

#include <stdatomic.h>
#include <stdlib.h>
#include "jit/arm64_mops.h"

// ISH_MOPS=0 (or `echo 0 > /proc/ish/arm64_mops`) hides MOPS from AT_HWCAP2,
// ID_AA64ISAR2 and /proc/cpuinfo for programs started afterwards, so glibc
// goes back to its NEON memcpy -- for A/B measurement, or if a program ever
// trips over it. The instructions still execute either way, so a process
// that already chose them keeps working.
static atomic_int mops_advertised = -1;

bool arm64_mops_advertised(void) {
#ifdef ISH_JIT_ARM64_GUEST
    int advertised = atomic_load_explicit(&mops_advertised, memory_order_relaxed);
    if (advertised < 0) {
        const char *env = getenv("ISH_MOPS");
        int from_env = !(env != NULL && env[0] == '0');
        atomic_compare_exchange_strong(&mops_advertised, &advertised, from_env);
        advertised = atomic_load_explicit(&mops_advertised, memory_order_relaxed);
    }
    return advertised;
#else
    return false; // no arm64-guest JIT on this host, so nothing decodes them
#endif
}

void arm64_mops_set_advertised(bool advertised) {
    atomic_store_explicit(&mops_advertised, advertised, memory_order_relaxed);
}
