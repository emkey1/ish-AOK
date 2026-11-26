// syslog(2) SYSLOG_ACTION_READ waits for something unread instead of
// answering 0.
//
//   AOK's syslog_read returned 0 the moment the kernel log ring buffer was
//   empty. Linux parks the call in wait_event_interruptible until there are
//   records past the reader's position (printk.c, syslog_print), and a signal
//   ends the wait with -ERESTARTSYS.
//
//   That difference is the whole main loop of a syslog daemon. busybox klogd
//   is a klogctl(2, ...) loop that reads 0 as "got nothing, go round again":
//   measured with `klogd -n` in an arm64 guest before the fix, it sat in state
//   R and burned 100 ticks a second -- a whole core -- for as long as it ran.
//   After it, state S and 0 ticks over the same six seconds.
//
//   Note READ is DESTRUCTIVE: it consumes out of the ring buffer rather than
//   tracking a position, so "unread" here means a non-empty buffer. That is
//   also why this file puts lines back at the end -- see the last block.
//
// Measured against x86_64 glibc on Linux 6.12 (camd), running as root, with
// rsyslogd paused for the run: it holds /proc/kmsg open, which is the SAME
// destructive reader position this test uses, so it would race for every
// record. Nothing in an AOK-Workstation guest consumes the log that way, so only the
// oracle needs that.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static int klogctl_(int type, char *buf, int len) {
    return (int) syscall(SYS_syslog, type, buf, len);
}

#define SYSLOG_READ       2
#define SYSLOG_READ_ALL   3
#define SYSLOG_READ_CLEAR 4
#define SYSLOG_SIZE_UNREAD 9
#define SYSLOG_SIZE_BUFFER 10

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-10ld want=%ld\n", label, got, want);
}

static void ck_ge(const char *label, long got, long floor) {
    if (got < floor)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) floor, 0, 0);
    test_logf("  %-56s got=%-10ld want>=%ld\n", label, got, floor);
}

static void ck_le(const char *label, long got, long ceil) {
    if (got > ceil)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) ceil, 0, 0);
    test_logf("  %-56s got=%-10ld want<=%ld\n", label, got, ceil);
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

// Put one line in the log. Root-only (the node is 0644), which is checked once
// up front.
static int kmsg_say(const char *text) {
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd < 0)
        return -1;
    char line[256];
    int n = snprintf(line, sizeof line, "<6>%s\n", text);
    ssize_t w = write(fd, line, (size_t) n);
    close(fd);
    return w == n ? 0 : -1;
}

// Consume everything unread, so a following READ has nothing to return and
// must wait. Bounded: a kernel that never empties fails rather than looping.
static int drain(void) {
    char buf[8192];
    for (int i = 0; i < 10000; i++) {
        int unread = klogctl_(SYSLOG_SIZE_UNREAD, NULL, 0);
        if (unread <= 0)
            return unread == 0 ? 0 : -1;
        int want = unread < (int) sizeof buf ? unread : (int) sizeof buf;
        if (klogctl_(SYSLOG_READ, buf, want) <= 0)
            return -1;
    }
    return -1;
}

// /proc/<pid>/stat: state character, and utime+stime in ticks. comm can hold
// spaces and parentheses, so both are counted from the LAST ')'.
static int proc_stat(pid_t pid, char *state, long *ticks) {
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", (int) pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    char *p = strrchr(buf, ')');
    if (p == NULL || p[1] == '\0')
        return -1;
    *state = p[2];
    // Fields after the state: ppid pgrp session tty tpgid flags minflt
    // cminflt majflt cmajflt utime stime -- utime is the 11th, stime the 12th.
    long f[16];
    char *rest = p + 3;
    int got = 0;
    for (; got < 16; got++) {
        char *end;
        long v = strtol(rest, &end, 10);
        if (end == rest)
            break;
        f[got] = v;
        rest = end;
    }
    if (got < 12)
        return -1;
    *ticks = f[10] + f[11];
    return 0;
}

// What the blocked child hands back through the pipe.
struct read_result {
    long elapsed_ms;
    int  found_marker;   // the READ that returned our line
    int  zero_returns;   // a READ that answered 0 -- the bug this file is about
    int  err;            // errno of a failed READ, 0 otherwise
};

static volatile sig_atomic_t usr1_count;
static void on_usr1(int sig) { (void) sig; usr1_count++; }

// A watchdog PROCESS, because SIGALRM's slot has to stay free: every wait below
// is ended by a signal on purpose, and an alarm() backstop would be
// indistinguishable from the thing being measured. Without one, a read that
// never wakes hangs the whole suite instead of failing this test.
static pid_t watchdog_start(unsigned secs) {
    pid_t victim = getpid();
    fflush(stdout);
    pid_t dog = fork();
    if (dog != 0)
        return dog;
    for (unsigned i = 0; i < secs; i++) {
        sleep(1);
        if (getppid() != victim)
            _exit(0);  // the parent is gone already; nothing to guard
    }
    printf("syslog_read_blocks: FAIL watchdog fired after %us -- a read never "
           "woke\n", secs);
    fflush(stdout);
    kill(victim, SIGKILL);
    _exit(1);
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    // ---- the log has to be writable at all ------------------------------
    // /dev/kmsg is 0644 root-owned, as on Linux, so an unprivileged run
    // cannot inject the messages every wait below is woken by. The suite is
    // not always root (the CLI harness is, an ssh session into the app is
    // not), so report that as a skip rather than a wall of failures.
    if (kmsg_say("syslog_read_blocks: start") != 0) {
        if (geteuid() != 0) {
            printf("syslog_read_blocks: SKIP (unprivileged: cannot write "
                   "/dev/kmsg, %s)\n", strerror(errno));
            return 0;
        }
        ck("/dev/kmsg accepts a write", 0, 1);
        return finish_suite("syslog_read_blocks");
    }
    // Linux gates every syslog(2) type on CAP_SYSLOG when dmesg_restrict is
    // set; measured as EPERM for all of them, including the ones that take no
    // buffer. AOK does not implement that check, so this only ever trips on
    // the oracle when it is run unprivileged.
    if (klogctl_(SYSLOG_SIZE_BUFFER, NULL, 0) < 0 && errno == EPERM) {
        printf("syslog_read_blocks: SKIP (unprivileged: syslog(2) is "
               "EPERM)\n");
        return 0;
    }

    // Only now: the two skips above return without killing it, and a watchdog
    // outliving its victim would eventually SIGKILL whatever pid got recycled.
    pid_t watchdog = watchdog_start(test_watchdog_secs(90));

    char buf[8192];

    // ---- arguments are checked before anything waits ---------------------
    // All three answer at once on a log with nothing unread (measured on
    // 6.12). They mattered little while the read could not block; now that it
    // can, a guest passing a zero length or a null buffer would wait forever.
    {
        ck("drained the log", drain(), 0);
        long t0 = now_ms();
        errno = 0;
        ck("READ len=0 returns 0", klogctl_(SYSLOG_READ, buf, 0), 0);
        errno = 0;
        int r = klogctl_(SYSLOG_READ, NULL, 16);
        ck("READ buf=NULL is EINVAL", r < 0 ? -errno : r, -EINVAL);
        errno = 0;
        r = klogctl_(SYSLOG_READ, buf, -1);
        ck("READ len<0 is EINVAL", r < 0 ? -errno : r, -EINVAL);
        ck_le("  and none of them waited (ms)", now_ms() - t0, 200);
    }

    // ---- READ_ALL and READ_CLEAR never wait ------------------------------
    // Only type 2 waits on Linux; 3 and 4 peek at what is buffered and return,
    // 0 included. A guest running `dmesg` on a drained log must not hang.
    {
        ck("drained the log", drain(), 0);
        long t0 = now_ms();
        int all = klogctl_(SYSLOG_READ_ALL, buf, (int) sizeof buf);
        int clr = klogctl_(SYSLOG_READ_CLEAR, buf, (int) sizeof buf);
        ck("READ_ALL does not wait", all >= 0 ? 1 : 0, 1);
        ck("READ_CLEAR does not wait", clr >= 0 ? 1 : 0, 1);
        ck_le("  neither of them waited (ms)", now_ms() - t0, 200);
    }

    // ---- data already there comes back at once ---------------------------
    {
        ck("drained the log", drain(), 0);
        ck("wrote a line", kmsg_say("syslog_read_blocks: pre-queued"), 0);
        long t0 = now_ms();
        int n = klogctl_(SYSLOG_READ, buf, (int) sizeof buf);
        ck("READ with data buffered returns it", n > 0 ? 1 : 0, 1);
        ck_le("  without waiting (ms)", now_ms() - t0, 500);
        if (n > 0) {
            buf[n] = '\0';
            ck("  and it is the line written",
               strstr(buf, "syslog_read_blocks: pre-queued") != NULL ? 1 : 0, 1);
        }
        // A big buffer and one small record: the read returns short rather
        // than waiting for enough to fill it. klogd passes 4 KiB every time.
        ck("  returned short, did not wait to fill the buffer",
           n < (int) sizeof buf ? 1 : 0, 1);
    }

    // ---- a len smaller than one record returns a prefix ------------------
    // Measured on 6.12: READ with len 8 on a 70-byte record returns exactly 8
    // bytes, and SIZE_UNREAD drops by exactly 8 -- the rest stays for the next
    // read. This kernel's log is a byte stream, so it agrees for free; the
    // check is here because a record-oriented rewrite would break it.
    {
        ck("drained the log", drain(), 0);
        ck("wrote a long line",
           kmsg_say("syslog_read_blocks: abcdefghijklmnopqrstuvwxyz0123456789"), 0);
        int before = klogctl_(SYSLOG_SIZE_UNREAD, NULL, 0);
        ck("SIZE_UNREAD sees it", before > 8 ? 1 : 0, 1);
        int n = klogctl_(SYSLOG_READ, buf, 8);
        ck("READ len=8 returns 8", n, 8);
        int after = klogctl_(SYSLOG_SIZE_UNREAD, NULL, 0);
        ck("  and SIZE_UNREAD dropped by exactly 8", before - after, 8);
        n = klogctl_(SYSLOG_READ, buf, (int) sizeof buf);
        ck("  the rest is still there", n, after);
        if (n > 0) {
            buf[n] = '\0';
            ck("  and carries the tail of the line",
               strstr(buf, "uvwxyz0123456789") != NULL ? 1 : 0, 1);
        }
    }

    // ---- the whole point: a drained READ waits, ASLEEP -------------------
    //
    // Three separate claims, and the spin failed all three: the call must not
    // return 0, the task must read as S rather than R, and it must burn no
    // CPU while it waits.
    {
        enum { WAIT_MS = 1500, SAMPLES = 10 };
        ck("drained the log", drain(), 0);

        int pipefd[2];
        ck("pipe", pipe(pipefd), 0);
        fflush(stdout);
        pid_t child = fork();
        ck("fork", child >= 0 ? 1 : 0, 1);
        if (child == 0) {
            close(pipefd[0]);
            struct read_result res = { 0, 0, 0, 0 };
            char cbuf[8192];
            long t0 = now_ms();
            // Loop rather than read once: any other message landing first is
            // a legitimate wakeup, and would otherwise look like a pass for
            // the wrong reason. Bounded, so the spin cannot loop forever.
            for (int i = 0; i < 100000 && !res.found_marker; i++) {
                errno = 0;
                int n = klogctl_(SYSLOG_READ, cbuf, (int) sizeof cbuf - 1);
                if (n == 0) {
                    res.zero_returns++;
                    if (res.zero_returns > 1000)
                        break;  // spinning: stop burning the core
                    continue;
                }
                if (n < 0) {
                    res.err = errno;
                    break;
                }
                cbuf[n] = '\0';
                if (strstr(cbuf, "syslog_read_blocks: the-awaited-line") != NULL)
                    res.found_marker = 1;
            }
            res.elapsed_ms = now_ms() - t0;
            ssize_t w = write(pipefd[1], &res, sizeof res);
            close(pipefd[1]);
            _exit(w == (ssize_t) sizeof res ? 0 : 1);
        }
        close(pipefd[1]);

        // Watch it wait. A spinner is R in every sample and climbs ~100 ticks
        // a second; a sleeper is S and flat.
        int s_samples = 0, r_samples = 0;
        long ticks_first = -1, ticks_last = -1;
        for (int i = 0; i < SAMPLES; i++) {
            sleep_ms(WAIT_MS / SAMPLES);
            char state = '?';
            long ticks = 0;
            if (proc_stat(child, &state, &ticks) != 0)
                continue;
            if (state == 'S')
                s_samples++;
            else if (state == 'R')
                r_samples++;
            if (ticks_first < 0)
                ticks_first = ticks;
            ticks_last = ticks;
        }

        ck("wrote the line it was waiting for",
           kmsg_say("syslog_read_blocks: the-awaited-line"), 0);

        struct read_result res = { 0, 0, 0, -1 };
        ssize_t got = read(pipefd[0], &res, sizeof res);
        close(pipefd[0]);
        int status = 0;
        waitpid(child, &status, 0);

        ck("the waiting read reported back", got == (ssize_t) sizeof res ? 1 : 0, 1);
        ck("  it got the line, not an error", res.found_marker, 1);
        ck("  errno on the way", res.err, 0);
        // The failure this file exists for.
        ck("  and never once returned 0", res.zero_returns, 0);
        ck_ge("  it waited for the line to arrive (ms)", res.elapsed_ms,
              WAIT_MS * 4 / 5);
        // Sampled state. A spin gives s_samples == 0.
        ck_ge("  and was asleep, not running (S samples)", s_samples,
              SAMPLES / 2 + 1);
        test_logf("  %-56s S=%d R=%d ticks %ld..%ld\n",
                  "state samples", s_samples, r_samples, ticks_first, ticks_last);
        // The decisive one, and the only check immune to a busy host: waiting
        // costs no CPU. The spin measured 100 ticks a second, so 1.5s of it
        // would be ~150 here.
        ck_le("  burning no CPU while it waited (ticks)",
              ticks_first >= 0 ? ticks_last - ticks_first : 0, 20);
    }

    // ---- a signal ends the wait: EINTR without SA_RESTART ----------------
    // SIGUSR1 from a child, not alarm(), so SIGALRM stays free for a watchdog.
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_usr1;
        sa.sa_flags = 0;  // no SA_RESTART
        ck("sigaction SIGUSR1 without SA_RESTART", sigaction(SIGUSR1, &sa, NULL), 0);
        ck("drained the log", drain(), 0);

        usr1_count = 0;
        pid_t me = getpid();
        fflush(stdout);
        pid_t poker = fork();
        if (poker == 0) {
            sleep_ms(500);
            kill(me, SIGUSR1);
            _exit(0);
        }
        long t0 = now_ms();
        errno = 0;
        int n = klogctl_(SYSLOG_READ, buf, (int) sizeof buf);
        int e = errno;
        long waited = now_ms() - t0;
        int status = 0;
        waitpid(poker, &status, 0);

        ck("READ interrupted by a handler is EINTR", n < 0 ? -e : n, -EINTR);
        ck("  the handler ran", (long) usr1_count, 1);
        ck_ge("  and it had really been waiting (ms)", waited, 300);
    }

    // ---- with SA_RESTART it resumes instead of failing -------------------
    // Linux ends the wait with -ERESTARTSYS, so SA_RESTART decides whether the
    // guest ever sees the interruption. Measured on 6.12: the read came back
    // 2.0s later with the message written then, having run the handler at 1.0s.
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_usr1;
        sa.sa_flags = SA_RESTART;
        ck("sigaction SIGUSR1 with SA_RESTART", sigaction(SIGUSR1, &sa, NULL), 0);
        ck("drained the log", drain(), 0);

        usr1_count = 0;
        pid_t me = getpid();
        fflush(stdout);
        pid_t helper = fork();
        if (helper == 0) {
            sleep_ms(500);
            kill(me, SIGUSR1);
            sleep_ms(700);
            kmsg_say("syslog_read_blocks: after-the-restart");
            _exit(0);
        }
        long t0 = now_ms();
        errno = 0;
        int n = klogctl_(SYSLOG_READ, buf, (int) sizeof buf);
        int e = errno;
        long waited = now_ms() - t0;
        int status = 0;
        waitpid(helper, &status, 0);

        ck("READ restarts across an SA_RESTART handler", n > 0 ? 1 : -e, 1);
        ck("  the handler ran", (long) usr1_count, 1);
        if (n > 0) {
            buf[n] = '\0';
            ck("  and it returned the later message",
               strstr(buf, "syslog_read_blocks: after-the-restart") != NULL ? 1 : 0, 1);
        }
        // Past the signal at 500ms, out to the message at 1200ms: proof it
        // resumed the wait rather than answering the interruption.
        ck_ge("  having waited past the signal (ms)", waited, 900);
    }

    // ---- leave the log non-empty -----------------------------------------
    // Every drain above is DESTRUCTIVE -- unlike /dev/kmsg's position-based
    // reads, SYSLOG_ACTION_READ takes the bytes out of the ring buffer. Left
    // empty, the next test to run would find one: kmsg_stream asserts that a
    // fresh /dev/kmsg reader sees buffered messages, and an emptied buffer has
    // none to see. The suite shares one guest, so this is not hypothetical.
    {
        for (int i = 0; i < 8; i++)
            kmsg_say("syslog_read_blocks: refilling the log for the next test");
        ck_ge("left lines in the log for the next test",
              klogctl_(SYSLOG_SIZE_UNREAD, NULL, 0), 1);
    }

    if (watchdog > 0) {
        kill(watchdog, SIGKILL);
        waitpid(watchdog, NULL, 0);
    }
    return finish_suite("syslog_read_blocks");
}
