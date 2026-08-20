#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/personality.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
#undef PAGE_SIZE // want definition from emu/memory.h
#include "../misc.h"

long trycall(long res, const char *msg) {
    if (res == -1 && errno != 0) {
        perror(msg); printf("\r\n"); exit(1);
    }
    return res;
}

int start_tracee(int at, const char *path, char *const argv[], char *const envp[]) {
    // shut off aslr
    int persona = personality(0xffffffff);
    persona |= ADDR_NO_RANDOMIZE;
    personality(persona);

    int pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        // child
        // enable segfaulting on rdtsc and cpuid
        trycall(prctl(PR_SET_TSC, PR_TSC_SIGSEGV), "rdtsc faulting");
        trycall(ptrace(PTRACE_TRACEME, 0, NULL, NULL), "ptrace traceme");
        // get rid of signal handlers
        for (int sig = 0; sig < SIGRTMIN - 1; sig++)
            signal(sig, SIG_DFL);
        trycall(fexecve(openat(at, path, O_RDONLY), argv, envp), "execve");
    } else {
        // parent, wait for child to stop after exec.
        //
        // waitpid on the tracee specifically, not wait(). A bare wait() reaps
        // whichever child happens to be ready, so any other child of this
        // process exiting at the wrong moment was consumed here instead --
        // leaving the real tracee unwaited and this check looking at a status
        // that belongs to something else. That is issue #541's "tracee reaped
        // during setup": the reap was real, the pid was not the tracee's.
        int status;
        trycall(waitpid(pid, &status, 0), "waitpid tracee");
        if (!WIFSTOPPED(status)) {
            if (WIFEXITED(status))
                fprintf(stderr, "tracee exited during setup with status %d "
                        "(exec of the guest binary probably failed)\n",
                        WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                fprintf(stderr, "tracee died during setup on signal %d (%s)\n",
                        WTERMSIG(status), strsignal(WTERMSIG(status)));
            else
                fprintf(stderr, "tracee failed to start (status %#x)\n", status);
            exit(1);
        }
    }
    return pid;
}

int open_mem(int pid) {
    char filename[1024];
    snprintf(filename, sizeof(filename), "/proc/%d/mem", pid);
    return trycall(open(filename, O_RDWR), "open mem");
}

void pt_readn(int pid, addr_t addr, void *buf, size_t count) {
    int fd = open_mem(pid);
    trycall(lseek(fd, addr, SEEK_SET), "read seek");
    trycall(read(fd, buf, count), "read read");
    close(fd);
}

void pt_writen(int pid, addr_t addr, void *buf, size_t count) {
    int fd = open_mem(pid);
    trycall(lseek(fd, addr, SEEK_SET), "write seek");
    trycall(write(fd, buf, count), "write write");
    close(fd);
}

dword_t pt_read(int pid, addr_t addr) {
    dword_t res;
    pt_readn(pid, addr, &res, sizeof(res));
    return res;
}

void pt_write(int pid, addr_t addr, dword_t val) {
    pt_writen(pid, addr, &val, sizeof(val));
}

void pt_write8(int pid, addr_t addr, byte_t val) {
    pt_writen(pid, addr, &val, sizeof(val));
}
