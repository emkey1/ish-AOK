import re

with open("jit/gen.c", "r") as f:
    content = f.read()

# Fix gen_amd64_raise_gadget to only declare and use gadget_amd64_raise if __aarch64__
search = """static void gen_amd64_raise_gadget(struct gen_state *state, int interrupt, guest_addr_t rip) {
    extern void gadget_amd64_raise(void);
    gen(state, (unsigned long) gadget_amd64_raise);
    gen(state, (unsigned long) interrupt);
    gen(state, (unsigned long) rip);
}"""

replace = """static void gen_amd64_raise_gadget(struct gen_state *state, int interrupt, guest_addr_t rip) {
#if defined(__aarch64__)
    extern void gadget_amd64_raise(void);
    gen(state, (unsigned long) gadget_amd64_raise);
    gen(state, (unsigned long) interrupt);
    gen(state, (unsigned long) rip);
#else
    (void) state;
    (void) interrupt;
    (void) rip;
    assert(!"gadget_amd64_raise is only available on __aarch64__");
#endif
}"""

if search in content:
    with open("jit/gen.c", "w") as f:
        f.write(content.replace(search, replace))
    print("Patched jit/gen.c successfully")
else:
    print("Search string not found")
