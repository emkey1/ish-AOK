#ifndef JIT_ARM64_MOPS_H
#define JIT_ARM64_MOPS_H

#include <stdbool.h>
#include <stdint.h>

// Whether the guest is told about MOPS (ISH_MOPS=0 or /proc/ish/arm64_mops
// says no), and the /proc/ish/arm64_mops switch.
bool arm64_mops_advertised(void);
void arm64_mops_set_advertised(bool advertised);

#endif
