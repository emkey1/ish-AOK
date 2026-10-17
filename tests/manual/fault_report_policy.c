// The kernel log reports a guest fault only when the fault kills its program.
//
// AOK wrote a full report -- the line, registers, an opcode window, the
// stack -- for every guest fault, the ones a program takes on purpose and
// catches included. A JVM's null checks, or a test that raises 61,440 #UDs
// to read what each one reports, filled the log; a distro's syslog daemon
// wrote it to disk twice, and three suite runs left 1.3 GB in /var/log on a
// phone with 1.7 GB free. Linux prints for a fault only when nobody will
// handle it (unhandled_signal()), and ratelimits that.
//
// /proc/ish/fault_reports is the switch: "unhandled" (the default), "all"
// (every fault, as it was), "none". Each case below faults at an address of
// its own in a child, and looks for "page fault on <that address>" in the
// kernel log afterwards:
//
//   unhandled mode
//     a handler catches it, fifty times     no report
//     no handler: the child dies            one report (the control: the
//                                           log is being read, and reports
//                                           do get there)
//     a handler, but SIGSEGV is blocked     one report -- it kills too
//     a handled SIGILL                      no "illegal instruction" line
//                                           for that child
//     thirty unhandled faults at once       ten reports at most, and the
//                                           file's `suppressed` count rises
//                                           by the rest
//   all mode:  a handled fault is reported
//   none mode: an unhandled one is not
//
// Needs root (it sets the switch and reads the log). Leaves the switch as it
// found it.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SWITCH "/proc/ish/fault_reports"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("fault_report_policy: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static int set_mode(const char *mode) {
    int fd = open(SWITCH, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, mode, strlen(mode));
    close(fd);
    return n == (ssize_t) strlen(mode) ? 0 : -1;
}

static int get_mode(char *mode, size_t size, unsigned long *suppressed) {
    FILE *f = fopen(SWITCH, "r");
    if (f == NULL)
        return -1;
    char line[64];
    mode[0] = '\0';
    *suppressed = 0;
    if (fgets(line, sizeof(line), f) != NULL)
        sscanf(line, "%63s", mode);
    if (fgets(line, sizeof(line), f) != NULL)
        sscanf(line, "suppressed %lu", suppressed);
    fclose(f);
    (void) size;
    return mode[0] != '\0' ? 0 : -1;
}

// How many times `needle` is in the kernel log now.
static int log_count(const char *needle) {
    int size = klogctl(10 /* SYSLOG_ACTION_SIZE_BUFFER */, NULL, 0);
    if (size <= 0)
        size = 1 << 20;
    char *buf = malloc((size_t) size + 1);
    if (buf == NULL)
        return -1;
    int n = klogctl(3 /* SYSLOG_ACTION_READ_ALL */, buf, size);
    if (n < 0) {
        free(buf);
        return -1;
    }
    buf[n] = '\0';
    int count = 0;
    for (char *p = buf; (p = strstr(p, needle)) != NULL; p += strlen(needle))
        count++;
    free(buf);
    return count;
}

static sigjmp_buf back;
static void on_fault(int sig) {
    (void) sig;
    siglongjmp(back, 1);
}

enum how { HANDLED, UNHANDLED, BLOCKED };

// A child that writes to `addr` `times` times. Returns its wait status.
static int fault_in_child(uintptr_t addr, enum how how, int times) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (how != UNHANDLED) {
            struct sigaction sa = {.sa_handler = on_fault};
            sigemptyset(&sa.sa_mask);
            sigaction(SIGSEGV, &sa, NULL);
        }
        if (how == BLOCKED) {
            sigset_t set;
            sigemptyset(&set);
            sigaddset(&set, SIGSEGV);
            sigprocmask(SIG_BLOCK, &set, NULL);
        }
        for (volatile int i = 0; i < times; i++) {
            if (sigsetjmp(back, 1) == 0)
                *(volatile int *) addr = 1;
        }
        _exit(0);
    }
    int status = -1;
    waitpid(pid, &status, 0);
    return status;
}

static int reports_for(uintptr_t addr) {
    char needle[64];
    snprintf(needle, sizeof(needle), "page fault on %#llx ", (unsigned long long) addr);
    return log_count(needle);
}

int main(void) {
    char was[64], now[64];
    unsigned long suppressed_before = 0, suppressed_after = 0;
    if (get_mode(was, sizeof(was), &suppressed_before) != 0) {
        printf("fault_report_policy: SKIP (no " SWITCH ")\n");
        return 0;
    }
    if (geteuid() != 0 || set_mode("unhandled") != 0 || log_count("\n") < 0) {
        printf("fault_report_policy: SKIP (needs root: to set the switch and read the kernel log)\n");
        return 0;
    }
    CHECK(get_mode(now, sizeof(now), &suppressed_before) == 0 && strcmp(now, "unhandled") == 0,
          "the switch reads \"%s\" after writing unhandled", now);
    CHECK(set_mode("loud") != 0 && errno == EINVAL, "a mode that is not one was accepted");

    // Addresses nothing maps: low, page-aligned, and a different one each.
    const uintptr_t a_handled = 0x5a5a1000, a_unhandled = 0x6b6b2000, a_blocked = 0x7c7c3000,
                    a_burst = 0x4d4d4000, a_all = 0x3e3e5000, a_none = 0x2f2f6000;

    int status = fault_in_child(a_handled, HANDLED, 50);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the handled faults: status %#x, wanted exit 0", status);
    CHECK(reports_for(a_handled) == 0, "fifty faults a handler caught left %d reports in the log", reports_for(a_handled));

    status = fault_in_child(a_unhandled, UNHANDLED, 1);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "the unhandled fault: status %#x, wanted killed by SIGSEGV", status);
    CHECK(reports_for(a_unhandled) == 1, "a fault that killed its program left %d reports, wanted 1 (the control)",
          reports_for(a_unhandled));

    status = fault_in_child(a_blocked, BLOCKED, 1);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "the blocked fault: status %#x, wanted killed by SIGSEGV", status);
    CHECK(reports_for(a_blocked) == 1, "a fault whose handler was blocked left %d reports, wanted 1", reports_for(a_blocked));

    // A handled illegal instruction: an encoding undefined on every guest
    // architecture here (all-zero bytes are, on x86 only as a pair -- so use
    // the one each defines for the purpose).
    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction sa = {.sa_handler = on_fault};
        sigemptyset(&sa.sa_mask);
        sigaction(SIGILL, &sa, NULL);
        if (sigsetjmp(back, 1) == 0) {
#if defined(__x86_64__) || defined(__i386__)
            __asm__ volatile("ud2");
#elif defined(__aarch64__)
            __asm__ volatile(".inst 0x00000000"); // udf #0
#elif defined(__riscv)
            __asm__ volatile(".4byte 0x00000000"); // defined illegal
#elif defined(__powerpc64__)
            __asm__ volatile(".long 0x00000000"); // primary opcode 0: illegal
#else
            raise(SIGILL);
#endif
            _exit(3); // it did not fault
        }
        _exit(0);
    }
    status = -1;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the handled SIGILL: status %#x, wanted exit 0", status);
    char needle[64];
    snprintf(needle, sizeof(needle), "ERROR: %d(", (int) pid);
    CHECK(log_count(needle) == 0, "a handled illegal instruction left %d report lines for pid %d", log_count(needle), (int) pid);

    // The rate limit: a fresh window, then thirty at once.
    struct timespec pause = {.tv_sec = 5, .tv_nsec = 300000000};
    nanosleep(&pause, NULL);
    get_mode(now, sizeof(now), &suppressed_before);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 30; i++)
        fault_in_child(a_burst, UNHANDLED, 1);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double took = (double) (t1.tv_sec - t0.tv_sec) + (double) (t1.tv_nsec - t0.tv_nsec) / 1e9;
    get_mode(now, sizeof(now), &suppressed_after);
    int burst = reports_for(a_burst);
    CHECK(burst >= 1, "thirty unhandled faults left no report at all");
    CHECK((unsigned long) burst + (suppressed_after - suppressed_before) == 30,
          "thirty faults: %d reported and %lu counted as suppressed, which is not thirty",
          burst, suppressed_after - suppressed_before);
    if (took < 4.5)
        CHECK(burst == 10, "thirty faults in %.1f s left %d reports, wanted the 10 the limit allows", took, burst);
    else
        printf("fault_report_policy: (the burst took %.1f s, longer than one window; %d reported)\n", took, burst);

    CHECK(set_mode("all") == 0, "could not set all");
    status = fault_in_child(a_all, HANDLED, 3);
    CHECK(reports_for(a_all) == 3, "in all mode, three handled faults left %d reports, wanted 3", reports_for(a_all));

    CHECK(set_mode("none") == 0, "could not set none");
    status = fault_in_child(a_none, UNHANDLED, 1);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV, "none mode: status %#x, wanted killed by SIGSEGV", status);
    CHECK(reports_for(a_none) == 0, "in none mode, an unhandled fault left %d reports", reports_for(a_none));

    set_mode(was);
    if (failures != 0) {
        printf("fault_report_policy: FAIL failures=%d\n", failures);
        return 1;
    }
    printf("fault_report_policy: PASS\n");
    return 0;
}
