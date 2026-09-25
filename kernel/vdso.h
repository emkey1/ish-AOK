#ifndef KERNEL_VDSO_H
#define KERNEL_VDSO_H
#include "tools/ptraceomatic-config.h"

extern const char vdso_data[VDSO_PAGES * (1 << 12)] __asm__("vdso_data");
int vdso_symbol(const char *name);

// The 64-bit vDSO images, one per guest ABI that has one (vdso/arm64/vdso.S
// says what they are for). Each process gets a private copy (kernel/exec.c
// map_vdso64), so these are only ever read.
extern const char vdso_amd64_image[] __asm__("vdso_amd64_image");
extern const char vdso_amd64_image_end[] __asm__("vdso_amd64_image_end");
extern const char vdso_arm64_image[] __asm__("vdso_arm64_image");
extern const char vdso_arm64_image_end[] __asm__("vdso_arm64_image_end");
extern const char vdso_riscv64_image[] __asm__("vdso_riscv64_image");
extern const char vdso_riscv64_image_end[] __asm__("vdso_riscv64_image_end");

#endif
