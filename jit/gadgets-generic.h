#include "cpu-offsets.h"

#define ifin(thing, ...) _ifin(thing, __COUNTER__, __VA_ARGS__)
#define _ifin(thing, line, ...) __ifin(thing, line, __VA_ARGS__)
#define __ifin(thing, line, ...) irp da_op##line, __VA_ARGS__ N .ifc thing,\da_op##line
#define endifin endif N .endr

# sync with enum reg
#define REG_LIST reg_a,reg_c,reg_d,reg_b,reg_sp,reg_bp,reg_si,reg_di
# sync with enum arg
#define GADGET_LIST REG_LIST,imm,mem,addr
# sync with enum size
#define SIZE_LIST 8,16,32

# darwin/linux compatibility
.macro .pushsection_rodata
#if __APPLE__
    .pushsection __DATA,__const
#else
    .pushsection .data.rel.ro
#endif
.endm
.macro .pushsection_bullshit
#if __APPLE__
    .pushsection __TEXT,__text_bullshit,regular,pure_instructions
#else
    .pushsection .text.bullshit
#endif
.endm

#if defined(__aarch64__)
# The guest's time-stamp and virtual counters (x86 RDTSC, arm64 CNTVCT_EL0):
# the host's system counter in nanoseconds, floor(CNTVCT * 1e9 / CNTFRQ) done
# as quotient and remainder so nothing overflows. 1 GHz is what the guests are
# told (arm64 CNTFRQ_EL0). The ISB keeps the read after everything before it,
# so a guest's LFENCE; RDTSC or ISB; MRS (which it may not pass on) still
# orders it -- without it a read could come out older than a value another
# thread had already published. \rd, \t1-\t3: x registers, all clobbered.
.macro host_counter_ns rd, t1, t2, t3
    isb
    mrs  \rd, cntvct_el0
    mrs  \t1, cntfrq_el0
    udiv \t2, \rd, \t1
    msub \t3, \t2, \t1, \rd
    movz \rd, 0xca00
    movk \rd, 0x3b9a, lsl 16            /* 1e9 */
    mul  \t2, \t2, \rd
    mul  \t3, \t3, \rd
    udiv \t3, \t3, \t1
    add  \rd, \t2, \t3
.endm
#endif

#if __APPLE__
#define NAME(x) _##x
#else
#define NAME(x) x
#endif
.macro .global.name name
    .global NAME(\name)
    NAME(\name)\():
.endm

.macro .type_compat type:vararg
#if !__APPLE__
    .type \type
#endif
.endm

# an array of gadgets
.macro _gadget_array_start name
    .pushsection_rodata
    .p2align 3
    .type_compat \name\()_gadgets,@object
    .global.name \name\()_gadgets
.endm

.macro gadgets type, list:vararg
    .irp arg, \list
        .ifndef NAME(gadget_\type\()_\arg)
            .set NAME(gadget_\type\()_\arg), 0
        .endif
        .quad NAME(gadget_\type\()_\arg)
    .endr
.endm

.macro .gadget_list type, list:vararg
    _gadget_array_start \type
        gadgets \type, \list
    .popsection
.endm

.macro .gadget_list_size type, list:vararg
    _gadget_array_start \type
        # sync with enum size
        gadgets \type\()8, \list
        gadgets \type\()16, \list
        gadgets \type\()32, \list
        gadgets \type\()64, \list
        gadgets \type\()80, \list
    .popsection
.endm

.macro .gadget_array type
    .gadget_list_size \type, GADGET_LIST
.endm

# jfc
# https://github.com/llvm-mirror/llvm/blob/release_80/lib/Target/AArch64/MCTargetDesc/AArch64MCAsmInfo.cpp#L41
# https://bugs.llvm.org/show_bug.cgi?id=39010#c4
#if defined(__APPLE__) && defined(__arm64__)
#define N %%
#else
#define N ;
#endif

# vim: ft=gas
