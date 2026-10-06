// x86_signal_entry_flags.c -- a signal handler starts with DF clear, as
// Linux's handle_signal makes it (x86_signal_handler_flags in kernel/
// signal.c), and the interrupted code gets its DF back on return. A handler
// entered with DF set ran memcpy and friends' string instructions backwards.
// i386 and amd64.
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile unsigned long handler_flags, after_flags;
static unsigned long flags(void) {
    unsigned long f;
    __asm__ volatile("pushf\n pop %0" : "=r"(f));
    return f;
}
static volatile char copy_dst[32];
static void on_usr1(int sig) {
    (void) sig;
    handler_flags = flags();
    // a forward rep movsb must copy forward here
    static const char src[] = "forward";
    void *d = (void *) copy_dst;
    const void *s = src;
    unsigned long n = sizeof(src);
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
}

int main(void) {
    int bad = 0;
    signal(SIGUSR1, on_usr1);
    __asm__ volatile("std" ::: "cc");
    raise(SIGUSR1);
    __asm__ volatile("pushf\n pop %0\n cld" : "=r"(after_flags) :: "cc");
    if (handler_flags & 0x400) { printf("FAIL DF set in the handler\n"); bad++; }
    if (strcmp((const char *) copy_dst, "forward") != 0) { printf("FAIL rep movsb in the handler\n"); bad++; }
    if (!(after_flags & 0x400)) { printf("FAIL DF not restored after the handler\n"); bad++; }
    if (handler_flags & 0x100) { printf("FAIL TF set in the handler\n"); bad++; }
    printf("x86_signal_entry_flags: %s\n", bad ? "FAIL" : "PASS");
    return bad != 0;
}
