
static uint8_t *counter;
static void *worker(void *arg) {
    (void) arg;
    for (int i = 0; i < 20000; i++) {
        uint32_t one = 1;
        __asm__ volatile("lock addl %0, (%1)\n lock adcl $0, (%1)" : "+r"(one) : "r"(counter) : "cc", "memory");
    }
    return NULL;
}
static sigjmp_buf jb;
static volatile int f_sig;
static void on_ill(int sig) { f_sig = sig; siglongjmp(jb, 1); }

int main(void) {
    uint8_t *page = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *edge = page + 4096;
    static const int offs[] = {0, 1, 3, 6, 7};
    for (int k = 0; k < 400; k++) {
        for (int fi = 0; fi < NFORMS; fi++) {
            const struct form *f = &forms[fi];
            int bytes = f->bits / 8;
            uint8_t *p = (k & 1) ? edge - bytes + offs[k % 5] % bytes : page + 64 + offs[k % 5];
            if (f->lock && ((uintptr_t) p & 4095) + bytes > 4096) p = page + 128 + offs[k % 5];
            uint64_t mem = rnd(), reg = rnd(), fl;
            if (k % 7 == 0) reg = mem;                 // equal operands
            if (k % 11 == 0) mem = 0;
            int cin = (k >> 1) & 1;
            memcpy(p, &mem, 8);
            uint64_t reg_in = reg;
            f->fn(p, &reg, &fl, cin);
            uint64_t m = f->bits == 64 ? ~0ull : (1ull << f->bits) - 1, cur, wf;
            memcpy(&cur, p, 8);
            uint64_t rhs = f->kind == 2 ? (uint64_t) f->imm : reg_in;
            uint64_t lhs = f->kind == 1 ? reg_in : mem;
            if (f->kind == 1) rhs = mem;
            uint64_t res = model(f->op, f->bits, lhs, rhs, cin, &wf);
            check(f->name, fi * 1000 + k, fl & ARITH, wf);
            int writes = f->op != 7 && f->op != 8;
            if (f->kind == 1) {
                uint64_t want = !writes ? reg_in : f->bits == 32 ? res : f->bits == 64 ? res : (reg_in & ~m) | res;
                check(f->name, fi * 1000 + k, reg, want);
                check(f->name, fi * 1000 + k, cur, mem);
            } else {
                check(f->name, fi * 1000 + k, cur, writes ? (mem & ~m) | res : mem);
                check(f->name, fi * 1000 + k, reg, reg_in);
            }
        }
    }
    // register-register forms (op 9 is MOV: no flags)
    for (int k = 0; k < 600; k++) {
        for (int fi = 0; fi < NRR; fi++) {
            const struct rrform *f = &rrforms[fi];
            uint64_t c = rnd(), d = rnd(), fl, wf = 0;
            if (k % 7 == 0) d = c;
            int cin = k & 1;
            uint64_t c0 = c, d0 = d;
            f->fn(&c, &d, &fl, cin);
            uint64_t a = f->dst_hi ? c0 >> 8 : c0, b = f->src_hi ? d0 >> 8 : d0;
            uint64_t m = f->bits == 64 ? ~0ull : (1ull << f->bits) - 1, res;
            if (f->op == 9) res = b & m;
            else res = model(f->op, f->bits, a, b, cin, &wf);
            if (f->op != 9) check(f->name, fi * 1000 + k, fl & ARITH, wf);
            uint64_t want = c0;
            if (f->op != 7 && f->op != 8) {
                if (f->bits == 8) want = f->dst_hi ? (c0 & ~0xff00ull) | (res << 8) : (c0 & ~0xffull) | res;
                else if (f->bits == 16) want = (c0 & ~0xffffull) | res;
                else want = res;
            }
            check(f->name, fi * 1000 + k, c, want);
            check(f->name, fi * 1000 + k, d, d0);
        }
    }
    // high-byte registers: add %ah, (mem) and add (mem), %bh
    {
        uint8_t b = 0x10;
        uint64_t rax = 0x1111110522ull;
        __asm__ volatile("addb %%ah, (%1)" : "+a"(rax) : "S"(&b) : "cc", "memory");
        check("addb %ah, (mem)", 0, b, 0x15);
        uint64_t rbx = 0x3300ull;
        uint8_t c = 0x04;
        __asm__ volatile("addb (%1), %%bh" : "+b"(rbx) : "S"(&c) : "cc", "memory");
        check("addb (mem), %bh", 0, rbx, 0x3700);
    }
    // LOCK contention, aligned and misaligned (adc with the carry the add leaves: 0)
    static const int coffs[] = {0, 1, 13};
    for (int c = 0; c < 3; c++) {
        counter = page + 512 + coffs[c];
        memset(counter, 0, 4);
        pthread_t t[4];
        for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, NULL);
        for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
        uint32_t v;
        memcpy(&v, counter, 4);
        check("4 threads lock add/adc: no lost update", coffs[c], v, 80000);
    }
    // LOCK CMP is #UD
    signal(SIGILL, on_ill);
    f_sig = 0;
    uint32_t z = 0;
    if (!sigsetjmp(jb, 1))
        __asm__ volatile(".byte 0xf0, 0x83, 0x3f, 0x00" :: "D"(&z) : "cc", "memory");   // lock cmpl $0, (%rdi)
    check("lock cmp: SIGILL", 0, f_sig, SIGILL);
    printf("amd64_alu_mem: %s (%d forms, %lu checks, %lu mismatches)\n", bad ? "FAIL" : "PASS", NFORMS + NRR, checks, bad);
    return bad != 0;
}
