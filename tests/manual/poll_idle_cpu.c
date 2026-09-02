/* A blocking poll() on a quiet socket must not burn CPU.
 *
 * This is a COST test, not a correctness test, and that distinction is the
 * whole point: every functional poll/select test in the suite passed while an
 * idle poll() spun a host core at 97%. The guest-visible answer was right --
 * poll returned 0 after exactly the timeout it was given -- so nothing that
 * checked the answer could see it. What it cost to produce that answer is the
 * only thing that shows the bug.
 *
 * On device it looked like this: an idle chronyd, an idle rsyslogd and an idle
 * sshd-session each pinned a core, reported "S" (sleeping) in ps, and issued no
 * syscalls at all while doing it -- because the spin was inside the emulator's
 * own poll_wait, not in guest code. The cause was the kqueue write filter,
 * which is registered for hangup detection even when the guest only asked to
 * read: it is level-triggered, a quiet socket is always writable, so poll_wait
 * woke immediately, found nothing the guest wanted, and slept again, forever.
 * (fs/poll.c real_poll_update; the fix is EV_CLEAR on a hangup-only filter.)
 *
 * The three socket kinds here are the three that were seen spinning: a unix
 * socketpair (rsyslogd's /dev/log), UDP (chronyd) and TCP (sshd-session).
 *
 * The 25% threshold is deliberately loose. A correct implementation measures
 * ~0%, a spinning one ~95%, and nothing lands in between; a wide band keeps
 * this from flaking on a loaded machine.
 *
 * The last two checks are the other half: a filter that never fires would also
 * score 0% CPU, so readiness and hangup must still wake the poll promptly.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include "test_common.h"
#include <netinet/in.h>

static double cpu_now(void) {
    struct rusage r; getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec + r.ru_utime.tv_usec/1e6
         + r.ru_stime.tv_sec + r.ru_stime.tv_usec/1e6;
}
static double wall_now(void) {
    struct timeval t; gettimeofday(&t, NULL);
    return t.tv_sec + t.tv_usec/1e6;
}


static void idle_cost(const char *name, int fd, int ms) {
    double c0 = cpu_now(), w0 = wall_now();
    struct pollfd p = { .fd = fd, .events = POLLIN };
    int r = poll(&p, 1, ms);
    double cpu = cpu_now() - c0, wall = wall_now() - w0;
    double pct = wall > 0 ? cpu * 100.0 / wall : 0;
    printf("%-20s poll(%dms) -> %d  cpu=%.3fs wall=%.3fs  = %.0f%% CPU %s\n",
           name, ms, r, cpu, wall, pct, pct > 25 ? "<-- SPINNING" : "ok");
    if (pct > 25) failures_total++;
    if (r != 0) { printf("   (expected timeout, got %d revents=0x%x)\n", r, p.revents); failures_total++; }
}

static void *writer(void *a) { int fd = *(int*)a; usleep(200000); write(fd, "x", 1); return 0; }
static void *closer(void *a) { int fd = *(int*)a; usleep(200000); close(fd); return 0; }

int main(int argc, char **argv) {
    test_init(argc, argv);
    /* 1. idle cost on a socketpair */
    { int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
      idle_cost("socketpair", sv[0], 600); close(sv[0]); close(sv[1]); }
    /* 2. idle cost on a TCP connection */
    { int l = socket(AF_INET, SOCK_STREAM, 0);
      struct sockaddr_in a = {.sin_family=AF_INET, .sin_addr={htonl(INADDR_LOOPBACK)}};
      bind(l,(struct sockaddr*)&a,sizeof a); listen(l,1);
      socklen_t al=sizeof a; getsockname(l,(struct sockaddr*)&a,&al);
      int c = socket(AF_INET, SOCK_STREAM, 0);
      connect(c,(struct sockaddr*)&a,sizeof a);
      int s = accept(l,NULL,NULL);
      idle_cost("tcp", c, 600); close(c); close(s); close(l); }
    /* 3. idle cost on a UDP socket */
    { int fd = socket(AF_INET, SOCK_DGRAM, 0);
      struct sockaddr_in a = {.sin_family=AF_INET, .sin_addr={htonl(INADDR_LOOPBACK)}};
      bind(fd,(struct sockaddr*)&a,sizeof a);
      idle_cost("udp", fd, 600); close(fd); }

    /* 4. readiness still wakes it promptly */
    { int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
      pthread_t t; pthread_create(&t,0,writer,&sv[1]);
      double w0 = wall_now();
      struct pollfd p = { .fd = sv[0], .events = POLLIN };
      int r = poll(&p,1,5000);
      double w = wall_now()-w0;
      printf("%-20s wake-on-data -> %d revents=0x%x after %.3fs %s\n",
             "socketpair", r, p.revents, w, (r==1 && w<1.0) ? "ok" : "<-- BAD");
      if (!(r==1 && w<1.0)) failures_total++;
      pthread_join(t,0); close(sv[0]); close(sv[1]); }

    /* 5. hangup still wakes it */
    { int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
      pthread_t t; pthread_create(&t,0,closer,&sv[1]);
      double w0 = wall_now();
      struct pollfd p = { .fd = sv[0], .events = POLLIN };
      int r = poll(&p,1,5000);
      double w = wall_now()-w0;
      printf("%-20s wake-on-hangup -> %d revents=0x%x after %.3fs %s\n",
             "socketpair", r, p.revents, w, (r>=1 && w<1.0) ? "ok" : "<-- BAD");
      if (!(r>=1 && w<1.0)) failures_total++;
      pthread_join(t,0); close(sv[0]); }

    return finish_suite("poll_idle_cpu");
}
