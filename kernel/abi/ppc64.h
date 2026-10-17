#ifndef KERNEL_ABI_PPC64_H
#define KERNEL_ABI_PPC64_H

#include "misc.h"

// ppc64 (64-bit little-endian PowerPC, ELFv2) is LP64 like amd64 and arm64,
// and the types are the same. Its system call numbers and a handful of its
// constants are its own (docs/ppc64le_guest_plan.md).
typedef qword_t ppc64_guest_addr_t;
typedef qword_t ppc64_guest_word_t;
typedef sqword_t ppc64_guest_sword_t;
typedef qword_t ppc64_guest_reg_t;
typedef qword_t ppc64_guest_ulong_t;
typedef sqword_t ppc64_guest_long_t;
typedef qword_t ppc64_guest_size_t;
typedef sqword_t ppc64_guest_ssize_t;

struct ppc64_iovec_ {
    ppc64_guest_addr_t base;
    ppc64_guest_size_t len;
};

static_assert(sizeof(struct ppc64_iovec_) == 16, "ppc64_iovec size");

#endif
