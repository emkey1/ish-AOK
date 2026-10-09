// anon_inode_names.c -- what readlink(/proc/self/fd/N) says for the anonymous
// inode family. Linux prints the name each anon_inode_getfd caller passed,
// brackets included or not: "anon_inode:[eventfd]" but "anon_inode:inotify".
// AOK bracketed every class that did not spell its own, so inotify read
// "anon_inode:[inotify]"; lsof and similar readers parse these. Checked on
// camd (Linux 6.12); the fscontext line only where fsopen is permitted.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <unistd.h>

static int failures, checks;

static void expect(const char *what, int fd, const char *want) {
    checks++;
    char link[64], text[256] = "";
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = fd >= 0 ? readlink(link, text, sizeof text - 1) : -1;
    if (n >= 0)
        text[n] = '\0';
    if (fd < 0 || n < 0 || strcmp(text, want) != 0) {
        failures++;
        printf("FAIL %s: \"%s\" (want \"%s\")%s%s\n", what, text, want, fd < 0 ? ": " : "",
               fd < 0 ? strerror(errno) : "");
    }
}

int main(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    expect("eventfd", eventfd(0, 0), "anon_inode:[eventfd]");
    expect("epoll", epoll_create1(0), "anon_inode:[eventpoll]");
    expect("inotify", inotify_init1(0), "anon_inode:inotify");
    expect("signalfd", signalfd(-1, &set, 0), "anon_inode:[signalfd]");
    expect("timerfd", timerfd_create(CLOCK_MONOTONIC, 0), "anon_inode:[timerfd]");
    expect("pidfd", (int) syscall(SYS_pidfd_open, getpid(), 0), "anon_inode:[pidfd]");
    expect("memfd", memfd_create("an", 0), "/memfd:an (deleted)");
    int fs = (int) syscall(SYS_fsopen, "tmpfs", 0);
    if (fs >= 0)
        expect("fscontext", fs, "anon_inode:[fscontext]");
    printf("anon_inode_names: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
