/*
 * proc_read_snapshot -- a procfs file read in pieces must come from ONE
 * rendering, not from a fresh one per read(2).
 *
 * Linux renders a single_open seq_file once per read pass: seq_read_iter
 * fills seq_file::buf at the read that starts at offset 0, and every later
 * read of the pass copies out of that buffer (fs/seq_file.c). AOK used to
 * re-run the entry's show() on every read(2) AND every lseek(2), so a reader
 * taking a /proc file in small pieces spliced together as many different
 * renderings as it made calls.
 *
 * That is invisible while the renderings have the same shape, and corrupts
 * the file the moment a field earlier in it changes WIDTH: the tail shifts
 * under the reader, and the byte at the offset it asks for next is the one it
 * already has (duplicated) or the one after (lost). Seen in a Devuan guest on
 * 2026-09-22, where dash's `read` builtin reads one byte at a time:
 *
 *   while read -r k v; do [ "$k" = btime ] && echo "BTIME=$v"; done </proc/stat
 *
 * printed BTIME=17990071649 for a btime of 1790071649 -- one duplicated digit,
 * because a counter printed above btime grew a digit between two of those
 * reads. Any shell script reading /proc with `read` was exposed.
 *
 * Three phases:
 *
 * 1. Deterministic. /proc/<pid>/status opens with "Name:\t<comm>\n", and
 *    prctl(PR_SET_NAME) changes that comm -- so this process can change the
 *    width of the file's FIRST line whenever it likes. Read the file one byte
 *    at a time, flipping the name between a short and a long one after every
 *    byte. A kernel that renders per pass hands back exactly the snapshot
 *    taken at offset 0, name and all; a kernel that re-renders per read hands
 *    back a splice of two files that differ by fourteen bytes at offset six,
 *    which stops being a /proc/<pid>/status by the second line.
 *
 * 2. Freshness, so the fix cannot be "cache it and never look again". A read
 *    starting at offset 0 must re-render (this is the lseek(0)+read refresh
 *    loop that pollers run), and so must a pread that does not continue where
 *    the last read stopped -- both are exactly where seq_read_iter resets or
 *    calls traverse().
 *
 * 3. The reported symptom. /proc/stat read one byte at a time while other
 *    processes churn its counters: every line must keep its exact shape and
 *    btime must equal what a single read(2) reports. Sensitivity comes from
 *    making procs_blocked straddle a power of ten, which is the only width
 *    change in that file anyone can arrange -- every other counter there is
 *    monotonic and crosses a decade about once per decade.
 *
 * Also passes on real Linux, which is where the required behaviour comes from.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#define BUF_SIZE 65536
#define MAX_LINES 512
#define MAX_REPORTS 6

// Two comms of very different length. PR_SET_NAME truncates at 16 bytes
// including the NUL, so fifteen characters is the longest name there is.
#define COMM_SHORT "s"
#define COMM_LONG  "abcdefghijklmno"

#define STAT_PASSES 400UL        // upper bound; the phase is time-boxed below
#define STAT_SECONDS 8
#define STAT_TAIL_BYTES 80       // slow the reader down over the tail, where
#define STAT_TAIL_DELAY_US 400   // the width changes below actually land
#define FORK_CHURNERS 2
#define TOGGLERS 4
#define MAX_SLEEPERS 64
#define TOGGLE_US 300

static char assembled[BUF_SIZE];
static char reference[BUF_SIZE];
static char scratch[BUF_SIZE];

// ---- small helpers ---------------------------------------------------------

static void sleep_us(long us) {
    struct timespec ts = {us / 1000000, (us % 1000000) * 1000};
    nanosleep(&ts, NULL);
}

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static void set_comm(const char *name) {
    prctl(PR_SET_NAME, name, 0, 0, 0);
}

// /proc/<pid>/status opens with the comm, so this is "the rendering this text
// came from had that name".
static int names_the_comm(const char *text, const char *name) {
    char want[64];
    int n = snprintf(want, sizeof(want), "Name:\t%s\n", name);
    return n > 0 && strncmp(text, want, (size_t) n) == 0;
}

// One read(2) of the whole file, which is the self-consistent reading every
// kernel gives: one call, one rendering. This is the "single-read grep" the
// byte-at-a-time passes are checked against.
static ssize_t read_whole_once(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return n;
}

// Read to EOF from an already-open description, leaving its offset at EOF.
static ssize_t read_rest(int fd, char *buf, size_t cap) {
    size_t len = 0;
    for (;;) {
        ssize_t n = read(fd, buf + len, cap - 1 - len);
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        len += (size_t) n;
        if (len >= cap - 1)
            break;
    }
    buf[len] = '\0';
    return (ssize_t) len;
}

// Split text into NUL-terminated lines in place. Returns the count, or -1 if
// the text is not a sequence of nonempty newline-terminated lines -- which a
// splice can produce on its own, by duplicating or swallowing a newline.
static int split_lines(char *text, size_t len, char **lines, int max) {
    if (len == 0 || text[len - 1] != '\n')
        return -1;
    int n = 0;
    char *p = text;
    while (*p != '\0') {
        char *nl = strchr(p, '\n');
        if (nl == NULL || nl == p)
            return -1;
        if (n >= max)
            return -1;
        *nl = '\0';
        lines[n++] = p;
        p = nl + 1;
    }
    return n;
}

// Exactly `count` fields, each a nonempty run of digits, separated by exactly
// one space, with nothing after the last. A byte duplicated or lost inside a
// run of digits only changes a value; anywhere else -- a label, a separator, a
// newline -- it changes this shape, which is why the check is this strict.
static int digit_fields(const char *s, int count) {
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            if (*s != ' ')
                return 0;
            s++;
        }
        if (*s < '0' || *s > '9')
            return 0;
        while (*s >= '0' && *s <= '9')
            s++;
    }
    return *s == '\0';
}

// ---- phase 1: /proc/self/status under a comm that keeps changing width -----

// /proc/<pid>/status is entirely "Key:\tValue" lines. Returns the key length,
// or 0 if the line does not have that shape.
static size_t status_key_len(const char *line) {
    size_t i = 0;
    while ((line[i] >= 'A' && line[i] <= 'Z') || (line[i] >= 'a' && line[i] <= 'z') ||
           (line[i] >= '0' && line[i] <= '9') || line[i] == '_')
        i++;
    if (i == 0 || line[i] != ':' || line[i + 1] != '\t')
        return 0;
    return i;
}

// Compares the assembled text's line KEYS against a single-read reference.
// Values are deliberately not compared: FDSize, VmRSS and friends move on
// their own, and it is the file's shape that a splice destroys.
static int check_status_text(const char *text, size_t len, const char *what,
                             char **ref_lines, int ref_count, const char *expect_name) {
    int problems = 0;
    memcpy(scratch, text, len);
    scratch[len] = '\0';
    char *lines[MAX_LINES];
    int count = split_lines(scratch, len, lines, MAX_LINES);
    if (count < 0) {
        test_log_if(1, "  %s: not a sequence of nonempty lines (%zu bytes)\n", what, len);
        return 1;
    }
    if (count != ref_count) {
        test_log_if(1, "  %s: %d lines, single read gives %d\n", what, count, ref_count);
        problems++;
    }
    for (int i = 0; i < count; i++) {
        size_t key = status_key_len(lines[i]);
        if (key == 0) {
            test_log_if(problems < MAX_REPORTS, "  %s: line %d is not \"Key:<tab>...\": \"%s\"\n",
                        what, i, lines[i]);
            problems++;
            continue;
        }
        if (i < ref_count && strncmp(lines[i], ref_lines[i], key + 2) != 0) {
            test_log_if(problems < MAX_REPORTS, "  %s: line %d key is \"%.*s\", expected \"%.*s\"\n",
                        what, i, (int) key, lines[i],
                        (int) status_key_len(ref_lines[i]), ref_lines[i]);
            problems++;
        }
    }
    // The whole point: the name belongs to the rendering the pass STARTED on.
    if (count > 0) {
        char want[64];
        snprintf(want, sizeof(want), "Name:\t%s", expect_name);
        if (strcmp(lines[0], want) != 0) {
            test_log_if(1, "  %s: first line is \"%s\", expected \"%s\"\n", what, lines[0], want);
            problems++;
        }
    }
    return problems;
}

static void phase_status_splice(void) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int) getpid());

    set_comm(COMM_SHORT);
    ssize_t ref_len = read_whole_once(path, reference, sizeof(reference));
    if (ref_len <= 0) {
        failf("read /proc/<pid>/status", 0, 0, 0, 1, 0, 0);
        return;
    }
    char *ref_lines[MAX_LINES];
    int ref_count = split_lines(reference, (size_t) ref_len, ref_lines, MAX_LINES);
    if (ref_count <= 0) {
        failf("single read of status is not lines", 0, 0, 0, 1, 0, 0);
        return;
    }

    // Positive control for the pass below: it can only detect a splice if the
    // comm really does move the file's first line. A kernel that ignored
    // PR_SET_NAME, or a status that did not print the comm, would make every
    // check here pass while testing nothing at all.
    set_comm(COMM_LONG);
    ssize_t long_len = read_whole_once(path, scratch, sizeof(scratch));
    set_comm(COMM_SHORT);
    if (long_len <= ref_len || !names_the_comm(scratch, COMM_LONG)) {
        test_log_if(1, "  long-name rendering: %zd bytes, \"%.32s\"\n",
                    long_len, long_len > 0 ? scratch : "");
        failf("PR_SET_NAME does not change /proc/<pid>/status's width",
              (uint64_t) long_len, (uint64_t) ref_len, 0, 0, 0, 0);
        return;
    }
    test_logf("status: %zd bytes short-named, %zd long-named\n", ref_len, long_len);

    // Offset 0 is read while the comm is short, so the pass's snapshot is the
    // short one. Every byte after that is read with the file's first line
    // changing width underneath.
    set_comm(COMM_SHORT);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        failf("open /proc/<pid>/status", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }
    size_t len = 0;
    int toggle = 0;
    unsigned long flips = 0;
    for (;;) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0) {
            failf("read status one byte at a time", (uint64_t) errno, 0, 0, 0, 0, 0);
            close(fd);
            return;
        }
        if (n == 0)
            break;
        assembled[len++] = c;
        if (len >= sizeof(assembled) - 1)
            break;
        toggle ^= 1;
        set_comm(toggle ? COMM_LONG : COMM_SHORT);
        flips++;
    }
    set_comm(COMM_SHORT);
    test_logf("status: assembled %zu bytes one byte at a time, %lu name flips\n", len, flips);

    int problems = check_status_text(assembled, len, "status byte-at-a-time",
                                     ref_lines, ref_count, COMM_SHORT);
    if (problems != 0)
        failf("status: byte-at-a-time read spliced several renderings", problems, 0, 0, 0, 0, 0);

    // Freshness on the SAME description. Each of these is a place Linux
    // re-renders, and a poller that never reopens depends on every one.
    set_comm(COMM_LONG);
    if (lseek(fd, 0, SEEK_SET) != 0) {
        failf("lseek(status, 0)", (uint64_t) errno, 0, 0, 0, 0, 0);
    } else {
        ssize_t n = read_rest(fd, assembled, sizeof(assembled));
        if (n <= 0 || !names_the_comm(assembled, COMM_LONG)) {
            test_log_if(1, "  after lseek(0)+read: \"%.32s\"\n", n > 0 ? assembled : "");
            failf("lseek(0)+read did not re-render", 0, 0, 0, 1, 0, 0);
        }
    }

    set_comm(COMM_SHORT);
    ssize_t n = pread(fd, assembled, sizeof(assembled) - 1, 0);
    if (n > 0)
        assembled[n] = '\0';
    if (n <= 0 || !names_the_comm(assembled, COMM_SHORT)) {
        test_log_if(1, "  after pread(.., 0): \"%.32s\"\n", n > 0 ? assembled : "");
        failf("pread at offset 0 did not re-render", 0, 0, 0, 1, 0, 0);
    }

    // A pread that lands where the last read did NOT stop is seq_read_iter's
    // traverse() case: it re-renders too. Offset 6 is one past "Name:\t", so
    // what starts there is the comm of whatever rendering answers.
    set_comm(COMM_LONG);
    n = pread(fd, assembled, sizeof(assembled) - 1, 6);
    if (n > 0)
        assembled[n] = '\0';
    if (n <= 0 || strncmp(assembled, COMM_LONG "\n", sizeof(COMM_LONG)) != 0) {
        test_log_if(1, "  after pread(.., 6): \"%.32s\"\n", n > 0 ? assembled : "");
        failf("pread at an unrelated offset did not re-render", 0, 0, 0, 1, 0, 0);
    }
    close(fd);

    // And a fresh open always starts a fresh pass.
    set_comm(COMM_SHORT);
    n = read_whole_once(path, assembled, sizeof(assembled));
    if (n <= 0 || !names_the_comm(assembled, COMM_SHORT)) {
        test_log_if(1, "  after reopen: \"%.32s\"\n", n > 0 ? assembled : "");
        failf("a reopened status did not re-render", 0, 0, 0, 1, 0, 0);
    }
}

// ---- phase 3: /proc/stat under churn ---------------------------------------

struct stat_shape {
    int ncpu;
    char intr_line[4096];
    char softirq_line[256];
    char btime_line[64];
};

static const char *const stat_tail_labels[] = {
    "ctxt ", "btime ", "processes ", "procs_running ", "procs_blocked ",
};
#define STAT_TAIL_LABELS ((int) (sizeof(stat_tail_labels) / sizeof(stat_tail_labels[0])))

// Learns the constant parts of this guest's /proc/stat from a single read.
// Taking them from the file rather than hardcoding them means the check
// follows the kernel if it ever prints a different number of interrupt or
// softirq columns.
static int stat_shape_learn(struct stat_shape *shape, const char *text, size_t len) {
    memcpy(scratch, text, len);
    scratch[len] = '\0';
    char *lines[MAX_LINES];
    int count = split_lines(scratch, len, lines, MAX_LINES);
    if (count < 0)
        return -1;
    memset(shape, 0, sizeof(*shape));
    for (int i = 1; i < count; i++)
        if (strncmp(lines[i], "cpu", 3) == 0 && lines[i][3] >= '0' && lines[i][3] <= '9')
            shape->ncpu++;
    if (count != shape->ncpu + 8)
        return -1;
    if (strlen(lines[shape->ncpu + 1]) >= sizeof(shape->intr_line))
        return -1;
    strcpy(shape->intr_line, lines[shape->ncpu + 1]);
    if (strlen(lines[count - 1]) >= sizeof(shape->softirq_line))
        return -1;
    strcpy(shape->softirq_line, lines[count - 1]);
    if (strlen(lines[shape->ncpu + 3]) >= sizeof(shape->btime_line))
        return -1;
    strcpy(shape->btime_line, lines[shape->ncpu + 3]);
    if (strncmp(shape->intr_line, "intr ", 5) != 0 ||
        strncmp(shape->softirq_line, "softirq ", 8) != 0 ||
        strncmp(shape->btime_line, "btime ", 6) != 0)
        return -1;
    return 0;
}

// btime_a and btime_b bracket the pass: single reads taken just before and
// just after it. A byte-at-a-time btime has to equal one of them. Bracketing
// rather than tolerating is what keeps this exact (a spliced btime matches
// neither) while still surviving a host clock step across the pass, which
// moves the boot instant AOK derives by subtraction.
static int check_stat_text(const char *text, size_t len, const char *what,
                           const struct stat_shape *shape,
                           const char *btime_a, const char *btime_b) {
    int problems = 0;
    memcpy(scratch, text, len);
    scratch[len] = '\0';
    char *lines[MAX_LINES];
    int count = split_lines(scratch, len, lines, MAX_LINES);
    if (count < 0) {
        test_log_if(1, "  %s: not a sequence of nonempty lines (%zu bytes)\n", what, len);
        return 1;
    }
    if (count != shape->ncpu + 8) {
        test_log_if(1, "  %s: %d lines, expected %d\n", what, count, shape->ncpu + 8);
        return problems + 1;
    }

    if (strncmp(lines[0], "cpu  ", 5) != 0 || !digit_fields(lines[0] + 5, 8)) {
        test_log_if(problems < MAX_REPORTS, "  %s: bad aggregate line: \"%s\"\n", what, lines[0]);
        problems++;
    }
    for (int i = 0; i < shape->ncpu; i++) {
        char prefix[24];
        snprintf(prefix, sizeof(prefix), "cpu%d  ", i);
        size_t plen = strlen(prefix);
        if (strncmp(lines[1 + i], prefix, plen) != 0 || !digit_fields(lines[1 + i] + plen, 8)) {
            test_log_if(problems < MAX_REPORTS, "  %s: bad %s line: \"%s\"\n",
                        what, prefix, lines[1 + i]);
            problems++;
        }
    }
    // Constant lines, compared byte for byte: a splice landing anywhere in
    // them shows up here even when it only duplicates a digit or a space.
    if (strcmp(lines[shape->ncpu + 1], shape->intr_line) != 0) {
        test_log_if(problems < MAX_REPORTS, "  %s: intr line changed: \"%s\"\n",
                    what, lines[shape->ncpu + 1]);
        problems++;
    }
    if (strcmp(lines[shape->ncpu + 8 - 1], shape->softirq_line) != 0) {
        test_log_if(problems < MAX_REPORTS, "  %s: softirq line changed: \"%s\"\n",
                    what, lines[shape->ncpu + 8 - 1]);
        problems++;
    }
    for (int i = 0; i < STAT_TAIL_LABELS; i++) {
        const char *line = lines[shape->ncpu + 2 + i];
        size_t plen = strlen(stat_tail_labels[i]);
        if (strncmp(line, stat_tail_labels[i], plen) != 0 || !digit_fields(line + plen, 1)) {
            test_log_if(problems < MAX_REPORTS, "  %s: bad \"%s\" line: \"%s\"\n",
                        what, stat_tail_labels[i], line);
            problems++;
        }
    }
    // The reported symptom, checked by name.
    const char *btime = lines[shape->ncpu + 3];
    if (strcmp(btime, btime_a) != 0 && strcmp(btime, btime_b) != 0) {
        test_log_if(1, "  %s: %s, single reads either side give %s / %s\n",
                    what, btime, btime_a, btime_b);
        problems++;
    }
    return problems;
}

// The whole "btime <n>" line, from a single read(2) -- so it is one
// rendering's answer, the reading every kernel agrees on.
static int stat_btime_line(char *out, size_t cap) {
    ssize_t n = read_whole_once("/proc/stat", scratch, sizeof(scratch));
    if (n <= 0)
        return -1;
    char *p = strstr(scratch, "\nbtime ");
    if (p == NULL)
        return -1;
    p++;
    char *nl = strchr(p, '\n');
    if (nl == NULL || (size_t) (nl - p) >= cap)
        return -1;
    memcpy(out, p, (size_t) (nl - p));
    out[nl - p] = '\0';
    return 0;
}

// Reads one named counter out of a single-read /proc/stat. -1 if absent.
static long stat_counter(const char *label) {
    ssize_t n = read_whole_once("/proc/stat", scratch, sizeof(scratch));
    if (n <= 0)
        return -1;
    size_t plen = strlen(label);
    for (char *p = scratch; p != NULL; ) {
        if (strncmp(p, label, plen) == 0 && p[plen] == ' ')
            return strtol(p + plen + 1, NULL, 10);
        p = strchr(p, '\n');
        if (p != NULL)
            p++;
    }
    return -1;
}

static pid_t spawn(void (*body)(void)) {
    pid_t pid = fork();
    if (pid == 0) {
        // Never outlive the parent by much, even if it is killed outright.
        alarm(test_watchdog_secs(240));
        body();
        _exit(0);
    }
    return pid;
}

// Blocks forever. Each of these adds exactly one to procs_blocked, which is
// how the churn is steered onto a decade boundary.
static int sleeper_pipe[2];
static void sleeper_body(void) {
    close(sleeper_pipe[1]);
    char c;
    while (read(sleeper_pipe[0], &c, 1) > 0)
        ;
    _exit(0);
}

// Alternates blocked and running, so procs_blocked (and procs_running with it)
// keeps moving across whatever boundary the sleepers parked it next to.
static void toggler_body(void) {
    for (;;) {
        sleep_us(TOGGLE_US);
        double until = now_seconds() + (double) TOGGLE_US / 1e6;
        volatile unsigned long sink = 0;
        while (now_seconds() < until)
            for (int i = 0; i < 200; i++)
                sink += (unsigned long) i;
    }
}

// Fork churn, which is what moved the counters in the original report.
static void fork_churner_body(void) {
    for (;;) {
        pid_t pid = fork();
        if (pid == 0) {
            sleep_us(2000);
            _exit(0);
        }
        if (pid < 0) {
            sleep_us(20000);
            continue;
        }
        waitpid(pid, NULL, 0);
    }
}

static void stop_children(pid_t *kids, int n) {
    for (int i = 0; i < n; i++)
        if (kids[i] > 0)
            kill(kids[i], SIGKILL);
    for (int i = 0; i < n; i++)
        if (kids[i] > 0)
            waitpid(kids[i], NULL, 0);
}

// Samples procs_blocked and reports whether it was seen on both sides of
// `boundary` -- the churn's own positive control. Without a width change
// there, the byte-at-a-time pass below cannot detect anything, and a pass
// that cannot fail must say so rather than look like a result.
static int straddles(long boundary, long *min_out, long *max_out) {
    long lo = -1, hi = -1;
    for (int i = 0; i < 120; i++) {
        long v = stat_counter("procs_blocked");
        if (v < 0)
            continue;
        if (lo < 0 || v < lo)
            lo = v;
        if (v > hi)
            hi = v;
        sleep_us(500);
    }
    *min_out = lo;
    *max_out = hi;
    return lo >= 0 && lo < boundary && hi >= boundary;
}

static void phase_stat_churn(void) {
    ssize_t ref_len = read_whole_once("/proc/stat", reference, sizeof(reference));
    static struct stat_shape shape;
    if (ref_len <= 0 || stat_shape_learn(&shape, reference, (size_t) ref_len) < 0) {
        printf("proc_read_snapshot: SKIP stat phase (/proc/stat is not the layout this checks)\n");
        return;
    }
    test_logf("stat: %zd bytes, %d cpu lines, %s\n", ref_len, shape.ncpu, shape.btime_line);

    if (pipe(sleeper_pipe) < 0) {
        failf("pipe for sleepers", (uint64_t) errno, 0, 0, 0, 0, 0);
        return;
    }

    // Park procs_blocked just under a power of ten, so the togglers' ±1 keeps
    // crossing it. Nothing else in /proc/stat changes width on demand: every
    // other counter there is monotonic and crosses a decade once per decade,
    // which is exactly why the original corruption was a once-in-a-while
    // event rather than something a loop reproduces.
    long base = stat_counter("procs_blocked");
    if (base < 0)
        base = 0;
    long boundary = 10;
    while (boundary <= base + 1)
        boundary *= 10;
    long want = boundary - 1 - base;
    if (want < 0 || want > MAX_SLEEPERS)
        want = 0;

    static pid_t sleepers[MAX_SLEEPERS];
    int nsleepers = 0;
    for (; nsleepers < want; nsleepers++) {
        sleepers[nsleepers] = spawn(sleeper_body);
        if (sleepers[nsleepers] <= 0)
            break;
    }

    static pid_t togglers[TOGGLERS];
    for (int i = 0; i < TOGGLERS; i++)
        togglers[i] = spawn(toggler_body);
    static pid_t churners[FORK_CHURNERS];
    for (int i = 0; i < FORK_CHURNERS; i++)
        churners[i] = spawn(fork_churner_body);

    // One correction round: the ambient blocked count is not ours to predict,
    // and a sleeper is cheap, so measure what actually happened and adjust.
    long lo = 0, hi = 0;
    int crossing = straddles(boundary, &lo, &hi);
    for (int round = 0; !crossing && round < 8; round++) {
        if (hi < boundary && nsleepers < MAX_SLEEPERS) {
            sleepers[nsleepers] = spawn(sleeper_body);
            if (sleepers[nsleepers] > 0)
                nsleepers++;
        } else if (lo >= boundary && nsleepers > 0) {
            stop_children(&sleepers[nsleepers - 1], 1);
            nsleepers--;
        } else {
            break;
        }
        crossing = straddles(boundary, &lo, &hi);
    }
    test_logf("stat: %d sleepers, procs_blocked seen %ld..%ld, boundary %ld, crossing=%d\n",
              nsleepers, lo, hi, boundary, crossing);
    if (!crossing)
        printf("proc_read_snapshot: WARN /proc/stat's procs_blocked never crossed %ld "
               "(seen %ld..%ld) -- this phase could not change a field's width\n",
               boundary, lo, hi);

    size_t tail_start = (size_t) ref_len > (size_t) STAT_TAIL_BYTES
                      ? (size_t) ref_len - (size_t) STAT_TAIL_BYTES : 0;
    double start = now_seconds();
    unsigned long passes = 0, reads = 0;
    int problems = 0;
    char btime_a[64], btime_b[64];
    while (passes < STAT_PASSES && now_seconds() - start < STAT_SECONDS) {
        if (stat_btime_line(btime_a, sizeof(btime_a)) < 0) {
            failf("read btime from /proc/stat", 0, 0, 0, 1, 0, 0);
            break;
        }
        int fd = open("/proc/stat", O_RDONLY);
        if (fd < 0) {
            failf("open /proc/stat", (uint64_t) errno, 0, 0, 0, 0, 0);
            break;
        }
        size_t len = 0;
        for (;;) {
            char c;
            ssize_t n = read(fd, &c, 1);
            if (n <= 0)
                break;
            assembled[len++] = c;
            reads++;
            if (len >= sizeof(assembled) - 1)
                break;
            // Linger over the tail: procs_blocked is the second-to-last line,
            // so only a reader still inside the last few dozen bytes can see
            // its width change shift the rest out from under it.
            if (len >= tail_start)
                sleep_us(STAT_TAIL_DELAY_US);
        }
        close(fd);
        passes++;
        if (stat_btime_line(btime_b, sizeof(btime_b)) < 0) {
            failf("read btime from /proc/stat", 0, 0, 0, 1, 0, 0);
            break;
        }
        int bad = check_stat_text(assembled, len, "stat byte-at-a-time", &shape,
                                  btime_a, btime_b);
        if (bad != 0 && problems < MAX_REPORTS)
            test_log_if(1, "  (pass %lu, %zu bytes)\n", passes, len);
        problems += bad;
    }
    test_logf("stat: %lu passes, %lu one-byte reads, %.1fs, %d problems\n",
              passes, reads, now_seconds() - start, problems);

    stop_children(churners, FORK_CHURNERS);
    stop_children(togglers, TOGGLERS);
    stop_children(sleepers, nsleepers);
    close(sleeper_pipe[0]);
    close(sleeper_pipe[1]);

    if (problems != 0)
        failf("stat: byte-at-a-time read spliced several renderings", problems, 0, 0, 0, 0, 0);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(180));
    signal(SIGPIPE, SIG_IGN);

    phase_status_splice();
    phase_stat_churn();

    return finish_suite("proc_read_snapshot");
}
