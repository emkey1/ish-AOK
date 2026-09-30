// Linux 6.12 on camd (AMD x86-64), identical for -m32 and -m64: a misaligned
// 128-bit legacy-SSE memory operand is #GP(0) -- SIGSEGV, SI_KERNEL, trap 13,
// error 0, at the instruction -- except for the unaligned forms and the
// narrower operands, which run (the controls, from "movaps load ok" on).
static const struct result expected[] = {
    { 11, 128, 13, 0, 0 },  // movaps load mis
    { 11, 128, 13, 0, 0 },  // movaps store mis
    { 11, 128, 13, 0, 0 },  // movdqa load mis
    { 11, 128, 13, 0, 0 },  // movdqa store mis
    { 11, 128, 13, 0, 0 },  // movapd load mis
    { 11, 128, 13, 0, 0 },  // movntps mis
    { 11, 128, 13, 0, 0 },  // movntdq mis
    { 11, 128, 13, 0, 0 },  // addps mem mis
    { 11, 128, 13, 0, 0 },  // pxor mem mis
    { 11, 128, 13, 0, 0 },  // pshufd mem mis
    { 11, 128, 13, 0, 0 },  // paddd mem mis
    { 11, 128, 13, 0, 0 },  // pcmpeqb mem mis
    { 11, 128, 13, 0, 0 },  // punpcklbw mis
    { 11, 128, 13, 0, 0 },  // andps mem mis
    { 0, 0, 0, 0, 0 },  // movaps load ok
    { 0, 0, 0, 0, 0 },  // movdqa store ok
    { 0, 0, 0, 0, 0 },  // pshufd mem ok
    { 0, 0, 0, 0, 0 },  // movups load mis
    { 0, 0, 0, 0, 0 },  // movups store mis
    { 0, 0, 0, 0, 0 },  // movdqu load mis
    { 0, 0, 0, 0, 0 },  // movdqu store mis
    { 0, 0, 0, 0, 0 },  // lddqu mis
    { 0, 0, 0, 0, 0 },  // pcmpistri mis
    { 0, 0, 0, 0, 0 },  // insertps m32 mis
    { 0, 0, 0, 0, 0 },  // addss mem mis
    { 0, 0, 0, 0, 0 },  // movsd load mis
};
