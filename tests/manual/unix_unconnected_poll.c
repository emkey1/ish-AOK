// unix_unconnected_poll.c -- poll() on a unix stream socket with no live peer
// never blocks. Linux reports POLLIN|POLLHUP once a connected peer is gone,
// and POLLOUT|POLLHUP for one that is not connected at all (never connected,
// only bound, or after a failed connect); a listener reports nothing. Darwin
// reports nothing for the unconnected kinds, and labwc waited on one for ever
// after Xwayland crashed on the 5th-gen iPad, freezing the desktop.
//
//     gcc -O1 -o unix_unconnected_poll unix_unconnected_poll.c
//
// Oracle: Linux 6.12 (camd) passes every case.
// and the same with the end closed in-process. Also an unconnected socket.
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int fails;
static void check(const char *name, int fd) {
    struct pollfd p = {fd, POLLIN, 0};
    double t0 = now();
    int r = poll(&p, 1, 2000);
    double dt = now() - t0;
    char b; ssize_t n = r > 0 ? read(fd, &b, 1) : -2;
    int ok = r == 1 && (p.revents & POLLHUP) && dt < 1.0;
    printf("%-34s poll=%d revents=%#x%s%s read=%zd %.2fs %s\n", name, r, p.revents,
           p.revents & POLLIN ? " IN" : "", p.revents & POLLHUP ? " HUP" : "", n, dt, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}
static void child_case(const char *name, int how) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pid_t c = fork();
    if (c == 0) {
        close(sv[0]);
        usleep(100000);
        if (how == 1) abort();
        if (how == 2) { char fd[16]; snprintf(fd, sizeof fd, "%d", sv[1]);
            execl("/bin/sh", "sh", "-c", "sleep 0.1; kill -ABRT $$", (char *) 0); }
        _exit(0);
    }
    close(sv[1]);
    int st; waitpid(c, &st, 0);
    check(name, sv[0]);
    close(sv[0]);
}
int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv); close(sv[1]); check("socketpair, peer closed here", sv[0]); close(sv[0]);
    child_case("socketpair, child exits", 0);
    child_case("socketpair, child aborts", 1);
    child_case("socketpair, child execs then SIGABRT", 2);
    // Waiting first, then the peer dies: the wake path, not a fresh look.
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pid_t c = fork();
    if (c == 0) { close(sv[0]); usleep(300000); abort(); }
    close(sv[1]);
    check("socketpair, peer dies during poll", sv[0]);
    waitpid(c, NULL, 0); close(sv[0]);
    int u = socket(AF_UNIX, SOCK_STREAM, 0); check("never connected", u); close(u);
    u = socket(AF_UNIX, SOCK_SEQPACKET, 0); check("seqpacket, never connected", u); close(u);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "/tmp/peerpoll-%d", getpid());
    u = socket(AF_UNIX, SOCK_STREAM, 0); bind(u, (void *) &a, sizeof a);
    check("bound, not connected", u);
    // A listener with nobody connecting has nothing to report, on Linux too.
    listen(u, 1);
    struct pollfd p = {u, POLLIN | POLLOUT, 0};
    int r = poll(&p, 1, 200);
    printf("%-34s poll=%d revents=%#x %s\n", "listening, idle", r, p.revents, r == 0 ? "ok" : "FAIL");
    if (r != 0) fails++;
    close(u); unlink(a.sun_path);
    // A connect that failed leaves the socket unconnected.
    u = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(u, (void *) &a, sizeof a) == 0) { printf("connect to a removed path succeeded\n"); fails++; }
    check("after a failed connect", u); close(u);
    printf(fails ? "FAIL: %d\n" : "PASS\n", fails);
    return fails != 0;
}
