// arm64_lse_gadgets.c -- the arm64 guest's atomics, gadgets throughout
// (jit/guest-arm64/atomics.S): every LSE read-modify-write (LDADD LDCLR LDEOR
// LDSET LDSMAX LDSMIN LDUMAX LDUMIN SWP) at every size and ordering, CAS,
// CASP (W and X pairs), LDXP/STXP, against a C model on random and edge
// values; four threads hammering LDADD and CAS for lost updates; and the
// faults: a misaligned atomic (SIGBUS, BUS_ADRALN -- it used to re-execute
// forever), one on an unmapped page and CAS on a read-only page (SIGSEGV),
// and LDXP on a read-only page (fine).
//
// Needs HWCAP_ATOMICS (AOK advertises it).
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define LSE __attribute__((target("+lse"), noinline))
static unsigned long checks, bad;
static void report(const char *what, int sz, uint64_t got, uint64_t want) {
    checks++;
    if (got != want && bad++ < 40)
        printf("%s/%d: %#llx, want %#llx\n", what, sz, (unsigned long long) got, (unsigned long long) want);
}
static uint64_t rng = 0x243f6a8885a308d3ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static const uint64_t edge[] = {0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000,
    0xffffffff, 0x7fffffffffffffffull, 0x8000000000000000ull, ~0ull};
#define NEDGE (sizeof(edge) / sizeof(edge[0]))

static uint64_t mask_of(int sz) { return sz == 8 ? ~0ull : (1ull << (sz * 8)) - 1; }
static int64_t sx(uint64_t v, int sz) { return sz == 8 ? (int64_t) v : (int64_t) (v << (64 - 8 * sz)) >> (64 - 8 * sz); }
static uint64_t model(int op, uint64_t old, uint64_t arg, int sz) {
    uint64_t m = mask_of(sz);
    old &= m;
    arg &= m;
    switch (op) {
    case 0: return (old + arg) & m;
    case 1: return old & ~arg;
    case 2: return old ^ arg;
    case 3: return old | arg;
    case 4: return sx(old, sz) > sx(arg, sz) ? old : arg;
    case 5: return sx(old, sz) < sx(arg, sz) ? old : arg;
    case 6: return old > arg ? old : arg;
    case 7: return old < arg ? old : arg;
    default: return arg;
    }
}

LSE static uint64_t f_ldaddb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldaddb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldaddh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldaddh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldadd_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldadd %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldadd_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldadd %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldaddab_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldaddab %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldaddah_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldaddah %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldadda_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldadda %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldadda_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldadda %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldaddlb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldaddlb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldaddlh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldaddlh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldaddl_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldaddl %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldaddl_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldaddl %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldaddalb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldaddalb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldaddalh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldaddalh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldaddal_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldaddal %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldaddal_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldaddal %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldclrb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldclrb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldclrh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldclrh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldclr_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldclr %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldclr_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldclr %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldclrab_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldclrab %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldclrah_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldclrah %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldclra_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldclra %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldclra_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldclra %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldclrlb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldclrlb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldclrlh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldclrlh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldclrl_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldclrl %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldclrl_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldclrl %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldclralb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldclralb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldclralh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldclralh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldclral_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldclral %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldclral_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldclral %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldeorb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldeorb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldeorh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldeorh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldeor_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldeor %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldeor_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldeor %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldeorab_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldeorab %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldeorah_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldeorah %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldeora_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldeora %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldeora_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldeora %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldeorlb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldeorlb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldeorlh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldeorlh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldeorl_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldeorl %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldeorl_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldeorl %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldeoralb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldeoralb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldeoralh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldeoralh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldeoral_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldeoral %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldeoral_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldeoral %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsetb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsetb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldseth_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldseth %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldset_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldset %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldset_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldset %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsetab_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsetab %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsetah_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsetah %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldseta_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldseta %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldseta_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldseta %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsetlb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsetlb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsetlh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsetlh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsetl_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsetl %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsetl_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsetl %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsetalb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsetalb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsetalh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsetalh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsetal_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsetal %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsetal_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsetal %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmaxb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmaxb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmaxh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmaxh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmax_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmax %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmax_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmax %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmaxab_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmaxab %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmaxah_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmaxah %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmaxa_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmaxa %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmaxa_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmaxa %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmaxlb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmaxlb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmaxlh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmaxlh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmaxl_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmaxl %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmaxl_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmaxl %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmaxalb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmaxalb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmaxalh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmaxalh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmaxal_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmaxal %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmaxal_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmaxal %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsminb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsminb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsminh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsminh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmin_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmin %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmin_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmin %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsminab_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsminab %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsminah_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsminah %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmina_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmina %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsmina_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsmina %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsminlb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsminlb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsminlh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsminlh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsminl_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsminl %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsminl_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsminl %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsminalb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsminalb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsminalh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsminalh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsminal_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsminal %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldsminal_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldsminal %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumaxb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumaxb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumaxh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumaxh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumax_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumax %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumax_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumax %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumaxab_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumaxab %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumaxah_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumaxah %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumaxa_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumaxa %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumaxa_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumaxa %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumaxlb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumaxlb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumaxlh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumaxlh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumaxl_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumaxl %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumaxl_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumaxl %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumaxalb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumaxalb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumaxalh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumaxalh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumaxal_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumaxal %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumaxal_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumaxal %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_lduminb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("lduminb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_lduminh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("lduminh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumin_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumin %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumin_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumin %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_lduminab_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("lduminab %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_lduminah_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("lduminah %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumina_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumina %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_ldumina_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("ldumina %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_lduminlb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("lduminlb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_lduminlh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("lduminlh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_lduminl_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("lduminl %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_lduminl_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("lduminl %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_lduminalb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("lduminalb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_lduminalh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("lduminalh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_lduminal_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("lduminal %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_lduminal_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("lduminal %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_swpb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("swpb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_swph_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("swph %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_swp_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("swp %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_swp_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("swp %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_swpab_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("swpab %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_swpah_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("swpah %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_swpa_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("swpa %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_swpa_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("swpa %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_swplb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("swplb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_swplh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("swplh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_swpl_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("swpl %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_swpl_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("swpl %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_swpalb_1(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("swpalb %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_swpalh_2(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("swpalh %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_swpal_4(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("swpal %w1, %w0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
LSE static uint64_t f_swpal_8(void *p, uint64_t arg) { uint64_t old; __asm__ volatile("swpal %x1, %x0, [%2]" : "=&r"(old) : "r"(arg), "r"(p) : "memory"); return old; }
struct rmw { uint64_t (*fn)(void *, uint64_t); int op, sz; const char *name; };
static const struct rmw rmws[] = {
    {f_ldaddb_1, 0, 1, "ldaddb"},
    {f_ldaddh_2, 0, 2, "ldaddh"},
    {f_ldadd_4, 0, 4, "ldadd"},
    {f_ldadd_8, 0, 8, "ldadd"},
    {f_ldaddab_1, 0, 1, "ldaddab"},
    {f_ldaddah_2, 0, 2, "ldaddah"},
    {f_ldadda_4, 0, 4, "ldadda"},
    {f_ldadda_8, 0, 8, "ldadda"},
    {f_ldaddlb_1, 0, 1, "ldaddlb"},
    {f_ldaddlh_2, 0, 2, "ldaddlh"},
    {f_ldaddl_4, 0, 4, "ldaddl"},
    {f_ldaddl_8, 0, 8, "ldaddl"},
    {f_ldaddalb_1, 0, 1, "ldaddalb"},
    {f_ldaddalh_2, 0, 2, "ldaddalh"},
    {f_ldaddal_4, 0, 4, "ldaddal"},
    {f_ldaddal_8, 0, 8, "ldaddal"},
    {f_ldclrb_1, 1, 1, "ldclrb"},
    {f_ldclrh_2, 1, 2, "ldclrh"},
    {f_ldclr_4, 1, 4, "ldclr"},
    {f_ldclr_8, 1, 8, "ldclr"},
    {f_ldclrab_1, 1, 1, "ldclrab"},
    {f_ldclrah_2, 1, 2, "ldclrah"},
    {f_ldclra_4, 1, 4, "ldclra"},
    {f_ldclra_8, 1, 8, "ldclra"},
    {f_ldclrlb_1, 1, 1, "ldclrlb"},
    {f_ldclrlh_2, 1, 2, "ldclrlh"},
    {f_ldclrl_4, 1, 4, "ldclrl"},
    {f_ldclrl_8, 1, 8, "ldclrl"},
    {f_ldclralb_1, 1, 1, "ldclralb"},
    {f_ldclralh_2, 1, 2, "ldclralh"},
    {f_ldclral_4, 1, 4, "ldclral"},
    {f_ldclral_8, 1, 8, "ldclral"},
    {f_ldeorb_1, 2, 1, "ldeorb"},
    {f_ldeorh_2, 2, 2, "ldeorh"},
    {f_ldeor_4, 2, 4, "ldeor"},
    {f_ldeor_8, 2, 8, "ldeor"},
    {f_ldeorab_1, 2, 1, "ldeorab"},
    {f_ldeorah_2, 2, 2, "ldeorah"},
    {f_ldeora_4, 2, 4, "ldeora"},
    {f_ldeora_8, 2, 8, "ldeora"},
    {f_ldeorlb_1, 2, 1, "ldeorlb"},
    {f_ldeorlh_2, 2, 2, "ldeorlh"},
    {f_ldeorl_4, 2, 4, "ldeorl"},
    {f_ldeorl_8, 2, 8, "ldeorl"},
    {f_ldeoralb_1, 2, 1, "ldeoralb"},
    {f_ldeoralh_2, 2, 2, "ldeoralh"},
    {f_ldeoral_4, 2, 4, "ldeoral"},
    {f_ldeoral_8, 2, 8, "ldeoral"},
    {f_ldsetb_1, 3, 1, "ldsetb"},
    {f_ldseth_2, 3, 2, "ldseth"},
    {f_ldset_4, 3, 4, "ldset"},
    {f_ldset_8, 3, 8, "ldset"},
    {f_ldsetab_1, 3, 1, "ldsetab"},
    {f_ldsetah_2, 3, 2, "ldsetah"},
    {f_ldseta_4, 3, 4, "ldseta"},
    {f_ldseta_8, 3, 8, "ldseta"},
    {f_ldsetlb_1, 3, 1, "ldsetlb"},
    {f_ldsetlh_2, 3, 2, "ldsetlh"},
    {f_ldsetl_4, 3, 4, "ldsetl"},
    {f_ldsetl_8, 3, 8, "ldsetl"},
    {f_ldsetalb_1, 3, 1, "ldsetalb"},
    {f_ldsetalh_2, 3, 2, "ldsetalh"},
    {f_ldsetal_4, 3, 4, "ldsetal"},
    {f_ldsetal_8, 3, 8, "ldsetal"},
    {f_ldsmaxb_1, 4, 1, "ldsmaxb"},
    {f_ldsmaxh_2, 4, 2, "ldsmaxh"},
    {f_ldsmax_4, 4, 4, "ldsmax"},
    {f_ldsmax_8, 4, 8, "ldsmax"},
    {f_ldsmaxab_1, 4, 1, "ldsmaxab"},
    {f_ldsmaxah_2, 4, 2, "ldsmaxah"},
    {f_ldsmaxa_4, 4, 4, "ldsmaxa"},
    {f_ldsmaxa_8, 4, 8, "ldsmaxa"},
    {f_ldsmaxlb_1, 4, 1, "ldsmaxlb"},
    {f_ldsmaxlh_2, 4, 2, "ldsmaxlh"},
    {f_ldsmaxl_4, 4, 4, "ldsmaxl"},
    {f_ldsmaxl_8, 4, 8, "ldsmaxl"},
    {f_ldsmaxalb_1, 4, 1, "ldsmaxalb"},
    {f_ldsmaxalh_2, 4, 2, "ldsmaxalh"},
    {f_ldsmaxal_4, 4, 4, "ldsmaxal"},
    {f_ldsmaxal_8, 4, 8, "ldsmaxal"},
    {f_ldsminb_1, 5, 1, "ldsminb"},
    {f_ldsminh_2, 5, 2, "ldsminh"},
    {f_ldsmin_4, 5, 4, "ldsmin"},
    {f_ldsmin_8, 5, 8, "ldsmin"},
    {f_ldsminab_1, 5, 1, "ldsminab"},
    {f_ldsminah_2, 5, 2, "ldsminah"},
    {f_ldsmina_4, 5, 4, "ldsmina"},
    {f_ldsmina_8, 5, 8, "ldsmina"},
    {f_ldsminlb_1, 5, 1, "ldsminlb"},
    {f_ldsminlh_2, 5, 2, "ldsminlh"},
    {f_ldsminl_4, 5, 4, "ldsminl"},
    {f_ldsminl_8, 5, 8, "ldsminl"},
    {f_ldsminalb_1, 5, 1, "ldsminalb"},
    {f_ldsminalh_2, 5, 2, "ldsminalh"},
    {f_ldsminal_4, 5, 4, "ldsminal"},
    {f_ldsminal_8, 5, 8, "ldsminal"},
    {f_ldumaxb_1, 6, 1, "ldumaxb"},
    {f_ldumaxh_2, 6, 2, "ldumaxh"},
    {f_ldumax_4, 6, 4, "ldumax"},
    {f_ldumax_8, 6, 8, "ldumax"},
    {f_ldumaxab_1, 6, 1, "ldumaxab"},
    {f_ldumaxah_2, 6, 2, "ldumaxah"},
    {f_ldumaxa_4, 6, 4, "ldumaxa"},
    {f_ldumaxa_8, 6, 8, "ldumaxa"},
    {f_ldumaxlb_1, 6, 1, "ldumaxlb"},
    {f_ldumaxlh_2, 6, 2, "ldumaxlh"},
    {f_ldumaxl_4, 6, 4, "ldumaxl"},
    {f_ldumaxl_8, 6, 8, "ldumaxl"},
    {f_ldumaxalb_1, 6, 1, "ldumaxalb"},
    {f_ldumaxalh_2, 6, 2, "ldumaxalh"},
    {f_ldumaxal_4, 6, 4, "ldumaxal"},
    {f_ldumaxal_8, 6, 8, "ldumaxal"},
    {f_lduminb_1, 7, 1, "lduminb"},
    {f_lduminh_2, 7, 2, "lduminh"},
    {f_ldumin_4, 7, 4, "ldumin"},
    {f_ldumin_8, 7, 8, "ldumin"},
    {f_lduminab_1, 7, 1, "lduminab"},
    {f_lduminah_2, 7, 2, "lduminah"},
    {f_ldumina_4, 7, 4, "ldumina"},
    {f_ldumina_8, 7, 8, "ldumina"},
    {f_lduminlb_1, 7, 1, "lduminlb"},
    {f_lduminlh_2, 7, 2, "lduminlh"},
    {f_lduminl_4, 7, 4, "lduminl"},
    {f_lduminl_8, 7, 8, "lduminl"},
    {f_lduminalb_1, 7, 1, "lduminalb"},
    {f_lduminalh_2, 7, 2, "lduminalh"},
    {f_lduminal_4, 7, 4, "lduminal"},
    {f_lduminal_8, 7, 8, "lduminal"},
    {f_swpb_1, 8, 1, "swpb"},
    {f_swph_2, 8, 2, "swph"},
    {f_swp_4, 8, 4, "swp"},
    {f_swp_8, 8, 8, "swp"},
    {f_swpab_1, 8, 1, "swpab"},
    {f_swpah_2, 8, 2, "swpah"},
    {f_swpa_4, 8, 4, "swpa"},
    {f_swpa_8, 8, 8, "swpa"},
    {f_swplb_1, 8, 1, "swplb"},
    {f_swplh_2, 8, 2, "swplh"},
    {f_swpl_4, 8, 4, "swpl"},
    {f_swpl_8, 8, 8, "swpl"},
    {f_swpalb_1, 8, 1, "swpalb"},
    {f_swpalh_2, 8, 2, "swpalh"},
    {f_swpal_4, 8, 4, "swpal"},
    {f_swpal_8, 8, 8, "swpal"},
};
LSE static uint64_t c_casb_1(void *p, uint64_t exp, uint64_t des) { __asm__ volatile("casb %w0, %w1, [%2]" : "+r"(exp) : "r"(des), "r"(p) : "memory"); return exp; }
LSE static uint64_t c_cash_2(void *p, uint64_t exp, uint64_t des) { __asm__ volatile("cash %w0, %w1, [%2]" : "+r"(exp) : "r"(des), "r"(p) : "memory"); return exp; }
LSE static uint64_t c_cas_4(void *p, uint64_t exp, uint64_t des) { __asm__ volatile("cas %w0, %w1, [%2]" : "+r"(exp) : "r"(des), "r"(p) : "memory"); return exp; }
LSE static uint64_t c_cas_8(void *p, uint64_t exp, uint64_t des) { __asm__ volatile("cas %x0, %x1, [%2]" : "+r"(exp) : "r"(des), "r"(p) : "memory"); return exp; }
LSE static uint64_t c_casab_1(void *p, uint64_t exp, uint64_t des) { __asm__ volatile("casab %w0, %w1, [%2]" : "+r"(exp) : "r"(des), "r"(p) : "memory"); return exp; }
LSE static uint64_t c_casah_2(void *p, uint64_t exp, uint64_t des) { __asm__ volatile("casah %w0, %w1, [%2]" : "+r"(exp) : "r"(des), "r"(p) : "memory"); return exp; }
LSE static uint64_t c_casa_4(void *p, uint64_t exp, uint64_t des) { __asm__ volatile("casa %w0, %w1, [%2]" : "+r"(exp) : "r"(des), "r"(p) : "memory"); return exp; }
LSE static uint64_t c_casa_8(void *p, uint64_t exp, uint64_t des) { __asm__ volatile("casa %x0, %x1, [%2]" : "+r"(exp) : "r"(des), "r"(p) : "memory"); return exp; }
LSE static uint64_t c_caslb_1(void *p, uint64_t exp, uint64_t des) { __asm__ volatile("caslb %w0, %w1, [%2]" : "+r"(exp) : "r"(des), "r"(p) : "memory"); return exp; }
LSE static uint64_t c_caslh_2(void *p, uint64_t exp, uint64_t des) { __asm__ volatile("caslh %w0, %w1, [%2]" : "+r"(exp) : "r"(des), "r"(p) : "memory"); return exp; }
LSE static uint64_t c_casl_4(void *p, uint64_t exp, uint64_t des) { __asm__ volatile("casl %w0, %w1, [%2]" : "+r"(exp) : "r"(des), "r"(p) : "memory"); return exp; }
LSE static uint64_t c_casl_8(void *p, uint64_t exp, uint64_t des) { __asm__ volatile("casl %x0, %x1, [%2]" : "+r"(exp) : "r"(des), "r"(p) : "memory"); return exp; }
LSE static uint64_t c_casalb_1(void *p, uint64_t exp, uint64_t des) { __asm__ volatile("casalb %w0, %w1, [%2]" : "+r"(exp) : "r"(des), "r"(p) : "memory"); return exp; }
LSE static uint64_t c_casalh_2(void *p, uint64_t exp, uint64_t des) { __asm__ volatile("casalh %w0, %w1, [%2]" : "+r"(exp) : "r"(des), "r"(p) : "memory"); return exp; }
LSE static uint64_t c_casal_4(void *p, uint64_t exp, uint64_t des) { __asm__ volatile("casal %w0, %w1, [%2]" : "+r"(exp) : "r"(des), "r"(p) : "memory"); return exp; }
LSE static uint64_t c_casal_8(void *p, uint64_t exp, uint64_t des) { __asm__ volatile("casal %x0, %x1, [%2]" : "+r"(exp) : "r"(des), "r"(p) : "memory"); return exp; }
struct cas { uint64_t (*fn)(void *, uint64_t, uint64_t); int sz; const char *name; };
static const struct cas cass[] = {
    {c_casb_1, 1, "casb"},
    {c_cash_2, 2, "cash"},
    {c_cas_4, 4, "cas"},
    {c_cas_8, 8, "cas"},
    {c_casab_1, 1, "casab"},
    {c_casah_2, 2, "casah"},
    {c_casa_4, 4, "casa"},
    {c_casa_8, 8, "casa"},
    {c_caslb_1, 1, "caslb"},
    {c_caslh_2, 2, "caslh"},
    {c_casl_4, 4, "casl"},
    {c_casl_8, 8, "casl"},
    {c_casalb_1, 1, "casalb"},
    {c_casalh_2, 2, "casalh"},
    {c_casal_4, 4, "casal"},
    {c_casal_8, 8, "casal"},
};

// CASP: Rs/Rs+1 expected in, the old pair out; Rt/Rt+1 desired
LSE static void casp_x(void *p, uint64_t *e0, uint64_t *e1, uint64_t d0, uint64_t d1) {
    register uint64_t x0 __asm__("x0") = *e0, x1 __asm__("x1") = *e1, x2 __asm__("x2") = d0, x3 __asm__("x3") = d1;
    __asm__ volatile("caspal x0, x1, x2, x3, [%4]" : "+r"(x0), "+r"(x1) : "r"(x2), "r"(x3), "r"(p) : "memory");
    *e0 = x0;
    *e1 = x1;
}
LSE static void casp_w(void *p, uint64_t *e0, uint64_t *e1, uint64_t d0, uint64_t d1) {
    register uint64_t x0 __asm__("x0") = *e0, x1 __asm__("x1") = *e1, x2 __asm__("x2") = d0, x3 __asm__("x3") = d1;
    __asm__ volatile("casp w0, w1, w2, w3, [%4]" : "+r"(x0), "+r"(x1) : "r"(x2), "r"(x3), "r"(p) : "memory");
    *e0 = x0;
    *e1 = x1;
}
// LDXP then STXP of new values: status 0 if stored
static uint64_t ldxp_stxp(uint64_t *p, uint64_t *lo, uint64_t *hi, uint64_t n0, uint64_t n1) {
    uint64_t a, b, st;
    __asm__ volatile("ldaxp %0, %1, [%3]\n stlxp %w2, %4, %5, [%3]" : "=&r"(a), "=&r"(b), "=&r"(st) : "r"(p), "r"(n0), "r"(n1) : "memory");
    *lo = a;
    *hi = b;
    return st;
}
static uint64_t ldxp_stxp_w(uint32_t *p, uint64_t *lo, uint64_t *hi, uint64_t n0, uint64_t n1) {
    uint32_t a, b;
    uint64_t st;
    __asm__ volatile("ldxp %w0, %w1, [%3]\n stxp %w2, %w4, %w5, [%3]" : "=&r"(a), "=&r"(b), "=&r"(st) : "r"(p), "r"(n0), "r"(n1) : "memory");
    *lo = a;
    *hi = b;
    return st;
}

static uint64_t pick(void) { return rnd() % 3 == 0 ? edge[rnd() % NEDGE] : rnd(); }

static void single(void) {
    static uint64_t cell[4] __attribute__((aligned(16)));
    for (unsigned r = 0; r < sizeof(rmws) / sizeof(rmws[0]); r++) {
        for (int i = 0; i < 60; i++) {
            uint64_t old = pick(), arg = pick(), neigh = rnd();
            int sz = rmws[r].sz;
            cell[0] = neigh;
            cell[1] = neigh ^ 0x5555;
            memcpy(cell, &old, (size_t) sz);
            uint64_t got = rmws[r].fn(cell, arg);
            report(rmws[r].name, sz, got, old & mask_of(sz));
            uint64_t now = 0, want = model(rmws[r].op, old, arg, sz);
            memcpy(&now, cell, (size_t) sz);
            report(rmws[r].name, sz, now, want);
            uint64_t rest = 0, wrest = 0;  // the bytes past the operand untouched
            memcpy(&rest, (char *) cell + sz, (size_t) (8 - (sz == 8 ? 0 : sz)));
            memcpy(&wrest, (char *) &neigh + sz, (size_t) (8 - (sz == 8 ? 0 : sz)));
            if (sz < 8)
                report("bytes past the operand", sz, rest, wrest);
        }
    }
    for (unsigned c = 0; c < sizeof(cass) / sizeof(cass[0]); c++) {
        for (int i = 0; i < 80; i++) {
            int sz = cass[c].sz;
            uint64_t old = pick(), des = pick(), exp = i & 1 ? old : pick();
            if (i % 4 == 3) // equal in the low bytes, different above them
                exp = (old & mask_of(sz)) | (sz < 8 ? rnd() << (8 * sz) : 0);
            cell[0] = rnd();
            memcpy(cell, &old, (size_t) sz);
            uint64_t got = cass[c].fn(cell, exp, des), now = 0;
            memcpy(&now, cell, (size_t) sz);
            report(cass[c].name, sz, got, old & mask_of(sz));
            int match = (exp & mask_of(sz)) == (old & mask_of(sz));
            report(cass[c].name, sz, now, match ? des & mask_of(sz) : old & mask_of(sz));
        }
    }
    for (int i = 0; i < 200; i++) {
        uint64_t o0 = pick(), o1 = pick(), d0 = pick(), d1 = pick();
        int match = i & 1;
        uint64_t e0 = match ? o0 : (i & 2 ? pick() : o0), e1 = match ? o1 : (i & 2 ? o1 : pick());
        if (!match && e0 == o0 && e1 == o1)
            e1 ^= 1;
        cell[0] = o0; cell[1] = o1;
        casp_x(cell, &e0, &e1, d0, d1);
        report("caspal x", 16, e0, o0); report("caspal x hi", 16, e1, o1);
        report("caspal x mem", 16, cell[0], match ? d0 : o0); report("caspal x mem hi", 16, cell[1], match ? d1 : o1);
        uint32_t *w = (uint32_t *) cell;
        w[0] = (uint32_t) o0; w[1] = (uint32_t) o1;
        e0 = match ? (uint32_t) o0 : (uint32_t) (o0 ^ 1); e1 = (uint32_t) o1;
        casp_w(cell, &e0, &e1, d0, d1);
        report("casp w", 8, e0, (uint32_t) o0); report("casp w hi", 8, e1, (uint32_t) o1);
        report("casp w mem", 8, w[0], match ? (uint32_t) d0 : (uint32_t) o0);
        report("casp w mem hi", 8, w[1], match ? (uint32_t) d1 : (uint32_t) o1);
        uint64_t lo, hi;
        cell[0] = o0; cell[1] = o1;
        uint64_t st = ldxp_stxp(cell, &lo, &hi, d0, d1);
        report("ldaxp", 16, lo, o0); report("ldaxp hi", 16, hi, o1);
        report("stlxp status", 16, st, 0); report("stlxp mem", 16, cell[0], d0); report("stlxp mem hi", 16, cell[1], d1);
        w[0] = (uint32_t) o0; w[1] = (uint32_t) o1;
        st = ldxp_stxp_w(w, &lo, &hi, d0, d1);
        report("ldxp w", 8, lo, (uint32_t) o0); report("ldxp w hi", 8, hi, (uint32_t) o1);
        report("stxp w status", 8, st, 0); report("stxp w mem", 8, w[0], (uint32_t) d0); report("stxp w mem hi", 8, w[1], (uint32_t) d1);
    }
}

// four threads: LDADD and a CAS loop must lose nothing
static uint64_t counter_add, counter_cas;
#define ITER 20000
LSE static void *worker(void *arg) {
    (void) arg;
    for (int i = 0; i < ITER; i++) {
        f_ldaddal_8(&counter_add, 1);
        uint64_t seen = __atomic_load_n(&counter_cas, __ATOMIC_RELAXED);
        for (;;) {
            uint64_t got = c_casal_8(&counter_cas, seen, seen + 3);
            if (got == seen)
                break;
            seen = got;
        }
    }
    return NULL;
}
static void threads(void) {
    pthread_t t[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, worker, NULL);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    report("ldaddal from 4 threads", 8, counter_add, 4ull * ITER);
    report("casal loop from 4 threads", 8, counter_cas, 3ull * 4 * ITER);
}

static sigjmp_buf jb;
static void on_segv(int sig) { siglongjmp(jb, sig); }
// the signal it takes, or 0
static int faults(void (*fn)(void *), void *p) {
    int sig = sigsetjmp(jb, 1);
    if (sig == 0) {
        fn(p);
        return 0;
    }
    return sig;
}
LSE static void ldadd_at(void *p) { f_ldadd_4(p, 1); }
LSE static void cas_at(void *p) { c_cas_8(p, 0, 1); }
static void ldxp_at(void *p) { uint64_t a, b; __asm__ volatile("ldxp %0, %1, [%2]\n clrex" : "=&r"(a), "=&r"(b) : "r"(p) : "memory"); }
static void fault_cases(void) {
    struct sigaction sa = {0};
    sa.sa_handler = on_segv;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    char *pg = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    report("misaligned ldadd: SIGBUS", 4, (uint64_t) faults(ldadd_at, pg + 2), SIGBUS);
    munmap(pg + 4096, 4096);
    report("ldadd on an unmapped page: SIGSEGV", 4, (uint64_t) faults(ldadd_at, pg + 4096), SIGSEGV);
    mprotect(pg, 4096, PROT_READ);
    report("cas on a read-only page: SIGSEGV", 8, (uint64_t) faults(cas_at, pg), SIGSEGV);
    report("ldxp on a read-only page: no fault", 16, (uint64_t) faults(ldxp_at, pg), 0);
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
}

int main(void) {
    single();
    threads();
    fault_cases();
    printf("arm64_lse_gadgets: %s (%lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", checks, bad);
    return bad != 0;
}

