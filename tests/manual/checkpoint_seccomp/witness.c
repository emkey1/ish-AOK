// The process checkpoint_seccomp.sh saves and restores: sandboxed with a
// seccomp filter (getppid -> EPERM) that a second thread shares, and
// undumpable. It reports the same facts before the save and after the
// restore, and at the end installs a filter with TSYNC, which works only if
// the two threads still share their filters rather than holding equal copies.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct sf { unsigned short code; unsigned char jt, jf; unsigned int k; };
struct sfprog { unsigned short len; struct sf *filter; };
#ifndef SYS_seccomp
# if defined(__x86_64__)
#  define SYS_seccomp 317
# elif defined(__i386__)
#  define SYS_seccomp 354
# else
#  define SYS_seccomp 277
# endif
#endif

static int install(unsigned nr, unsigned action, unsigned flags) {
    struct sf f[] = {
        {0x20, 0, 0, 0},            // A = nr
        {0x15, 0, 1, nr},           // if A == nr
        {0x06, 0, 0, action},       //   return action
        {0x06, 0, 0, 0x7fff0000u},  // return ALLOW
    };
    struct sfprog p = {4, f};
    return (int) syscall(SYS_seccomp, 1, flags, &p);
}

static int status_field(const char *name) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    int v = -1;
    size_t n = strlen(name);
    while (f != NULL && fgets(line, sizeof line, f) != NULL)
        if (strncmp(line, name, n) == 0 && line[n] == ':')
            v = atoi(line + n + 1);
    if (f != NULL)
        fclose(f);
    return v;
}

static void *idle(void *arg) {
    (void) arg;
    for (;;)
        pause();
    return NULL;
}

static void show(const char *when) {
    errno = 0;
    long r = syscall(SYS_getppid);
    int e = r < 0 ? errno : 0;
    printf("%s seccomp=%d filters=%d getppid_errno=%d dumpable=%d\n", when,
           status_field("Seccomp"), status_field("Seccomp_filters"), e,
           prctl(PR_GET_DUMPABLE, 0, 0, 0, 0));
    fflush(stdout);
}

int main(void) {
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    if (install(SYS_getppid, 0x00050000u | EPERM, 0) != 0) {
        printf("install failed errno=%d\n", errno);
        return 1;
    }
    pthread_t t;
    pthread_create(&t, NULL, idle, NULL);
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    show("BEFORE");
    // Long enough for the save to land in the middle, in small sleeps so the
    // freeze parks this thread in one.
    for (int i = 0; i < 60; i++) {
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
    show("AFTER");
    int r = install(SYS_getuid, 0x00050000u | EPERM, 1 /* TSYNC */);
    printf("TSYNC %d\n", r);
    fflush(stdout);
    return 0;
}
