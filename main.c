#include <stdlib.h>
#include <string.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "xX_main_Xx.h"

#include <stdio.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include "util/list.h"

#include <mach/task.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>

static void handler(int signo, siginfo_t *sigaction, void *context) {
    printk("ERROR: in exception handler.\n");
    signal(signo, SIG_DFL);
}

static void gen_exception(void) {
    printk("WARNING: gen_exception in.\n");
    *(int *)0 = 0;
    printk("WARNING: gen_exception out.\n");
}

void *gen_exception_thread(void *parg) {
    gen_exception();
    return 0;
}

int main(int argc, char *const argv[]) {
    char *envp = malloc(1);
    if (envp) {
        envp[0] = '\0';
    }
    if (getenv("TERM")) {
        const char *term = getenv("TERM");
        size_t len = strlen("TERM=") + strlen(term) + 2;
        char *new_envp = malloc(len);
        if (new_envp) {
            snprintf(new_envp, len, "TERM=%s", term);
            new_envp[len - 1] = '\0';
            if (envp) free(envp);
            envp = new_envp;
        }
    }
    int err = xX_main_Xx(argc, argv, envp);
    if (envp)
        free(envp);
    if (err < 0) {
        fprintf(stderr, "xX_main_Xx: %s\n", strerror(-err));
        return err;
    }
    do_mount(&procfs, "proc", "/proc", "", 0);
    do_mount(&devptsfs, "devpts", " ", "", 0);
    
    task_set_exception_ports(
                             mach_task_self(),
                             EXC_MASK_BAD_ACCESS,
                             MACH_PORT_NULL,//m_exception_port,
                             EXCEPTION_DEFAULT,
                             0);
    
    
    struct sigaction sa;
    sa.sa_sigaction = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;

    if(sigaction(/*SIGBUS*/SIGSEGV, &sa, NULL) == -1) {
        printf("sigaction fails.\n");
        return 0;
    }

    pthread_t id;
    pthread_create(&id, NULL, gen_exception_thread, NULL);
    pthread_join(id, NULL);
    
    task_run_current();
}
