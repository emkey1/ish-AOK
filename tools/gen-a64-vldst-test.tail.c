
static unsigned long checks, bad;
static void mismatch(const struct form *f, const char *what, long where) {
    if (bad++ < 30)
        printf("FAIL %s: %s (at %ld)\n", f->name, what, where);
}

static int esize(const struct form *f) { return 1 << f->esz; }
static int regbytes(const struct form *f) { return f->q ? 16 : 8; }
static int total(const struct form *f) {
    return f->kind <= 1 ? f->count * regbytes(f) : f->count * esize(f);
}

// The Arm ARM, per form: kind 0 LD1/ST1 multiple (registers consecutive in
// memory), 1 LD2-4/ST2-4 (element e of register r at (e * count + r)),
// 2 one lane per register (the rest of a loaded register kept), 3 LD1R-4R
// (one element per register, replicated). A 64-bit arrangement's load
// clears the upper half.
static void model_load(const struct form *f, const uint8_t *img, const v128 *init, v128 *exp) {
    int es = esize(f), rb = regbytes(f), n = f->count;
    for (int r = 0; r < n; r++) {
        v128 v;
        memset(&v, 0, sizeof(v));
        switch (f->kind) {
        case 0: memcpy(v.b, img + r * rb, rb); break;
        case 1:
            for (int e = 0; e < rb / es; e++)
                memcpy(v.b + e * es, img + (e * n + r) * es, es);
            break;
        case 2: v = init[r]; memcpy(v.b + f->lane * es, img + r * es, es); break;
        case 3:
            for (int e = 0; e < rb / es; e++)
                memcpy(v.b + e * es, img + r * es, es);
            break;
        }
        exp[r] = v;
    }
}
static void model_store(const struct form *f, const v128 *regs, uint8_t *img) {
    int es = esize(f), rb = regbytes(f), n = f->count;
    for (int r = 0; r < n; r++) {
        switch (f->kind) {
        case 0: memcpy(img + r * rb, regs[r].b, rb); break;
        case 1:
            for (int e = 0; e < rb / es; e++)
                memcpy(img + (e * n + r) * es, regs[r].b + e * es, es);
            break;
        case 2: memcpy(img + r * es, regs[r].b + f->lane * es, es); break;
        }
    }
}

#define PG 4096
#define REGION (4 * PG)
static uint8_t before[REGION];

static void run_at(const struct form *f, int fi, uint8_t *region, uint8_t *base, unsigned seed) {
    for (int i = 0; i < REGION; i++)
        region[i] = (uint8_t) (i * 13 + seed);
    memcpy(before, region, REGION);
    v128 init[4], out[4], exp[4];
    for (int r = 0; r < 4; r++)
        for (int b = 0; b < 16; b++)
            init[r].b[b] = (uint8_t) ((r * 16 + b) * 29 + fi + seed);
    memset(out, 0xee, sizeof(out));
    uint64_t wb = 0;
    f->fn(base, init, out, &wb);
    long off = base - region;
    int n = f->count, t = total(f);
    if (f->load) {
        model_load(f, before + off, init, exp);
        for (int r = 0; r < n; r++, checks++)
            if (memcmp(&out[r], &exp[r], 16))
                mismatch(f, "register", off);
        checks++;
        if (memcmp(region, before, REGION))
            mismatch(f, "a load wrote memory", off);
    } else {
        uint8_t *want = malloc(REGION);
        memcpy(want, before, REGION);
        model_store(f, init, want + off);
        checks++;
        if (memcmp(region, want, REGION)) {
            int i = 0;
            while (region[i] == want[i])
                i++;
            mismatch(f, "memory", i - off);
        }
        free(want);
        for (int r = 0; r < n; r++, checks++)
            if (memcmp(&out[r], &init[r], 16))
                mismatch(f, "a store changed a register", off);
    }
    uint64_t want_wb = (uint64_t) base + (f->wb == 1 ? t : f->wb == 2 ? RM_VAL : 0);
    checks++;
    if (wb != want_wb)
        mismatch(f, "writeback", off);
}

static void placements(void) {
    uint8_t *region = mmap(NULL, REGION, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *edge = region + 2 * PG;
    for (int fi = 0; fi < NFORMS; fi++) {
        const struct form *f = &forms[fi];
        int t = total(f);
        int ks[] = {t, 1, t - 1, t / 2, 3, t - 3, 7};
        for (unsigned k = 0; k < sizeof(ks) / sizeof(ks[0]); k++)
            if (ks[k] >= 1 && ks[k] <= t)
                run_at(f, fi, region, edge - ks[k], k);
        run_at(f, fi, region, region + PG + 100, 9);     // mid-page
        run_at(f, fi, region, region + PG, 10);          // a page's start
        // a fresh mapping: the first access misses the TLB
        uint8_t *fresh = mmap(NULL, REGION, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        run_at(f, fi, fresh, fresh + 2 * PG - (t > 1 ? t / 2 : 0), 11);
        munmap(fresh, REGION);
    }
    munmap(region, REGION);
}

#if defined(__linux__)
static sigjmp_buf jb;
static volatile uintptr_t f_pc, f_addr, f_x9;
static volatile int f_sig;
static v128 f_v[32];
static int f_have_v;
static void on_fault(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    f_sig = sig;
    f_addr = (uintptr_t) si->si_addr;
    f_pc = uc->uc_mcontext.pc;
    f_x9 = uc->uc_mcontext.regs[9];
    const uint8_t *res = (const uint8_t *) &uc->uc_mcontext.__reserved;
    uint32_t magic;
    memcpy(&magic, res, 4);
    f_have_v = magic == 0x46508001;   // FPSIMD_MAGIC: vregs follow at +16
    if (f_have_v)
        memcpy(f_v, res + 16, sizeof(f_v));
    siglongjmp(jb, 1);
}

static void fault_at(const struct form *f, int fi, int k) {
    uint8_t *m = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (int i = 0; i < PG; i++)
        m[i] = (uint8_t) (i * 7 + 1);
    memcpy(before, m, PG);
    mprotect(m + PG, PG, f->load ? PROT_NONE : PROT_READ);
    uint8_t *base = m + PG - k;
    v128 init[4], out[4];
    for (int r = 0; r < 4; r++)
        for (int b = 0; b < 16; b++)
            init[r].b[b] = (uint8_t) ((r * 16 + b) * 31 + fi);
    uint64_t wb = 0;
    f_sig = 0;
    if (!sigsetjmp(jb, 1))
        f->fn(base, init, out, &wb);
    int t = total(f);
    checks++;
    if (f_sig != SIGSEGV) {
        mismatch(f, "no SIGSEGV", -k);
        goto done;
    }
    checks += 4;
    if (f_pc != (uintptr_t) f->pc)
        mismatch(f, "fault pc", -k);
    if (f_x9 != (uintptr_t) base)
        mismatch(f, "fault: base written back", -k);
    if (f_addr < (uintptr_t) (m + PG) || f_addr >= (uintptr_t) (m + PG + t))
        mismatch(f, "fault address", -k);
    if (!f_have_v)
        mismatch(f, "no fpsimd context", -k);
    else
        for (int r = 0; r < f->count; r++, checks++)
            if (memcmp(&f_v[(f->rt + r) % 32], &init[r], 16))
                mismatch(f, "fault: a register changed", -k);
    if (!f->load) {
        // bytes before the bad page: old, or what the store writes there
        uint8_t img[64];
        memcpy(img, base, k);
        model_store(f, init, img);
        for (int i = 0; i < k; i++, checks++)
            if (base[i] != before[PG - k + i] && base[i] != img[i])
                mismatch(f, "fault: a stray byte", i);
    }
done:
    munmap(m, 2 * PG);
}

static void faults(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    for (int fi = 0; fi < NFORMS; fi++) {
        const struct form *f = &forms[fi];
        int t = total(f);
        if (t > 1)
            fault_at(f, fi, t / 2);        // crossing into the bad page
        fault_at(f, fi, 0);                // wholly in it
    }
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
}
#endif

int main(void) {
    placements();
#if defined(__linux__)
    faults();
#endif
    printf("arm64_vldst_gadgets: %s (%d forms, %lu checks, %lu mismatches)\n",
           bad ? "FAIL" : "PASS", NFORMS, checks, bad);
    return bad != 0;
}
