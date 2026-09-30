// Linux 6.12 on camd (AMD x86-64), identical for -m32 and -m64: every
// privileged instruction is #GP(0) -- SIGSEGV, SI_KERNEL, trap 13, error 0,
// reported at the instruction.
static const struct result expected[] = {
    { 11, 128, 13, 0, 0 },  // clts
    { 11, 128, 13, 0, 0 },  // invd
    { 11, 128, 13, 0, 0 },  // wbinvd
    { 11, 128, 13, 0, 0 },  // mov eax,cr0
    { 11, 128, 13, 0, 0 },  // mov cr0,eax
    { 11, 128, 13, 0, 0 },  // mov eax,dr0
    { 11, 128, 13, 0, 0 },  // mov dr0,eax
    { 11, 128, 13, 0, 0 },  // wrmsr
    { 11, 128, 13, 0, 0 },  // rdmsr
    { 11, 128, 13, 0, 0 },  // rdpmc
    { 11, 128, 13, 0, 0 },  // lgdt [eax]
    { 11, 128, 13, 0, 0 },  // lidt [eax]
    { 11, 128, 13, 0, 0 },  // lmsw ax
    { 11, 128, 13, 0, 0 },  // invlpg [eax]
    { 11, 128, 13, 0, 0 },  // xsetbv
    { 11, 128, 13, 0, 0 },  // lldt ax
    { 11, 128, 13, 0, 0 },  // ltr ax
    { 11, 128, 13, 0, 0 },  // hlt
    { 11, 128, 13, 0, 0 },  // cli
    { 11, 128, 13, 0, 0 },  // sti
};
