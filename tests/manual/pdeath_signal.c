/*
 * pdeath_signal.c -- PR_SET_PDEATHSIG: the signal a process asked for when its
 * parent dies is sent when Linux sends it, and forgotten when Linux forgets it.
 *
 * AOK stored the setting (kernel/misc.c), returned it for PR_GET_PDEATHSIG and
 * carried it through a checkpoint, and nothing ever sent it. A helper that
 * asked to go with the program that started it -- Go's SysProcAttr.Pdeathsig,
 * systemd's FORK_DEATHSIG -- outlived it. The prctl took any number, and no
 * credential change or exec forgot it.
 *
 * Linux sends it from forget_original_parent, for every thread of every child
 * process a dying THREAD hands on, whoever takes it: group_send_sig_info(
 * t->pdeath_signal, SEND_SIG_NOINFO, t, PIDTYPE_TGID) -- process-directed,
 * SI_USER, naming the exiting process, and subject to the permission check
 * kill() makes. So a child forked by a worker thread gets it when that worker
 * exits, though its process lives on (the man page's warning), and it gets it
 * again at each later reparent. A thread's own setting is for its PROCESS's
 * parent: no thread is anyone's child on Linux, where AOK makes a thread the
 * child of the thread that created it -- so the exit of that creator must send
 * nothing.
 *
 * It is forgotten by fork (in the child), by a change of the effective or
 * filesystem ids -- commit_creds, the real and saved ids and the groups not
 * counting -- and by an exec that changes them or is secure; any other exec
 * keeps it.
 *
 * The scenarios, each in a process of its own that is a subreaper, so the
 * orphans come back to it; the process that asked reports what it saw through
 * a pipe:
 *   - the parent process exits: SIGUSR1, SI_USER, from the parent's pid and
 *     uid, and the child's parent is now the subreaper;
 *   - a worker thread forks it and exits: it arrives though the process lives
 *     on, which is still the child's parent and still reaps it;
 *   - the main thread forks it and exits, a worker staying: the same;
 *   - another thread of the parent exits: nothing (then the parent's own exit
 *     sends it, the control);
 *   - a thread asks for it in a process whose thread that created that thread
 *     exits: nothing, and another thread reads 0 (then the process's parent
 *     exits, and the process gets it);
 *   - the main thread asks, then leaves with the raw exit call: the process
 *     still gets it when its parent exits;
 *   - a subreaper that took the child and then exits sends it a second time;
 *   - exit_group from the main thread when a worker forked the child;
 *   - another thread of the parent execs, killing the thread that forked it;
 *   - a child it forks reads 0, and after an exec it still asks and gets it;
 *   - (root) a parent that has become uid 1000 sends a root child nothing;
 *   - credential changes, and execs -- of set-id binaries, and plain ones
 *     after a credential change -- that forget it and that keep it;
 *   - the prctl refuses 65 and -1 with EINVAL and takes 64 and 0.
 *
 * Measured before the fix: 26 failures on alpine-amd64-test and
 * devuan-amd64-test alike -- nothing was ever sent, the prctl took 65 and -1,
 * and no credential change or exec forgot it.
 *
 * Checked against Linux 6.12 (Devuan 6, x86_64 and -m32, as uid 1000 and as
 * root). The cases that need root, or a supplementary group to give a file,
 * are skipped without one.
 *
 * Exits 0 and prints "pdeath_signal: PASS" on success.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <linux/futex.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/fsuid.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#ifndef __WALL
#define __WALL 0x40000000
#endif

/* musl on i386 names the 32-bit-time calls for what they take. */
#if !defined(SYS_futex) && defined(SYS_futex_time32)
#define SYS_futex SYS_futex_time32
#endif

/* The signal every process under test asks for. Blocked before it is asked
 * for, so it is queued and taken with sigtimedwait rather than killing. */
#define PDEATH SIGUSR1
/* What an exit that has cleared its tid word may still send. */
#define SETTLE_MS 250
/* How long a signal that is due may take to arrive. */
#define ARRIVAL_MS 2000

/* Ids for the privileged cases: no /etc/passwd entry needed. */
#define OTHER_UID 1000
#define OTHER_GID 1234

static void check(int ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!ok || test_verbose) {
        printf("%s ", ok ? "ok" : "FAIL");
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
    if (!ok)
        failures_total++;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Scaled like the watchdogs, for a heavily loaded run. A longer wait only
 * makes the "nothing arrives" checks stricter. */
static long scaled_ms(long base) {
    return base * (long) test_watchdog_secs(1);
}

static void sleep_ms(long ms) {
    long deadline = now_ms() + ms;
    for (;;) {
        long left = deadline - now_ms();
        if (left <= 0)
            return;
        struct timespec ts = {.tv_sec = left / 1000, .tv_nsec = (left % 1000) * 1000000L};
        nanosleep(&ts, NULL);
    }
}

static void read_byte(int fd) {
    char c;
    while (read(fd, &c, 1) < 0 && errno == EINTR)
        continue;
}

static void write_byte(int fd) {
    if (write(fd, "x", 1) != 1)
        check(0, "pipe write: %s", strerror(errno));
}

/* Exit this thread alone, with its tid word cleared and woken by the kernel:
 * pthread_exit would do libc bookkeeping, and musl's pthread_join never
 * returns for a thread that left another way. */
static void thread_exit(volatile int *tid_word) {
    syscall(SYS_set_tid_address, tid_word);
    syscall(SYS_exit, 0);
}

/* Shared, not private: the kernel's wake at thread exit is a shared one. */
static void wait_thread_gone(volatile int *tid_word) {
    int v;
    while ((v = __atomic_load_n(tid_word, __ATOMIC_ACQUIRE)) != 0)
        syscall(SYS_futex, tid_word, FUTEX_WAIT, v, NULL, NULL, 0);
}

static int pdeath_get(void) {
    int value = -1;
    if (prctl(PR_GET_PDEATHSIG, &value, 0, 0, 0) != 0)
        return -2;
    return value;
}

static void block_pdeath(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, PDEATH);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

/* Ask for PDEATH, blocked first. Returns what PR_GET_PDEATHSIG then says. */
static int arm(void) {
    block_pdeath();
    if (prctl(PR_SET_PDEATHSIG, (unsigned long) PDEATH, 0, 0, 0) != 0)
        return -3;
    return pdeath_get();
}

static const char *signame(int sig) {
    return sig == PDEATH ? "SIGUSR1" : sig == 0 ? "nothing" : "?";
}

/* ---- reports ------------------------------------------------------------- */

/* What a process that asked for the signal saw, sent up to the scenario
 * process through a pipe it made before building its tree. */
struct report {
    int tag;        /* who sent it */
    int self;       /* its pid */
    int parent0;    /* its parent when it asked */
    int armed;      /* PR_GET_PDEATHSIG then */
    int other;      /* a second PR_GET_PDEATHSIG, where a case needs one */
    int sig;        /* what it took, 0 for nothing */
    int code;       /* ...its si_code */
    int pid;        /* ...its si_pid */
    int uid;        /* ...its si_uid */
    int ppid;       /* its parent afterwards */
    int euid, egid; /* its credentials, for an exec'd image */
};

static int rep[2];          /* the scenario's report pipe */

static void report_init(struct report *r, int tag) {
    memset(r, 0, sizeof(*r));
    r->tag = tag;
    r->self = (int) getpid();
    r->parent0 = (int) getppid();
    r->armed = -1;
    r->other = -1;
    r->euid = (int) geteuid();
    r->egid = (int) getegid();
}

static void send_report(const struct report *r) {
    if (write(rep[1], r, sizeof(*r)) != (ssize_t) sizeof(*r))
        _exit(99);
}

/* Take PDEATH if it comes within ms. */
static void take_pdeath(struct report *r, long ms) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, PDEATH);
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    long deadline = now_ms() + ms;
    int sig;
    for (;;) {
        long left = deadline - now_ms();
        if (left < 0)
            left = 0;
        struct timespec ts = {.tv_sec = left / 1000, .tv_nsec = (left % 1000) * 1000000L};
        sig = sigtimedwait(&set, &si, &ts);
        if (sig >= 0 || errno != EINTR)
            break;
    }
    r->sig = sig > 0 ? sig : 0;
    r->code = sig > 0 ? si.si_code : 0;
    r->pid = sig > 0 ? (int) si.si_pid : 0;
    r->uid = sig > 0 ? (int) si.si_uid : 0;
    r->ppid = (int) getppid();
}

static bool get_report(struct report *r, int tag, const char *label) {
    struct pollfd pfd = {.fd = rep[0], .events = POLLIN};
    long deadline = now_ms() + scaled_ms(ARRIVAL_MS * 3);
    for (;;) {
        long left = deadline - now_ms();
        if (left < 0)
            left = 0;
        int n = poll(&pfd, 1, (int) left);
        if (n > 0)
            break;
        if (n == 0 || errno != EINTR) {
            check(0, "%s: no report from '%c'", label, tag);
            return false;
        }
    }
    if (read(rep[0], r, sizeof(*r)) != (ssize_t) sizeof(*r)) {
        check(0, "%s: short report from '%c'", label, tag);
        return false;
    }
    if (r->tag != tag) {
        check(0, "%s: report from '%c', want '%c'", label, r->tag, tag);
        return false;
    }
    return true;
}

/* The report shows PDEATH, sent by process `from` as SI_USER from our uid,
 * with `ppid` its parent afterwards. */
static void expect_pdeath(const char *label, const struct report *r, pid_t from, pid_t ppid) {
    check(r->armed == PDEATH, "%s: PR_GET_PDEATHSIG = %d once asked, want %d",
          label, r->armed, PDEATH);
    check(r->sig == PDEATH && r->code == SI_USER && r->pid == (int) from &&
                  r->uid == (int) getuid(),
          "%s: got %s code %d from pid %d uid %d, want %s code SI_USER from pid %d uid %d",
          label, signame(r->sig), r->code, r->pid, r->uid,
          signame(PDEATH), (int) from, (int) getuid());
    check(r->ppid == (int) ppid, "%s: parent afterwards %d, want %d", label, r->ppid, (int) ppid);
}

static void expect_nothing(const char *label, const struct report *r) {
    check(r->sig == 0, "%s: got %s code %d from pid %d, want nothing",
          label, signame(r->sig), r->code, r->pid);
}

/* ---- the scenario process ------------------------------------------------ */

/* Every scenario runs in a process of its own, a subreaper, so the orphans
 * its tree makes come back to it; its tree is the process groups spawn()
 * started, killed when it is done. */
static pid_t tops[4];
static int ntops;

static pid_t spawn(void) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        ntops = 0;
        return 0;
    }
    if (pid > 0) {
        setpgid(pid, pid);
        if (ntops < (int) (sizeof(tops) / sizeof(tops[0])))
            tops[ntops++] = pid;
    } else {
        check(0, "fork: %s", strerror(errno));
    }
    return pid;
}

static void cleanup_tree(void) {
    for (int i = 0; i < ntops; i++)
        kill(-tops[i], SIGKILL);
    long deadline = now_ms() + scaled_ms(3000);
    while (now_ms() < deadline) {
        pid_t w = waitpid(-1, NULL, WNOHANG | __WALL);
        if (w < 0 && errno == ECHILD)
            break;
        if (w <= 0)
            sleep_ms(10);
    }
}

/* The exit status of `pid`, a child of this process, or -1. */
static int exit_status(pid_t pid) {
    int status;
    pid_t w;
    while ((w = waitpid(pid, &status, 0)) < 0 && errno == EINTR)
        continue;
    if (w != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

typedef void (*scenario_fn)(const char *label);

static void scenario(const char *label, scenario_fn fn) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        check(0, "%s: fork: %s", label, strerror(errno));
        return;
    }
    if (pid == 0) {
        failures_total = 0;     /* forked from the harness, which has its own */
        alarm(test_watchdog_secs(60));
        if (prctl(PR_SET_CHILD_SUBREAPER, 1L, 0L, 0L, 0L) != 0)
            check(0, "%s: PR_SET_CHILD_SUBREAPER: %s", label, strerror(errno));
        else if (pipe(rep) != 0)
            check(0, "%s: pipe: %s", label, strerror(errno));
        else
            fn(label);
        cleanup_tree();
        fflush(stdout);
        _exit(failures_total > 100 ? 100 : (int) failures_total);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        continue;
    if (WIFSIGNALED(status))
        check(0, "%s: killed by signal %d", label, WTERMSIG(status));
    else if (WEXITSTATUS(status) != 0)
        failures_total += (unsigned) WEXITSTATUS(status);
    else
        test_logf("ok %s\n", label);
}

/* ---- the parent process exits -------------------------------------------- */

static void process_exits(const char *label) {
    int armed[2];
    if (pipe(armed) != 0) {
        check(0, "%s: pipe: %s", label, strerror(errno));
        return;
    }
    pid_t p = spawn();
    if (p == 0) {
        if (fork() == 0) {
            struct report r;
            report_init(&r, 'C');
            r.armed = arm();
            write_byte(armed[1]);
            take_pdeath(&r, scaled_ms(ARRIVAL_MS));
            send_report(&r);
            _exit(0);
        }
        read_byte(armed[0]);
        _exit(0);
    }
    struct report r;
    if (get_report(&r, 'C', label))
        expect_pdeath(label, &r, p, getpid());
}

/* ---- the forking thread exits and the process lives on ------------------- */

static struct {
    int armed[2];       /* child -> forking thread */
    int release[2];     /* scenario -> process: finish */
    volatile int tid_word;
    pid_t child;
} tw;

static void *worker_forks_then_exits(void *arg) {
    (void) arg;
    pid_t c = fork();
    if (c == 0) {
        struct report r;
        report_init(&r, 'C');
        r.armed = arm();
        write_byte(tw.armed[1]);
        take_pdeath(&r, scaled_ms(ARRIVAL_MS));
        send_report(&r);
        _exit(0);
    }
    tw.child = c;
    read_byte(tw.armed[0]);
    thread_exit(&tw.tid_word);
    return NULL;
}

/* Waits for the scenario to release it, then reaps the child the worker
 * forked: still this process's child, whichever of its threads is left. */
static void reap_worker_child(void) {
    read_byte(tw.release[0]);
    int status = 0;
    pid_t w;
    while ((w = waitpid(tw.child, &status, 0)) < 0 && errno == EINTR)
        continue;
    _exit(w == tw.child && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1);
}

static void worker_exits(const char *label) {
    if (pipe(tw.armed) != 0 || pipe(tw.release) != 0) {
        check(0, "%s: pipe: %s", label, strerror(errno));
        return;
    }
    pid_t p = spawn();
    if (p == 0) {
        tw.tid_word = 1;
        pthread_t worker;
        if (pthread_create(&worker, NULL, worker_forks_then_exits, NULL) != 0)
            _exit(2);
        wait_thread_gone(&tw.tid_word);
        reap_worker_child();
    }
    struct report r;
    if (get_report(&r, 'C', label))
        expect_pdeath(label, &r, p, p);
    write_byte(tw.release[1]);
    check(exit_status(p) == 0, "%s: the process reaps the worker's child", label);
}

/* ---- the main thread forked it and exits; a worker stays ----------------- */

static void *main_exits_survivor(void *arg) {
    (void) arg;
    wait_thread_gone(&tw.tid_word);
    reap_worker_child();
    return NULL;
}

static void main_exits(const char *label) {
    if (pipe(tw.armed) != 0 || pipe(tw.release) != 0) {
        check(0, "%s: pipe: %s", label, strerror(errno));
        return;
    }
    pid_t p = spawn();
    if (p == 0) {
        pid_t c = fork();
        if (c == 0) {
            struct report r;
            report_init(&r, 'C');
            r.armed = arm();
            write_byte(tw.armed[1]);
            take_pdeath(&r, scaled_ms(ARRIVAL_MS));
            send_report(&r);
            _exit(0);
        }
        tw.child = c;
        tw.tid_word = 1;
        read_byte(tw.armed[0]);
        pthread_t survivor;
        if (pthread_create(&survivor, NULL, main_exits_survivor, NULL) != 0)
            _exit(2);
        thread_exit(&tw.tid_word);
    }
    struct report r;
    if (get_report(&r, 'C', label))
        expect_pdeath(label, &r, p, p);
    write_byte(tw.release[1]);
    check(exit_status(p) == 0, "%s: the process reaps the child", label);
}

/* ---- another thread's exit is not the parent's --------------------------- */

static struct {
    int go[2];          /* process -> worker: exit now */
    int worker_gone[2]; /* process -> child: it has */
    int release[2];     /* scenario -> process: exit now */
    volatile int tid_word;
} sb;

static void *sibling_waits_then_exits(void *arg) {
    (void) arg;
    read_byte(sb.go[0]);
    thread_exit(&sb.tid_word);
    return NULL;
}

static void sibling_exits(const char *label) {
    int armed[2];
    if (pipe(armed) != 0 || pipe(sb.go) != 0 || pipe(sb.worker_gone) != 0 ||
            pipe(sb.release) != 0) {
        check(0, "%s: pipe: %s", label, strerror(errno));
        return;
    }
    pid_t p = spawn();
    if (p == 0) {
        sb.tid_word = 1;
        pthread_t worker;
        if (pthread_create(&worker, NULL, sibling_waits_then_exits, NULL) != 0)
            _exit(2);
        if (fork() == 0) {
            struct report r;
            report_init(&r, 'C');
            r.armed = arm();
            write_byte(armed[1]);
            read_byte(sb.worker_gone[0]);
            take_pdeath(&r, scaled_ms(SETTLE_MS));
            send_report(&r);
            report_init(&r, 'c');
            r.armed = pdeath_get();
            take_pdeath(&r, scaled_ms(ARRIVAL_MS));
            send_report(&r);
            _exit(0);
        }
        read_byte(armed[0]);
        write_byte(sb.go[1]);
        wait_thread_gone(&sb.tid_word);
        write_byte(sb.worker_gone[1]);
        read_byte(sb.release[0]);
        _exit(0);
    }
    struct report r;
    if (get_report(&r, 'C', label))
        expect_nothing(label, &r);
    /* The control: its parent's own exit does send it. */
    write_byte(sb.release[1]);
    if (get_report(&r, 'c', label))
        expect_pdeath(label, &r, p, getpid());
}

/* ---- a thread's setting is for its process's parent ---------------------- */

/* In AOK a thread is a child of the thread that created it, so a thread's exit
 * hands its sibling threads on along with its child processes. On Linux no
 * thread is anyone's child: a thread's parent-death signal is sent when the
 * PROCESS's parent dies, and only then. */

static struct {
    int armed[2];       /* arming thread -> worker: what it read back */
    int relay[2];       /* worker -> main: the same, once it is about to exit */
    volatile int worker_word;
} ts;

static void *thread_arms(void *arg) {
    (void) arg;
    int value = arm();
    if (write(ts.armed[1], &value, sizeof(value)) != (ssize_t) sizeof(value))
        _exit(3);
    for (;;)
        pause();
    return NULL;
}

static void *worker_starts_armer_then_exits(void *arg) {
    (void) arg;
    pthread_t armer;
    if (pthread_create(&armer, NULL, thread_arms, NULL) != 0)
        _exit(2);
    int value = 0;
    if (read(ts.armed[0], &value, sizeof(value)) != (ssize_t) sizeof(value))
        _exit(3);
    if (write(ts.relay[1], &value, sizeof(value)) != (ssize_t) sizeof(value))
        _exit(3);
    thread_exit(&ts.worker_word);
    return NULL;
}

static void thread_setting(const char *label) {
    int release[2];
    if (pipe(ts.armed) != 0 || pipe(ts.relay) != 0 || pipe(release) != 0) {
        check(0, "%s: pipe: %s", label, strerror(errno));
        return;
    }
    pid_t q = spawn();
    if (q == 0) {
        if (fork() == 0) {
            /* P: every thread blocks PDEATH, inherited from here. */
            block_pdeath();
            ts.worker_word = 1;
            pthread_t worker;
            if (pthread_create(&worker, NULL, worker_starts_armer_then_exits, NULL) != 0)
                _exit(2);
            int value = -1;
            if (read(ts.relay[0], &value, sizeof(value)) != (ssize_t) sizeof(value))
                _exit(3);
            wait_thread_gone(&ts.worker_word);
            struct report r;
            report_init(&r, 'P');
            r.armed = value;            /* the arming thread's own view */
            r.other = pdeath_get();     /* this thread's */
            take_pdeath(&r, scaled_ms(SETTLE_MS));
            send_report(&r);
            report_init(&r, 'p');
            r.armed = value;
            take_pdeath(&r, scaled_ms(ARRIVAL_MS));
            send_report(&r);
            _exit(0);
        }
        read_byte(release[0]);
        _exit(0);
    }
    struct report r;
    if (get_report(&r, 'P', label)) {
        check(r.armed == PDEATH, "%s: the arming thread reads back %d, want %d",
              label, r.armed, PDEATH);
        check(r.other == 0, "%s: another thread of its process reads %d, want 0",
              label, r.other);
        char sub[160];
        snprintf(sub, sizeof(sub), "%s: the exit of the thread that created it", label);
        expect_nothing(sub, &r);
    }
    /* The control: the process's parent exiting sends it, to the process. */
    write_byte(release[1]);
    if (get_report(&r, 'p', label))
        expect_pdeath(label, &r, q, getpid());
}

/* ---- the leader asked for it and has exited ------------------------------ */

static struct {
    int armed[2];       /* child -> parent: armed, and its leader is gone */
    volatile int leader_word;
} la;

static void *leader_gone_taker(void *arg) {
    (void) arg;
    struct report r;
    report_init(&r, 'C');
    r.armed = pdeath_get();         /* not this thread's: 0 */
    wait_thread_gone(&la.leader_word);
    write_byte(la.armed[1]);
    take_pdeath(&r, scaled_ms(ARRIVAL_MS));
    send_report(&r);
    _exit(0);
    return NULL;
}

static void leader_armed_exited(const char *label) {
    if (pipe(la.armed) != 0) {
        check(0, "%s: pipe: %s", label, strerror(errno));
        return;
    }
    pid_t p = spawn();
    if (p == 0) {
        if (fork() == 0) {
            int value = arm();      /* before the thread, which inherits the block */
            if (value != PDEATH)
                _exit(4);
            la.leader_word = 1;
            pthread_t taker;
            if (pthread_create(&taker, NULL, leader_gone_taker, NULL) != 0)
                _exit(2);
            thread_exit(&la.leader_word);
        }
        read_byte(la.armed[0]);
        _exit(0);
    }
    struct report r;
    if (get_report(&r, 'C', label)) {
        check(r.armed == 0, "%s: the other thread reads %d, want 0", label, r.armed);
        r.armed = PDEATH;           /* the leader's, checked before it exited */
        expect_pdeath(label, &r, p, getpid());
    }
}

/* ---- every reparent sends it again --------------------------------------- */

static void every_reparent(const char *label) {
    int armed[2], release[2];
    if (pipe(armed) != 0 || pipe(release) != 0) {
        check(0, "%s: pipe: %s", label, strerror(errno));
        return;
    }
    pid_t r_pid = spawn();
    if (r_pid == 0) {
        /* R: a subreaper, so the child comes here when its parent exits. */
        if (prctl(PR_SET_CHILD_SUBREAPER, 1L, 0L, 0L, 0L) != 0)
            _exit(2);
        if (fork() == 0) {
            if (fork() == 0) {
                struct report r;
                report_init(&r, 'C');
                r.armed = arm();
                write_byte(armed[1]);
                take_pdeath(&r, scaled_ms(ARRIVAL_MS));
                send_report(&r);
                report_init(&r, 'c');
                r.armed = pdeath_get();
                take_pdeath(&r, scaled_ms(ARRIVAL_MS));
                send_report(&r);
                _exit(0);
            }
            read_byte(armed[0]);
            _exit(0);
        }
        read_byte(release[0]);
        _exit(0);
    }
    struct report r;
    if (get_report(&r, 'C', label)) {
        char sub[160];
        snprintf(sub, sizeof(sub), "%s: its parent exits", label);
        expect_pdeath(sub, &r, r.parent0, r_pid);
    }
    write_byte(release[1]);
    if (get_report(&r, 'c', label)) {
        char sub[160];
        snprintf(sub, sizeof(sub), "%s: then the subreaper that took it", label);
        expect_pdeath(sub, &r, r_pid, getpid());
    }
}

/* ---- the process exits from another thread ------------------------------- */

static void *worker_forks_then_waits(void *arg) {
    (void) arg;
    if (fork() == 0) {
        struct report r;
        report_init(&r, 'C');
        r.armed = arm();
        write_byte(tw.armed[1]);
        take_pdeath(&r, scaled_ms(ARRIVAL_MS));
        send_report(&r);
        _exit(0);
    }
    for (;;)
        pause();
    return NULL;
}

static void exit_group_other_thread(const char *label) {
    if (pipe(tw.armed) != 0) {
        check(0, "%s: pipe: %s", label, strerror(errno));
        return;
    }
    pid_t p = spawn();
    if (p == 0) {
        pthread_t worker;
        if (pthread_create(&worker, NULL, worker_forks_then_waits, NULL) != 0)
            _exit(2);
        read_byte(tw.armed[0]);
        exit(0);                    /* exit_group, from main */
    }
    struct report r;
    if (get_report(&r, 'C', label))
        expect_pdeath(label, &r, p, getpid());
}

/* ---- another thread execs ------------------------------------------------ */

static char self_path[64];

static struct {
    int release[2];     /* scenario -> the exec'd image: exit */
} ex;

static void *worker_execs(void *arg) {
    (void) arg;
    char fd[16];
    snprintf(fd, sizeof(fd), "%d", ex.release[0]);
    execl(self_path, self_path, "--pdeath-wait", fd, (char *) NULL);
    _exit(127);
    return NULL;
}

static void exec_other_thread(const char *label) {
    int armed[2];
    if (pipe(armed) != 0 || pipe(ex.release) != 0) {
        check(0, "%s: pipe: %s", label, strerror(errno));
        return;
    }
    pid_t p = spawn();
    if (p == 0) {
        if (fork() == 0) {
            struct report r;
            report_init(&r, 'C');
            r.armed = arm();
            write_byte(armed[1]);
            take_pdeath(&r, scaled_ms(ARRIVAL_MS));
            send_report(&r);
            _exit(0);
        }
        read_byte(armed[0]);
        pthread_t worker;
        if (pthread_create(&worker, NULL, worker_execs, NULL) != 0)
            _exit(2);
        for (;;)
            pause();
    }
    struct report r;
    if (get_report(&r, 'C', label))
        expect_pdeath(label, &r, p, p);
    write_byte(ex.release[1]);
    check(exit_status(p) == 0, "%s: the exec'd process exits normally", label);
}

/* ---- fork forgets it, exec keeps it -------------------------------------- */

static void fork_and_exec(const char *label) {
    int ready[2];
    if (pipe(ready) != 0) {
        check(0, "%s: pipe: %s", label, strerror(errno));
        return;
    }
    pid_t p = spawn();
    if (p == 0) {
        if (fork() == 0) {
            int armed = arm();
            if (fork() == 0) {
                struct report r;
                report_init(&r, 'D');
                r.armed = pdeath_get();
                r.other = armed;
                send_report(&r);
                _exit(0);
            }
            while (wait(NULL) < 0 && errno == EINTR)
                continue;
            char fd[16], ready_fd[16], wait_ms[16];
            snprintf(fd, sizeof(fd), "%d", rep[1]);
            snprintf(ready_fd, sizeof(ready_fd), "%d", ready[1]);
            snprintf(wait_ms, sizeof(wait_ms), "%ld", scaled_ms(ARRIVAL_MS));
            execl(self_path, self_path, "--pdeath-exec", fd, ready_fd, wait_ms, (char *) NULL);
            _exit(127);
        }
        read_byte(ready[0]);
        _exit(0);
    }
    struct report r;
    if (get_report(&r, 'D', label)) {
        check(r.other == PDEATH, "%s: PR_GET_PDEATHSIG = %d once asked, want %d",
              label, r.other, PDEATH);
        check(r.armed == 0, "%s: a child it forks reads %d, want 0", label, r.armed);
    }
    if (get_report(&r, 'X', label)) {
        char sub[160];
        snprintf(sub, sizeof(sub), "%s: after exec", label);
        expect_pdeath(sub, &r, p, getpid());
    }
}

/* ---- the parent may not signal it ---------------------------------------- */

/* Linux sends it with group_send_sig_info, which asks check_kill_permission
 * of the exiting parent: a parent that has become another user sends nothing.
 * The subreaper above it, still root, is the control. */
static void parent_may_not_signal(const char *label) {
    if (geteuid() != 0) {
        test_logf("SKIP %s: needs root\n", label);
        return;
    }
    int armed[2], release[2];
    if (pipe(armed) != 0 || pipe(release) != 0) {
        check(0, "%s: pipe: %s", label, strerror(errno));
        return;
    }
    pid_t r_pid = spawn();
    if (r_pid == 0) {
        if (prctl(PR_SET_CHILD_SUBREAPER, 1L, 0L, 0L, 0L) != 0)
            _exit(2);
        if (fork() == 0) {
            if (fork() == 0) {
                struct report r;
                report_init(&r, 'C');
                r.armed = arm();
                write_byte(armed[1]);
                /* Its parent exits as another user: wait for the reparent,
                 * then for anything it sent. */
                pid_t parent = getppid();
                long deadline = now_ms() + scaled_ms(ARRIVAL_MS);
                while (getppid() == parent && now_ms() < deadline)
                    sleep_ms(5);
                take_pdeath(&r, scaled_ms(SETTLE_MS));
                send_report(&r);
                report_init(&r, 'c');
                r.armed = pdeath_get();
                take_pdeath(&r, scaled_ms(ARRIVAL_MS));
                send_report(&r);
                _exit(0);
            }
            read_byte(armed[0]);
            if (setgid(OTHER_GID) != 0 || setuid(OTHER_UID) != 0)
                _exit(2);
            _exit(0);
        }
        read_byte(release[0]);
        _exit(0);
    }
    struct report r;
    if (get_report(&r, 'C', label)) {
        check(r.armed == PDEATH, "%s: PR_GET_PDEATHSIG = %d once asked, want %d",
              label, r.armed, PDEATH);
        check(r.ppid == (int) r_pid, "%s: its parent afterwards is %d, want %d",
              label, r.ppid, (int) r_pid);
        char sub[160];
        snprintf(sub, sizeof(sub), "%s: a parent that became uid %d exits", label, OTHER_UID);
        expect_nothing(sub, &r);
    }
    write_byte(release[1]);
    if (get_report(&r, 'c', label)) {
        char sub[160];
        snprintf(sub, sizeof(sub), "%s: then the root subreaper that took it", label);
        expect_pdeath(sub, &r, r_pid, getpid());
    }
}

/* ---- credential changes -------------------------------------------------- */

static int same_uid(void) { return setuid(getuid()); }
static int no_change(void) { return setresuid((uid_t) -1, (uid_t) -1, (uid_t) -1); }
static int fsuid_same(void) {
    setfsuid(geteuid());
    return setfsuid((uid_t) -1) == (int) geteuid() ? 0 : -1;
}
static int groups_change(void) {
    gid_t g = OTHER_GID;
    return setgroups(1, &g);
}
static int ruid_only(void) { return setresuid(OTHER_UID, (uid_t) -1, (uid_t) -1); }
static int suid_only(void) { return setresuid((uid_t) -1, (uid_t) -1, OTHER_UID); }
static int euid_change(void) { return setresuid((uid_t) -1, OTHER_UID, (uid_t) -1); }
static int fsuid_change(void) {
    setfsuid(OTHER_UID);
    return setfsuid((uid_t) -1) == OTHER_UID ? 0 : -1;
}
static int rgid_only(void) { return setresgid(OTHER_GID, (gid_t) -1, (gid_t) -1); }
static int sgid_only(void) { return setresgid((gid_t) -1, (gid_t) -1, OTHER_GID); }
static int egid_change(void) { return setresgid((gid_t) -1, OTHER_GID, (gid_t) -1); }
static int fsgid_change(void) {
    setfsgid(OTHER_GID);
    return setfsgid((gid_t) -1) == OTHER_GID ? 0 : -1;
}
static int uid_drop(void) { return setuid(OTHER_UID); }
static int cap_drop(void) {
    struct __user_cap_header_struct hdr = {.version = _LINUX_CAPABILITY_VERSION_3, .pid = 0};
    struct __user_cap_data_struct data[2];
    memset(data, 0, sizeof(data));
    if (syscall(SYS_capget, &hdr, data) != 0)
        return -1;
    data[CAP_SYS_BOOT / 32].permitted &= ~(1u << (CAP_SYS_BOOT % 32));
    data[CAP_SYS_BOOT / 32].effective &= ~(1u << (CAP_SYS_BOOT % 32));
    return (int) syscall(SYS_capset, &hdr, data);
}

struct cred_case {
    const char *label;
    int (*change)(void);
    bool kept;
    bool needs_root;
};

static const struct cred_case cred_cases[] = {
    {"setuid to its own uid", same_uid, true, false},
    {"setresuid(-1, -1, -1)", no_change, true, false},
    {"setfsuid to its euid", fsuid_same, true, false},
    {"setgroups", groups_change, true, true},
    {"a real uid change alone", ruid_only, true, true},
    {"a saved uid change alone", suid_only, true, true},
    {"a real gid change alone", rgid_only, true, true},
    {"a saved gid change alone", sgid_only, true, true},
    {"dropping a capability", cap_drop, true, true},
    {"an effective uid change", euid_change, false, true},
    {"a filesystem uid change", fsuid_change, false, true},
    {"an effective gid change", egid_change, false, true},
    {"a filesystem gid change", fsgid_change, false, true},
    {"setuid to another uid", uid_drop, false, true},
};

static void cred_changes(const char *label) {
    for (size_t i = 0; i < sizeof(cred_cases) / sizeof(cred_cases[0]); i++) {
        const struct cred_case *cc = &cred_cases[i];
        if (cc->needs_root && geteuid() != 0) {
            test_logf("SKIP %s: %s: needs root\n", label, cc->label);
            continue;
        }
        pid_t c = fork();
        if (c == 0) {
            if (arm() != PDEATH)
                _exit(4);
            if (cc->change() != 0)
                _exit(3);
            int after = pdeath_get();
            _exit(after == PDEATH ? 1 : after == 0 ? 0 : 2);
        }
        int got = exit_status(c);
        check(got == (cc->kept ? 1 : 0),
              "%s: %s: %s, want it %s (child status %d)", label, cc->label,
              got == 1 ? "kept" : got == 0 ? "cleared" : got == 3 ? "the change failed" :
              got == 4 ? "could not arm" : "?", cc->kept ? "kept" : "cleared", got);
    }
}

/* ---- execs --------------------------------------------------------------- */

/* A copy of this binary, owned by uid:gid with `mode`, or NULL. */
static bool copy_self(char *path, size_t size, const char *tag, uid_t uid, gid_t gid,
        mode_t mode) {
    snprintf(path, size, "/tmp/pdeath_signal.%d.%s", (int) getpid(), tag);
    unlink(path);
    int in = open(self_path, O_RDONLY);
    int out = open(path, O_WRONLY | O_CREAT | O_EXCL, 0700);
    bool ok = in >= 0 && out >= 0;
    char buf[65536];
    ssize_t n;
    while (ok && (n = read(in, buf, sizeof(buf))) != 0) {
        if (n < 0 || write(out, buf, (size_t) n) != n)
            ok = false;
    }
    if (in >= 0)
        close(in);
    if (out >= 0 && close(out) != 0)
        ok = false;
    /* chown first: it clears the set-id bits. */
    if (ok && (chown(path, uid, gid) != 0 || chmod(path, mode) != 0))
        ok = false;
    if (!ok) {
        check(0, "copying %s to %s: %s", self_path, path, strerror(errno));
        unlink(path);
    }
    return ok;
}

/* An exec, of a set-id copy of this binary or of this binary itself after a
 * credential change made before asking for the signal. */
struct exec_case {
    const char *tag;
    const char *label;
    int (*change)(void);    /* before asking, for a plain exec; NULL otherwise */
    bool other_uid;         /* the copy is owned by OTHER_UID, else by us */
    int gid_kind;           /* the copy's group: 0 our egid, 1 another group */
    mode_t mode;            /* the copy's mode; 0 for a plain exec */
    int euid_kind, egid_kind;   /* what the image runs as: 's' as we are,
                                 * 'o' OTHER_UID/OTHER_GID, 'f' the file's */
    bool kept;
    bool needs_root;
};

static const struct exec_case exec_cases[] = {
    {"suid-own", "exec of a set-user-ID binary it owns", NULL, false, 0, 04755,
     'f', 's', true, false},
    {"sgid-own", "exec of a set-group-ID binary of its own group", NULL, false, 0, 02755,
     's', 'f', true, false},
    {"sgid-other", "exec of a set-group-ID binary of another group", NULL, false, 1, 02755,
     's', 'f', false, false},
    {"suid-other", "exec of a set-user-ID binary of another user", NULL, true, 0, 04755,
     'f', 's', false, true},
    /* A plain exec that resets the filesystem ids, or leaves the effective
     * ones other than the real ones, is a change too. */
    {"plain-fsuid", "a plain exec after setfsuid", fsuid_change, false, 0, 0,
     's', 's', false, true},
    {"plain-euid", "a plain exec after an effective uid change", euid_change, false, 0, 0,
     'o', 's', false, true},
    {"plain-suid", "a plain exec after a saved uid change", suid_only, false, 0, 0,
     's', 's', true, true},
    {"plain-fsgid", "a plain exec after setfsgid", fsgid_change, false, 0, 0,
     's', 's', false, true},
    {"plain-egid", "a plain exec after an effective gid change", egid_change, false, 0, 0,
     's', 'o', false, true},
};

/* A group we may give a file that is not our effective one, or -1. */
static gid_t other_group(void) {
    if (geteuid() == 0)
        return OTHER_GID;
    gid_t groups[64];
    int n = getgroups(64, groups);
    for (int i = 0; i < n; i++) {
        if (groups[i] != getegid())
            return groups[i];
    }
    return (gid_t) -1;
}

static void execs(const char *label) {
    for (size_t i = 0; i < sizeof(exec_cases) / sizeof(exec_cases[0]); i++) {
        const struct exec_case *ec = &exec_cases[i];
        if (ec->needs_root && geteuid() != 0) {
            test_logf("SKIP %s: %s: needs root\n", label, ec->label);
            continue;
        }
        gid_t gid = ec->gid_kind == 1 ? other_group() : getegid();
        if (gid == (gid_t) -1) {
            test_logf("SKIP %s: %s: no other group to give it\n", label, ec->label);
            continue;
        }
        uid_t uid = ec->other_uid ? OTHER_UID : geteuid();
        char path[128];
        const char *file = self_path;
        if (ec->mode != 0) {
            if (!copy_self(path, sizeof(path), ec->tag, uid, gid, ec->mode))
                continue;
            file = path;
        }
        pid_t c = fork();
        if (c == 0) {
            if (ec->change != NULL && ec->change() != 0)
                _exit(3);
            arm();
            char fd[16];
            snprintf(fd, sizeof(fd), "%d", rep[1]);
            execl(file, file, "--pdeath-exec", fd, "-1", "0", (char *) NULL);
            _exit(127);
        }
        struct report r;
        bool got = get_report(&r, 'X', ec->label);
        int status = exit_status(c);
        if (ec->mode != 0)
            unlink(path);
        if (!got) {
            check(0, "%s: %s: the child exited %d", label, ec->label, status);
            continue;
        }
        /* The positive control: the image runs as the case says it does, or
         * the case tests nothing. */
        int want_euid = ec->euid_kind == 'f' ? (int) uid :
                        ec->euid_kind == 'o' ? OTHER_UID : (int) geteuid();
        int want_egid = ec->egid_kind == 'f' ? (int) gid :
                        ec->egid_kind == 'o' ? OTHER_GID : (int) getegid();
        check(r.euid == want_euid && r.egid == want_egid,
              "%s: %s: the new image runs as euid %d egid %d, want %d %d",
              label, ec->label, r.euid, r.egid, want_euid, want_egid);
        check(r.armed == (ec->kept ? PDEATH : 0),
              "%s: %s: PR_GET_PDEATHSIG = %d afterwards, want %d (%s)", label, ec->label,
              r.armed, ec->kept ? PDEATH : 0, ec->kept ? "kept" : "cleared");
    }
}

/* ---- the prctl itself ---------------------------------------------------- */

static void prctl_arguments(const char *label) {
    check(pdeath_get() == 0, "%s: a new process reads %d, want 0", label, pdeath_get());
    errno = 0;
    int rc = prctl(PR_SET_PDEATHSIG, 65UL, 0, 0, 0);
    check(rc == -1 && errno == EINVAL, "%s: signal 65: %d (%s), want EINVAL",
          label, rc, rc < 0 ? strerror(errno) : "-");
    check(pdeath_get() == 0, "%s: and nothing is set: %d", label, pdeath_get());
    errno = 0;
    rc = prctl(PR_SET_PDEATHSIG, (unsigned long) -1, 0, 0, 0);
    check(rc == -1 && errno == EINVAL, "%s: signal -1: %d (%s), want EINVAL",
          label, rc, rc < 0 ? strerror(errno) : "-");
    rc = prctl(PR_SET_PDEATHSIG, 64UL, 0, 0, 0);
    check(rc == 0 && pdeath_get() == 64, "%s: signal 64: %d, reads %d, want 0 and 64",
          label, rc, pdeath_get());
    rc = prctl(PR_SET_PDEATHSIG, 0UL, 0, 0, 0);
    check(rc == 0 && pdeath_get() == 0, "%s: signal 0: %d, reads %d, want 0 and 0",
          label, rc, pdeath_get());
}

/* ---- the images this test execs ------------------------------------------ */

/* --pdeath-exec REPORT_FD READY_FD WAIT_MS: report what PR_GET_PDEATHSIG says
 * after the exec, say so on READY_FD, and wait WAIT_MS for the signal. */
static int exec_image(char **argv) {
    rep[1] = atoi(argv[2]);
    int ready = atoi(argv[3]);
    long wait_ms = atol(argv[4]);
    struct report r;
    report_init(&r, 'X');
    r.armed = pdeath_get();
    if (ready >= 0)
        write_byte(ready);
    if (wait_ms > 0)
        take_pdeath(&r, wait_ms);
    send_report(&r);
    return 0;
}

/* --pdeath-wait FD: exit once FD has a byte. */
static int wait_image(char **argv) {
    read_byte(atoi(argv[2]));
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 5 && strcmp(argv[1], "--pdeath-exec") == 0)
        return exec_image(argv);
    if (argc == 3 && strcmp(argv[1], "--pdeath-wait") == 0)
        return wait_image(argv);
    test_init(argc, argv);
    setvbuf(stdout, NULL, _IONBF, 0);

    /* What exec runs again: the file itself, so a copy of it can be made. */
    ssize_t n = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (n <= 0 || n >= (ssize_t) sizeof(self_path) - 1) {
        printf("pdeath_signal: FAIL cannot read /proc/self/exe\n");
        return 1;
    }
    self_path[n] = '\0';

    scenario("prctl arguments", prctl_arguments);
    scenario("process exits", process_exits);
    scenario("forking thread exits", worker_exits);
    scenario("main thread exits", main_exits);
    scenario("another thread exits", sibling_exits);
    scenario("a thread's own setting", thread_setting);
    scenario("the leader that asked has exited", leader_armed_exited);
    scenario("every reparent", every_reparent);
    scenario("exit_group from another thread", exit_group_other_thread);
    scenario("exec from another thread", exec_other_thread);
    scenario("fork and exec", fork_and_exec);
    scenario("a parent that may not signal it", parent_may_not_signal);
    scenario("credential changes", cred_changes);
    scenario("execs", execs);

    return finish_suite("pdeath_signal");
}
