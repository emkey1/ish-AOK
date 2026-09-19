// struct spwd and the shadow-file calls, for native (host) code.
//
// The single definition, included from two directions: kernel/native_libc.c
// implements these, and deps/smallclue-shim/shadow.h serves them to SmallCLUE
// as <shadow.h> on platforms that have none. One header rather than a copy in
// each, because a struct layout that two files declare separately is a struct
// layout two files can disagree about -- and this one is read by an
// authentication check.
//
// NOT reachable by putting deps/smallclue-shim on the kernel's include path:
// that directory also shadows <zlib.h> and <openssl/evp.h> for whatever
// includes it, which is fine for SmallCLUE and would not be for libish.
#ifndef KERNEL_NLIBC_SHADOW_H
#define KERNEL_NLIBC_SHADOW_H

struct spwd {
    char *sp_namp;    // login name
    char *sp_pwdp;    // hashed password -- "*" or "!..." means locked
    long sp_lstchg;
    long sp_min;
    long sp_max;
    long sp_warn;
    long sp_inact;
    long sp_expire;
    unsigned long sp_flag;
};

// One entry cached at a time, as getspnam's contract allows: the returned
// pointer is valid only until the next call. Reads the GUEST's /etc/shadow.
struct spwd *nlibc_getspnam(const char *name);

// /etc/.pwd.lock, the advisory lock taken before rewriting the shadow file.
int nlibc_lckpwdf(void);
int nlibc_ulckpwdf(void);

#endif
