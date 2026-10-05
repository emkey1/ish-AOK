// i386_sse_moves.c -- random runs of the SSE moves AOK's i386 JIT does
// natively (jit/gen.c gen_vec: movd between xmm and a general register or
// memory, movq/movsd/movss loads and stores, movq xmm to xmm, punpckldq,
// pshufd, pxor, paddq, psrlq/psllq by an immediate)
// mixed with plain register moves, then every general register, xmm0-7 and
// the memory they touched, printed. Run it under two builds (or on Linux)
// and diff the output; the memory operands include unaligned ones and ones
// that cross a page.
//
//     gcc -m32 -O1 -o i386_sse_moves i386_sse_moves.c   (in an i386 guest: gcc -O1)

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

struct state {
    uint32_t r[8];        // eax ecx edx ebx (esp unused) ebp esi edi
    uint8_t xmm[8][16];
};

static uint32_t rng = 0x9e3779b9u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static uint8_t *p;
static void b(uint8_t v) { *p++ = v; }
static void d32(uint32_t v) { memcpy(p, &v, 4); p += 4; }

static uint8_t *page; // two pages; operands land near the boundary
static const int gprs[] = {0, 1, 2, 3, 6, 7}; // eax ecx edx ebx esi edi (ebp holds the state)

static void mem_operand(int reg) { // [abs32] near the page boundary: modrm mod=00 rm=101
    uint32_t off = 4096 - 12 + rnd() % 24;
    b((uint8_t) (0x05 | (reg & 7) << 3));
    d32((uint32_t) (uintptr_t) (page + off));
}

static void emit_one(void) {
    int x = (int) (rnd() % 8), y = (int) (rnd() % 8), g = gprs[rnd() % 6];
    switch (rnd() % 16) {
        case 12: b(0x66); b(0x0f); b(0x70); b((uint8_t) (0xc0 | x << 3 | y)); b((uint8_t) rnd()); break; // pshufd
        case 13: b(0x66); b(0x0f); b(rnd() & 1 ? 0xef : 0xd4); b((uint8_t) (0xc0 | x << 3 | y)); break; // pxor/paddq
        case 14: case 15: { // psrlq/psllq xmm, imm8: counts 0, 1, 63, 64 and past
            static const uint8_t cnt[] = {0, 1, 7, 32, 63, 64, 65, 127, 128, 200, 255};
            b(0x66); b(0x0f); b(0x73); b((uint8_t) (0xc0 | (rnd() & 1 ? 2 : 6) << 3 | x));
            b(cnt[rnd() % 11]); break;
        }
        case 0: b(0x66); b(0x0f); b(0x6e); b((uint8_t) (0xc0 | x << 3 | g)); break;   // movd xmm, r32
        case 1: b(0x66); b(0x0f); b(0x7e); b((uint8_t) (0xc0 | x << 3 | g)); break;   // movd r32, xmm
        case 2: b(0x66); b(0x0f); b(0x6e); mem_operand(x); break;                      // movd xmm, m32
        case 3: b(0x66); b(0x0f); b(0x7e); mem_operand(x); break;                      // movd m32, xmm
        case 4: b(0xf3); b(0x0f); b(0x7e); mem_operand(x); break;                      // movq xmm, m64
        case 5: b(0x66); b(0x0f); b(0xd6); mem_operand(x); break;                      // movq m64, xmm
        case 6: b(0xf2); b(0x0f); b((uint8_t) (rnd() & 1 ? 0x10 : 0x11)); mem_operand(x); break; // movsd
        case 7: b(0xf3); b(0x0f); b((uint8_t) (rnd() & 1 ? 0x10 : 0x11)); mem_operand(x); break; // movss
        case 8: b(0xf3); b(0x0f); b(0x7e); b((uint8_t) (0xc0 | x << 3 | y)); break;   // movq xmm, xmm
        case 9: b(0x66); b(0x0f); b(0xd6); b((uint8_t) (0xc0 | x << 3 | y)); break;   // movq xmm, xmm (D6)
        case 10: b(0x66); b(0x0f); b(0x62); b((uint8_t) (0xc0 | x << 3 | y)); break;  // punpckldq
        default: b(0x89); b((uint8_t) (0xc0 | gprs[rnd() % 6] << 3 | g)); break;     // mov r32, r32
    }
}

#define NSEQ 2000
static void (*seqs[NSEQ])(struct state *);

int main(void) {
    uint8_t *code = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    page = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED || page == MAP_FAILED) { perror("mmap"); return 1; }
    p = code;
    for (int s = 0; s < NSEQ; s++) {
        seqs[s] = (void (*)(struct state *)) p;
        b(0x55); b(0x53); b(0x56); b(0x57);              // push ebp ebx esi edi
        b(0x8b); b(0x6c); b(0x24); b(0x14);              // mov ebp, [esp + 20] (the state)
        for (int i = 0; i < 8; i++) if (i != 4 && i != 5) { b(0x8b); b((uint8_t) (0x45 | i << 3)); b((uint8_t) (4 * i)); }
        for (int i = 0; i < 8; i++) { b(0x0f); b(0x10); b((uint8_t) (0x85 | i << 3)); d32((uint32_t) (32 + 16 * i)); } // movups xmm, [ebp + ..]
        int n = 4 + (int) (rnd() % 16);
        for (int i = 0; i < n; i++) emit_one();
        for (int i = 0; i < 8; i++) if (i != 4 && i != 5) { b(0x89); b((uint8_t) (0x45 | i << 3)); b((uint8_t) (4 * i)); }
        for (int i = 0; i < 8; i++) { b(0x0f); b(0x11); b((uint8_t) (0x85 | i << 3)); d32((uint32_t) (32 + 16 * i)); } // movups [ebp + ..], xmm
        b(0x5f); b(0x5e); b(0x5b); b(0x5d); b(0xc3);     // pop edi esi ebx ebp; ret
    }
    for (int s = 0; s < NSEQ; s++) {
        struct state st;
        for (int i = 0; i < 8; i++) st.r[i] = rnd();
        for (int i = 0; i < 8; i++) for (int j = 0; j < 16; j++) st.xmm[i][j] = (uint8_t) rnd();
        for (int i = 4096 - 16; i < 4096 + 24; i++) page[i] = (uint8_t) rnd();
        seqs[s](&st);
        printf("%d", s);
        for (int i = 0; i < 8; i++) if (i != 4 && i != 5) printf(" %08x", st.r[i]);
        for (int i = 0; i < 8; i++) { printf(" "); for (int j = 15; j >= 0; j--) printf("%02x", st.xmm[i][j]); }
        printf(" m ");
        for (int i = 4096 - 16; i < 4096 + 24; i++) printf("%02x", page[i]);
        printf("\n");
    }
    return 0;
}
