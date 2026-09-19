// <shadow.h> for builds whose host has none -- which is every native build,
// since those are Darwin.
//
// Same shape as the other headers beside this one: SmallCLUE's su, sudo and
// passwd were compiled only under __linux__, but the dependency was never
// really the OS. It was a way to read the guest's /etc/shadow and a crypt(3)
// that speaks $6$. Both exist now (kernel/native_libc.c, kernel/sha_crypt.c),
// so this supplies the #include those callers need and routes it at the
// guest's file rather than the Mac's, which does not exist.
#ifndef SMALLCLUE_SHIM_SHADOW_H
#define SMALLCLUE_SHIM_SHADOW_H

#ifdef __linux__
#include_next <shadow.h>
#else

// By explicit relative path: this directory is on SmallCLUE's include path but
// the kernel's headers are not, and the struct must have exactly one
// definition -- see the note in that file.
#include "../../kernel/nlibc_shadow.h"

#define getspnam  nlibc_getspnam
#define lckpwdf   nlibc_lckpwdf
#define ulckpwdf  nlibc_ulckpwdf

#endif
#endif
