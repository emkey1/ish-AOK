// Where a signal aimed at a PROCESS actually lands, and what a shared
// pending queue must preserve.
//
// This is a guard rather than a fix: all fifteen cases already matched Linux
// when measured, several of them because of the group-signal work earlier in
// this release (kill() finding a live member when the leader is a corpse, and
// process-directed delivery choosing a thread that has the signal unblocked).
// They are pinned here because the answers are not obvious from the code and
// were only settled by running the same program under both kernels.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <time.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "test_common.h"

static void chk(const char *w, long got, long want) {
    if (got != want)
        failf(w, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-50s got=%ld want=%ld\n", w, got, want);
}
static void note(const char *w, long got) { test_logf("  %-50s got=%ld\n", w, got); }
static pid_t gettid_(void) { return (pid_t) syscall(SYS_gettid); }
static void nap(long ms) { struct timespec t = { ms/1000, (ms%1000)*1000000L }; nanosleep(&t, NULL); }

// ---- 1. a blocked process-directed signal stays pending and shows in
//         sigpending(), then arrives when unblocked --------------------
static volatile sig_atomic_t got_usr1;
static void onusr1(int s){(void)s; got_usr1++;}
static void case_shared_pending(void) {
    struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_handler=onusr1;
    sigaction(SIGUSR1,&sa,NULL);
    sigset_t set, pend;
    sigemptyset(&set); sigaddset(&set,SIGUSR1);
    pthread_sigmask(SIG_BLOCK,&set,NULL);
    got_usr1 = 0;
    kill(getpid(), SIGUSR1);            // process-directed, all threads block it
    nap(150);
    sigemptyset(&pend);
    sigpending(&pend);
    chk("blocked process-directed shows in sigpending", sigismember(&pend,SIGUSR1), 1);
    chk("  and has not run yet", (long) got_usr1, 0);
    pthread_sigmask(SIG_UNBLOCK,&set,NULL);
    nap(150);
    chk("delivered on unblock", (long) got_usr1, 1);
}

// ---- 2. standard signals do not queue; realtime ones do ---------------
static volatile sig_atomic_t rt_count, std_count;
static void onrt(int s){(void)s; rt_count++;}
static void onstd(int s){(void)s; std_count++;}
static void case_queueing(void) {
    struct sigaction sa; memset(&sa,0,sizeof sa);
    sa.sa_handler=onrt; sigaction(SIGRTMIN,&sa,NULL);
    sa.sa_handler=onstd; sigaction(SIGUSR2,&sa,NULL);
    sigset_t set;
    sigemptyset(&set); sigaddset(&set,SIGRTMIN); sigaddset(&set,SIGUSR2);
    pthread_sigmask(SIG_BLOCK,&set,NULL);
    rt_count = std_count = 0;
    for (int i = 0; i < 5; i++) {
        union sigval v; v.sival_int = i;
        sigqueue(getpid(), SIGRTMIN, v);
        kill(getpid(), SIGUSR2);
    }
    nap(100);
    pthread_sigmask(SIG_UNBLOCK,&set,NULL);
    nap(250);
    chk("5 queued realtime signals all delivered", (long) rt_count, 5);
    chk("5 standard signals collapse to one", (long) std_count, 1);
}

// ---- 3. sigqueue carries its sival_int ---------------------------------
static volatile sig_atomic_t seen_val;
static void onrt2(int s, siginfo_t *si, void *u){(void)s;(void)u; seen_val = si->si_value.sival_int;}
static void case_sigval(void) {
    struct sigaction sa; memset(&sa,0,sizeof sa);
    sa.sa_sigaction = onrt2; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGRTMIN+1,&sa,NULL);
    seen_val = 0;
    union sigval v; v.sival_int = 0x2bad;
    sigqueue(getpid(), SIGRTMIN+1, v);
    nap(200);
    chk("sigqueue delivers sival_int", (long) seen_val, 0x2bad);
}

// ---- 4. a POSIX timer's SIGEV_SIGNAL is process-directed ---------------
//         Created on a NON-leader thread: Linux still lets any thread with
//         the signal unblocked take it, and si_code is SI_TIMER.
static volatile sig_atomic_t timer_tid, timer_code;
static void ontimer(int s, siginfo_t *si, void *u){(void)s;(void)u;
    timer_tid = (sig_atomic_t) gettid_(); timer_code = si->si_code;}
static volatile int worker_armed;
static timer_t made;
static void *maker(void *arg) {
    (void) arg;
    struct sigevent sev; memset(&sev,0,sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGRTMIN+2;
    if (timer_create(CLOCK_MONOTONIC,&sev,&made) < 0) { worker_armed = -1; return NULL; }
    struct itimerspec its; memset(&its,0,sizeof its);
    its.it_value.tv_nsec = 300000000L;         // 300ms
    if (timer_settime(made,0,&its,NULL) < 0) { worker_armed = -1; return NULL; }
    worker_armed = 1;
    for (int i=0;i<100 && timer_tid==0;i++) nap(25);
    return NULL;
}
static void case_timer_sigev(void) {
    struct sigaction sa; memset(&sa,0,sizeof sa);
    sa.sa_sigaction = ontimer; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGRTMIN+2,&sa,NULL);
    timer_tid = 0; timer_code = 0; worker_armed = 0;
    pthread_t t; pthread_create(&t,NULL,maker,NULL);
    for (int i=0;i<200 && worker_armed==0;i++) nap(10);
    if (worker_armed < 0) { note("timer_create on a thread failed", -1); pthread_join(t,NULL); return; }
    for (int i=0;i<200 && timer_tid==0;i++) nap(25);
    pthread_join(t,NULL);
    chk("timer SIGEV_SIGNAL was delivered", timer_tid != 0, 1);
    chk("  si_code is SI_TIMER", (long) timer_code, SI_TIMER);
    timer_delete(made);
}

// ---- 5. tgkill targets one thread; kill targets the process ------------
static volatile sig_atomic_t tk_tid;
static void ontk(int s){(void)s; tk_tid = (sig_atomic_t) gettid_();}
static volatile pid_t worker_tid;
static volatile int tk_ready, tk_done;
static void *tkworker(void *arg){(void)arg;
    worker_tid = gettid_(); tk_ready = 1;
    for (int i=0;i<200 && !tk_done;i++) nap(25);
    return NULL;}
static void case_tgkill(void) {
    struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_handler=ontk;
    sigaction(SIGUSR1,&sa,NULL);
    tk_tid = 0; tk_ready = 0; tk_done = 0; worker_tid = 0;
    pthread_t t; pthread_create(&t,NULL,tkworker,NULL);
    for (int i=0;i<200 && !tk_ready;i++) nap(10);
    syscall(SYS_tgkill, getpid(), (int) worker_tid, SIGUSR1);
    for (int i=0;i<200 && tk_tid==0;i++) nap(25);
    tk_done = 1;
    pthread_join(t,NULL);
    chk("tgkill runs the handler on the named thread",
        tk_tid == (sig_atomic_t) worker_tid, 1);
}

// ---- SIGEV_THREAD_ID must land on exactly the named thread -------------
static volatile sig_atomic_t tid_hit;
static void onsig(int s, siginfo_t *si, void *u){(void)s;(void)si;(void)u; tid_hit=(sig_atomic_t)gettid_();}
static volatile pid_t target_tid;
static volatile int ready, done;
static void *target_thread(void *arg){(void)arg;
    target_tid = gettid_(); ready = 1;
    for(int i=0;i<300 && !done;i++) nap(20);
    return NULL;}

static void case_thread_id(void) {
    struct sigaction sa; memset(&sa,0,sizeof sa);
    sa.sa_sigaction=onsig; sa.sa_flags=SA_SIGINFO;
    sigaction(SIGRTMIN+3,&sa,NULL);
    tid_hit=0; ready=0; done=0; target_tid=0;
    pthread_t t; pthread_create(&t,NULL,target_thread,NULL);
    for(int i=0;i<300 && !ready;i++) nap(10);

    // The leader blocks it, so only the named thread can take it.
    sigset_t set; sigemptyset(&set); sigaddset(&set,SIGRTMIN+3);
    pthread_sigmask(SIG_BLOCK,&set,NULL);

    struct sigevent sev; memset(&sev,0,sizeof sev);
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGRTMIN+3;
#ifdef sigev_notify_thread_id
    sev.sigev_notify_thread_id = target_tid;
#else
    // glibc calls it sev._sigev_un._tid, musl sev.sigev_notify_thread_id
    // (a macro over __sev_fields). Both put it at the same offset as
    // sigev_value/sigev_signo + 4 words, which is what the kernel reads.
    memcpy((char *) &sev + offsetof(struct sigevent, sigev_notify) + sizeof(int),
           &(int){ (int) target_tid }, sizeof(int));
#endif
    timer_t tid;
    if (timer_create(CLOCK_MONOTONIC,&sev,&tid) < 0) {
        printf("  SIGEV_THREAD_ID: SKIP (timer_create: %s)\n", strerror(errno));
        done=1; pthread_join(t,NULL);
        pthread_sigmask(SIG_UNBLOCK,&set,NULL);
        return;
    }
    struct itimerspec its; memset(&its,0,sizeof its);
    its.it_value.tv_nsec = 300000000L;
    timer_settime(tid,0,&its,NULL);
    for(int i=0;i<300 && tid_hit==0;i++) nap(20);
    done=1; pthread_join(t,NULL);
    chk("SIGEV_THREAD_ID fired", tid_hit != 0, 1);
    chk("  on the named thread", tid_hit == (sig_atomic_t) target_tid, 1);
    timer_delete(tid);
    pthread_sigmask(SIG_UNBLOCK,&set,NULL);
}

// ---- a timer created by a thread that blocks the signal ---------------
static volatile sig_atomic_t blk_hit;
static void onblk(int s){(void)s; blk_hit=(sig_atomic_t)gettid_();}
static volatile int mk_ready, mk_done;
static timer_t blk_timer;
static void *blocking_maker(void *arg){
    (void) arg;
    // This thread blocks the signal, so a process-directed notification has
    // to be taken by the leader instead.
    sigset_t set; sigemptyset(&set); sigaddset(&set,SIGRTMIN+4);
    pthread_sigmask(SIG_BLOCK,&set,NULL);
    struct sigevent sev; memset(&sev,0,sizeof sev);
    sev.sigev_notify=SIGEV_SIGNAL; sev.sigev_signo=SIGRTMIN+4;
    if (timer_create(CLOCK_MONOTONIC,&sev,&blk_timer) < 0) { mk_ready=-1; return NULL; }
    struct itimerspec its; memset(&its,0,sizeof its);
    its.it_value.tv_nsec=300000000L;
    timer_settime(blk_timer,0,&its,NULL);
    mk_ready=1;
    for(int i=0;i<300 && !mk_done;i++) nap(20);
    return NULL;}
static void case_blocked_creator(void) {
    struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_handler=onblk;
    sigaction(SIGRTMIN+4,&sa,NULL);
    blk_hit=0; mk_ready=0; mk_done=0;
    pthread_t t; pthread_create(&t,NULL,blocking_maker,NULL);
    for(int i=0;i<300 && mk_ready==0;i++) nap(10);
    if (mk_ready<0){ printf("  blocked-creator: SKIP\n"); mk_done=1; pthread_join(t,NULL); return; }
    int leader = (int) gettid_();
    for(int i=0;i<300 && blk_hit==0;i++) nap(20);
    mk_done=1; pthread_join(t,NULL);
    chk("timer signal blocked in its creator still fires", blk_hit != 0, 1);
    chk("  taken by the leader instead", blk_hit == (sig_atomic_t) leader, 1);
    timer_delete(blk_timer);
}

// ---- kill() to a process whose leader has exited ----------------------
static void *survivor(void *arg){(void)arg; for(int i=0;i<400;i++) nap(50); return NULL;}
static void case_dead_leader(void) {
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        pthread_t t; pthread_create(&t,NULL,survivor,NULL);
        nap(200);
        pthread_exit(NULL);         // leader becomes a zombie, worker lives on
    }
    nap(700);
    errno = 0;
    int r = kill(c, SIGTERM);
    chk("kill() a process whose leader has exited: rc", r, 0);
    int st=0;
    int reaped = 0;
    for (int i=0;i<60;i++){ if (waitpid(c,&st,WNOHANG)==c){reaped=1;break;} nap(100); }
    chk("  the process actually died", reaped && WIFSIGNALED(st), 1);
    if (!reaped) { kill(c,SIGKILL); waitpid(c,&st,0); }
}


int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(180));
    case_shared_pending();
    case_queueing();
    case_sigval();
    case_timer_sigev();
    case_tgkill();
    case_thread_id();
    case_blocked_creator();
    case_dead_leader();
    return finish_suite("signal_process_target");
}
