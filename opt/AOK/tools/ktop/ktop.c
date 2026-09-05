/*
 * ktop -- a small, dependency-free htop-style process viewer with one extra
 * column: the CPU architecture (arm64 / x86_64 / x86 / riscv64) of each
 * process's binary, read straight from its ELF header via /proc/<pid>/exe --
 * or, for a natively-dispatched program, the host's architecture, since that
 * is what it was compiled for.
 *
 * Built for iSH-AOK: a single guest can run i386, amd64, arm64 and riscv64
 * binaries side by side (e.g. a chroot into another installed root via
 * /AOK/tools/mount-root.sh), and stock top/htop have no way to show that mix.
 *
 * Interactive mode is htop-flavored: colored per-CPU / memory / swap meter
 * bars, a cursor-selectable process list with scrolling, sort hotkeys, and
 * kill -- all with raw ANSI escapes. Batch mode (-b) keeps the original plain
 * top-style table for scripting.
 *
 * No dependencies beyond a C99 libc and /proc -- no ncurses, no procps.
 *
 * Usage: ktop [-b] [-n iterations] [-d seconds]
 *   -b            batch mode: no screen clearing/terminal control, just
 *                 print each snapshot and exit after -n iterations
 *                 (default 1 in batch mode).
 *   -n iterations number of snapshots before exiting (default: run until
 *                 'q' is pressed or the process is killed, in interactive
 *                 mode).
 *   -d seconds    delay between snapshots (default: 3).
 *
 * Interactive keys:
 *   Up/Down/PgUp/PgDn/Home/End  move the cursor / scroll
 *   P / M / T / N               sort by %CPU / %MEM / TIME+ / PID
 *   c                           toggle full command line vs comm
 *   k                           send a signal to the selected process
 *   q                           quit
 */
// Guarded: AOK's own build defines this on the command line when it compiles
// this file as a native program (meson.build), and an unguarded repeat of a
// macro whose spelling differs is a warning.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX_PROCS 4096
#define MAX_CPUS 16
#define CMDLINE_MAX 160

struct proc_sample {
    pid_t pid;
    uid_t uid;
    char comm[64];
    char cmdline[CMDLINE_MAX]; // full command line ('\0'-separators joined
                               // with spaces); empty for kernel threads or
                               // unreadable /proc/<pid>/cmdline
    char state;
    long priority, nice;
    unsigned long vsize;       // bytes
    long rss_pages;
    unsigned long long utime, stime; // jiffies, cumulative
    const char *arch;          // interned string, never freed
    unsigned long long cpu_delta; // jiffies since the previous sample;
                                   // transient, filled in by fill_cpu_deltas
                                   // just before sorting/printing (0 for a
                                   // process not present in the previous
                                   // sample, including the very first one)
};

// ---- colors ----------------------------------------------------------------

// Empty strings in batch mode / non-tty stdout so every printf below can use
// them unconditionally.
static const char *C_RESET = "", *C_BAR_GREEN = "", *C_BAR_RED = "",
                  *C_BAR_BLUE = "", *C_BAR_YELLOW = "", *C_LABEL = "",
                  *C_HDR = "", *C_HDR_SORT = "", *C_SEL = "", *C_DIM = "",
                  *C_KEY = "", *C_KEYBAR = "";

// The colourless defaults above are the state a run that does NOT want colour
// expects to find. enable_colors is one-way, so restoring them is reset_state's
// job rather than something any drawing path does.
static void disable_colors(void) {
    C_RESET = C_BAR_GREEN = C_BAR_RED = C_BAR_BLUE = C_BAR_YELLOW = "";
    C_LABEL = C_HDR = C_HDR_SORT = C_SEL = C_DIM = C_KEY = C_KEYBAR = "";
}

static void enable_colors(void) {
    C_RESET = "\033[0m";
    C_BAR_GREEN = "\033[32m";
    C_BAR_RED = "\033[31m";
    C_BAR_BLUE = "\033[34m";
    C_BAR_YELLOW = "\033[33m";
    C_LABEL = "\033[1;36m";     // meter labels, bright cyan
    C_HDR = "\033[30;42m";      // column header: black on green (htop)
    C_HDR_SORT = "\033[30;46m"; // active sort column: black on cyan
    C_SEL = "\033[30;46m";      // selected row: black on cyan
    C_DIM = "\033[2m";
    C_KEY = "\033[0;37m";       // key bar: "k" in plain
    C_KEYBAR = "\033[30;46m";   // key bar: label on cyan
}

// ---- per-process architecture detection -----------------------------------

static const char *arch_intern(const char *s) {
    // All callers pass one of these literals or "?"; returning the literal
    // itself keeps proc_sample.arch a non-owning pointer with no
    // allocation/free bookkeeping.
    return s;
}

// The vocabulary the ARCH column uses, from a machine name as uname(2) or
// /proc/cpuinfo spells it. Shared by the guest and host lookups below: they
// draw on different sources but have to land in the same alphabet.
//
// "arm64" is a prefix test rather than an equality one because
// NXGetLocalArchInfo -- what the app build reports the host as -- says
// "arm64e" on Apple Silicon, and the pointer-authentication ABI is not a
// different CPU architecture as far as this column is concerned. It has to be
// tried before the bare "arm" prefix below, which would otherwise swallow it.
static const char *arch_from_machine(const char *machine) {
    if (strcmp(machine, "aarch64") == 0 || strncmp(machine, "arm64", 5) == 0)
        return arch_intern("arm64");
    if (strcmp(machine, "x86_64") == 0 || strcmp(machine, "amd64") == 0)
        return arch_intern("x86_64");
    if (strcmp(machine, "riscv64") == 0)
        return arch_intern("riscv64");
    if (machine[0] == 'i' && strstr(machine, "86") != NULL)
        return arch_intern("x86");
    if (strncmp(machine, "arm", 3) == 0)
        return arch_intern("arm");
    return arch_intern("?");
}

// A kernel thread has no /proc/<pid>/exe -- on real Linux either -- so there is
// no ELF header to read an architecture out of. Reporting "?" for AOK's
// kthreadd is accurate but useless; a kernel thread belongs to the running
// kernel, so the honest answer is the guest's own architecture. uname gives
// that, and it is whatever the booted rootfs is.
//
// Cached: uname does not change under us.
// File scope rather than function-local statics purely so reset_state() can
// clear them -- see the note there. Neither value changes under a running
// guest, but the process can outlive the guest: switching roots restarts the
// guest inside the same app, and the answer to both questions can differ
// across that.
static const char *guest_arch_cached = NULL;
static const char *host_arch_cached = NULL;

static const char *guest_arch(void) {
    if (guest_arch_cached != NULL)
        return guest_arch_cached;
    struct utsname u;
    guest_arch_cached = uname(&u) == 0
        ? arch_from_machine(u.machine) : arch_intern("?");
    return guest_arch_cached;
}

// The architecture of the HOST iSH-AOK is running on, which is the one native
// programs (below) were compiled for. iSH-AOK publishes it as a "host arch"
// line in /proc/cpuinfo, printed for every guest ABI, so it does not depend on
// which root is booted -- native bash in an i386 root is still arm64 code.
// Nothing outside iSH-AOK publishes that line, and nothing outside iSH-AOK has
// native programs to ask about, so "?" there is both honest and unreachable.
//
// The app build appends the chip in parentheses ("arm64(Apple M4)"), so the
// value ends at the first '(' as well as at whitespace.
static const char *host_arch(void) {
    if (host_arch_cached != NULL)
        return host_arch_cached;
    host_arch_cached = arch_intern("?");
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f == NULL)
        return host_arch_cached;
    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "host arch", 9) != 0)
            continue;
        char *value = strchr(line, ':');
        if (value == NULL)
            continue;
        for (value++; *value == ' ' || *value == '\t'; value++)
            ;
        char *end = value;
        while (*end != '\0' && *end != '(' && !isspace((unsigned char) *end))
            end++;
        *end = '\0';
        if (*value != '\0')
            host_arch_cached = arch_from_machine(value);
        break;
    }
    fclose(f);
    return host_arch_cached;
}

// Natively-dispatched programs -- bash, zsh, and SmallCLUE's applets, all
// reached through /AOK/native -- are host code compiled into iSH-AOK rather
// than guest binaries. Exec of one points /proc/<pid>/exe at the /AOK/native
// entry, and what lives behind that path is a placeholder shell script, not an
// ELF image (fs/aok.c), so the header read below finds nothing: every native
// shell on the system showed "?" in the ARCH column. Recognise the path
// instead and report the host's architecture, which is what that code actually
// is.
//
// /AOK is where aokfs is mounted -- fixed by both entry points (main.c and
// app/AppDelegate.m), regardless of the installed root -- so the prefix is a
// reliable test. readlink rather than open: the link target is still readable
// from inside a chroot that cannot open it (see ktop.md).
static bool exe_is_native(pid_t pid) {
    static const char prefix[] = "/AOK/native/";
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/exe", (int) pid);
    char target[PATH_MAX];
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n < 0)
        return false;
    target[n] = '\0';
    return strncmp(target, prefix, sizeof(prefix) - 1) == 0;
}

// `kernel_thread` comes from the caller, which has already read the state and
// the command line: an empty /proc/<pid>/cmdline is how ps decides to bracket a
// name, and excluding zombies keeps a reaped process from borrowing the label.
static const char *detect_arch(pid_t pid, bool kernel_thread) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/exe", (int) pid);
    unsigned char hdr[20];
    ssize_t n = -1;
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        n = read(fd, hdr, sizeof(hdr));
        close(fd);
    }

    if (n >= 20 && memcmp(hdr, "\x7f""ELF", 4) == 0) {
        bool little_endian = hdr[5] != 2; // EI_DATA: 1 = LSB, 2 = MSB
        unsigned e_machine = little_endian
            ? (unsigned) hdr[18] | ((unsigned) hdr[19] << 8)
            : (unsigned) hdr[19] | ((unsigned) hdr[18] << 8);

        switch (e_machine) {
            case 3:   return arch_intern("x86");    // EM_386
            case 62:  return arch_intern("x86_64"); // EM_X86_64
            case 183: return arch_intern("arm64");  // EM_AARCH64
            case 40:  return arch_intern("arm");    // EM_ARM (32-bit, just in case)
            case 243: return arch_intern("riscv64"); // EM_RISCV
            default:  return arch_intern("?");
        }
    }

    // Nothing ELF-shaped behind the exe link. A native program is the one case
    // with a real answer; a kernel thread has no exe at all and borrows the
    // kernel's; anything else -- an exe outside this chroot, a process that
    // exited mid-scan -- is genuinely unknown.
    if (exe_is_native(pid))
        return host_arch();
    if (fd < 0 && kernel_thread)
        return guest_arch();
    return arch_intern("?");
}

// ---- /proc/<pid>/stat parsing ---------------------------------------------

// /proc/<pid>/cmdline and the comm field carry whatever bytes the process chose
// to put there. A raw newline ends the row in the middle of the table and a
// carriage return rewrites it from column 0, so one process with either in its
// argv corrupts the whole display until the next full redraw. Map every control
// byte to a space, which is what top does.
static void sanitize_display(char *s) {
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char) *s;
        if (c < 0x20 || c == 0x7f)
            *s = ' ';
    }
}


// Truncate for display: at most `width` COLUMNS, never splitting a UTF-8
// sequence, with top's '+' marker when something was cut.
//
// Two reasons this exists rather than a bare "%.*s". First, precision counts
// BYTES, so a multi-byte character straddling the limit is cut in half and the
// terminal renders the tail as garbage. Second, the batch path had no limit at
// all: it printed comm with a plain %s, so a long name ran past the right edge
// and wrapped, and a wrapped line makes every row after it unreadable.
static void truncate_display(char *dst, size_t dstsize, const char *src, int width) {
    if (dstsize == 0)
        return;
    if (width < 1)
        width = 1;
    size_t out = 0;
    int cols = 0;
    const unsigned char *p = (const unsigned char *) src;
    while (*p != '\0' && cols < width) {
        // Length of this UTF-8 sequence; a stray continuation or invalid lead
        // byte is treated as one byte so we always make progress.
        size_t seq = 1;
        if ((*p & 0xE0) == 0xC0) seq = 2;
        else if ((*p & 0xF0) == 0xE0) seq = 3;
        else if ((*p & 0xF8) == 0xF0) seq = 4;
        for (size_t k = 1; k < seq; k++)
            if ((p[k] & 0xC0) != 0x80) { seq = 1; break; }
        if (out + seq >= dstsize)
            break;
        memcpy(dst + out, p, seq);
        out += seq;
        p += seq;
        cols++;   // counts characters, not wcwidth -- see the note below
    }
    // A truncation marker, as top uses, so a cut name is not mistaken for the
    // whole one. Only when there is more to show AND room to say so.
    if (*p != '\0' && out > 0 && out + 1 < dstsize)
        dst[out - 1] = '+';
    dst[out] = '\0';
}

// Reads one process's fields we need. /proc/<pid>/stat's 2nd field (comm) is
// the only one that can contain spaces or parens, so we locate it by the
// *last* ')' on the line rather than naive whitespace splitting.
static bool read_proc_stat(pid_t pid, struct proc_sample *out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int) pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return false;
    char line[1024];
    bool ok = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    if (!ok)
        return false;

    char *lparen = strchr(line, '(');
    char *rparen = strrchr(line, ')');
    if (lparen == NULL || rparen == NULL || rparen < lparen)
        return false;

    size_t comm_len = (size_t) (rparen - lparen - 1);
    if (comm_len >= sizeof(out->comm))
        comm_len = sizeof(out->comm) - 1;
    memcpy(out->comm, lparen + 1, comm_len);
    out->comm[comm_len] = '\0';
    sanitize_display(out->comm);

    out->pid = pid;

    // Fields after ") " are: state pid.ppid... -- field 3 (state) is the
    // first token past the comm; utime/stime are fields 14/15; priority/nice
    // are 18/19; vsize/rss are 23/24 (1-indexed, full /proc/pid/stat layout).
    char *p = rparen + 2;
    int field = 3;
    char state = '?';
    unsigned long long utime = 0, stime = 0;
    long priority = 0, nice_val = 0;
    unsigned long vsize = 0;
    long rss = 0;
    while (*p != '\0' && field <= 24) {
        char *end;
        switch (field) {
            case 3:
                state = *p;
                while (*p != '\0' && !isspace((unsigned char) *p))
                    p++;
                break;
            case 14:
                utime = strtoull(p, &end, 10);
                p = end;
                break;
            case 15:
                stime = strtoull(p, &end, 10);
                p = end;
                break;
            case 18:
                priority = strtol(p, &end, 10);
                p = end;
                break;
            case 19:
                nice_val = strtol(p, &end, 10);
                p = end;
                break;
            case 23:
                vsize = strtoul(p, &end, 10);
                p = end;
                break;
            case 24:
                rss = strtol(p, &end, 10);
                p = end;
                break;
            default:
                while (*p != '\0' && !isspace((unsigned char) *p))
                    p++;
                break;
        }
        while (*p == ' ')
            p++;
        field++;
    }

    out->state = state;
    out->utime = utime;
    out->stime = stime;
    out->priority = priority;
    out->nice = nice_val;
    out->vsize = vsize;
    out->rss_pages = rss;
    return true;
}

static uid_t read_proc_uid(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int) pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return (uid_t) -1;
    uid_t uid = (uid_t) -1;
    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long real;
        if (sscanf(line, "Uid: %lu", &real) == 1) {
            uid = (uid_t) real;
            break;
        }
    }
    fclose(f);
    return uid;
}

// Width of everything to the left of the Command column. Kept in one place so
// the header and the rows agree on where Command starts and how wide it is.
static int command_column_start(void) {
    return 6 + 1 + 8 + 1 + 3 + 1 + 3 + 1 + 7 + 1 + 7 + 1 + 1 + 1
         + 7 + 1 + 5 + 1 + 5 + 1 + 8 + 1;
}

static void read_proc_cmdline(pid_t pid, char *buf, size_t bufsize) {
    buf[0] = '\0';
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int) pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    ssize_t n = read(fd, buf, bufsize - 1);
    close(fd);
    if (n <= 0) {
        buf[0] = '\0';
        return;
    }
    // argv strings are '\0'-separated (with a trailing '\0'); join with
    // spaces for display.
    for (ssize_t i = 0; i < n - 1; i++) {
        if (buf[i] == '\0')
            buf[i] = ' ';
    }
    buf[n] = '\0';
    sanitize_display(buf);
    // Trim trailing separator-turned-space noise.
    while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\0'))
        buf[--n] = '\0';
}

// ---- system-wide totals ----------------------------------------------------

struct meminfo {
    unsigned long total_kb, free_kb, avail_kb;
    unsigned long buffers_kb, cached_kb;
    unsigned long swap_total_kb, swap_free_kb;
};

static void read_meminfo(struct meminfo *mi) {
    memset(mi, 0, sizeof(*mi));
    FILE *f = fopen("/proc/meminfo", "r");
    if (f == NULL)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        sscanf(line, "MemTotal: %lu kB", &mi->total_kb);
        sscanf(line, "MemFree: %lu kB", &mi->free_kb);
        sscanf(line, "MemAvailable: %lu kB", &mi->avail_kb);
        sscanf(line, "Buffers: %lu kB", &mi->buffers_kb);
        sscanf(line, "Cached: %lu kB", &mi->cached_kb);
        sscanf(line, "SwapTotal: %lu kB", &mi->swap_total_kb);
        sscanf(line, "SwapFree: %lu kB", &mi->swap_free_kb);
    }
    fclose(f);
}

static void read_loadavg(double *l1, double *l5, double *l15) {
    *l1 = *l5 = *l15 = 0;
    FILE *f = fopen("/proc/loadavg", "r");
    if (f == NULL)
        return;
    if (fscanf(f, "%lf %lf %lf", l1, l5, l15) != 3) {
        *l1 = *l5 = *l15 = 0;
    }
    fclose(f);
}

// Per-CPU jiffy counters from /proc/stat's cpuN lines. Slot [i] is cpuN=i;
// returns the number of per-CPU lines found (0 if only the aggregate "cpu "
// line exists).
struct cpu_ticks {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
};

static int read_cpu_ticks(struct cpu_ticks *cpus, int max) {
    FILE *f = fopen("/proc/stat", "r");
    if (f == NULL)
        return 0;
    int count = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL && count < max) {
        int idx;
        struct cpu_ticks t = {0};
        // Only cpuN lines: sscanf's %d would happily skip the whitespace in
        // the aggregate "cpu  ..." line and misread its first counter as N.
        if (strncmp(line, "cpu", 3) != 0 || !isdigit((unsigned char) line[3]))
            continue;
        if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
                   &idx, &t.user, &t.nice, &t.system, &t.idle,
                   &t.iowait, &t.irq, &t.softirq, &t.steal) >= 5) {
            if (idx >= 0 && idx < max) {
                cpus[idx] = t;
                if (idx + 1 > count)
                    count = idx + 1;
            }
        }
    }
    fclose(f);
    return count;
}

// ---- process table snapshot ------------------------------------------------

static int collect(struct proc_sample *procs, int max) {
    DIR *d = opendir("/proc");
    if (d == NULL) {
        perror("ktop: opendir /proc");
        return 0;
    }
    int count = 0;
    struct dirent *de;
    while (count < max && (de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char) de->d_name[0]))
            continue;
        pid_t pid = (pid_t) atol(de->d_name);
        struct proc_sample p = {0};
        if (!read_proc_stat(pid, &p))
            continue;
        p.uid = read_proc_uid(pid);
        // cmdline first: detect_arch needs it to tell a kernel thread (no exe,
        // empty cmdline) from a process whose exe merely could not be opened.
        read_proc_cmdline(pid, p.cmdline, sizeof(p.cmdline));
        p.arch = detect_arch(pid, p.cmdline[0] == '\0' && p.state != 'Z');
        procs[count++] = p;
    }
    closedir(d);
    return count;
}

// Fill in cpu_delta (jiffies used since the previous sample) BEFORE sorting:
// a process absent from the previous sample -- including every process on the
// very first snapshot, where prev_n is 0 -- gets 0, not its full lifetime
// utime+stime. Without this, long-lived processes would show wildly inflated
// %CPU on first display and permanently outrank recently-busy ones.
static void fill_cpu_deltas(struct proc_sample *cur, int cur_n,
                            struct proc_sample *prev, int prev_n) {
    for (int i = 0; i < cur_n; i++) {
        cur[i].cpu_delta = 0;
        unsigned long long cur_total = cur[i].utime + cur[i].stime;
        for (int j = 0; j < prev_n; j++) {
            if (prev[j].pid == cur[i].pid) {
                unsigned long long prev_total = prev[j].utime + prev[j].stime;
                cur[i].cpu_delta = cur_total >= prev_total ? cur_total - prev_total : 0;
                break;
            }
        }
    }
}

// %CPU is jiffies-used divided by jiffies-ELAPSED, and the elapsed part is the
// wall time between the two samples -- NOT one second.
//
// This used to divide by clk_tck alone, i.e. it assumed the sample interval was
// exactly 1s while the default delay is 3s. Everything on screen was therefore
// multiplied by the delay: a process genuinely using one whole CPU was reported
// at 301%, which is what it looked like on an 8-core iPad -- two processes each
// pinning a core, each labelled 301%. The CPU burn was real; the number was
// three times too big, and changing -d changed every reading.
static double cpu_percent(unsigned long long cpu_delta, long clk_tck,
                          double elapsed_sec) {
    if (clk_tck <= 0 || elapsed_sec <= 0)
        return 0;
    return (double) cpu_delta * 100.0 / ((double) clk_tck * elapsed_sec);
}

// Seconds between two CLOCK_MONOTONIC readings.
static double elapsed_since(const struct timespec *then) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double d = (double) (now.tv_sec - then->tv_sec)
             + (double) (now.tv_nsec - then->tv_nsec) / 1e9;
    return d > 0 ? d : 0;
}

// ---- sorting ----------------------------------------------------------------

enum sort_mode { SORT_CPU, SORT_MEM, SORT_TIME, SORT_PID };
static enum sort_mode sort_mode = SORT_CPU;

static int cmp_procs(const void *pa, const void *pb) {
    const struct proc_sample *a = pa, *b = pb;
    switch (sort_mode) {
        case SORT_MEM:
            if (a->rss_pages != b->rss_pages)
                return a->rss_pages > b->rss_pages ? -1 : 1;
            break;
        case SORT_TIME: {
            unsigned long long ta = a->utime + a->stime, tb = b->utime + b->stime;
            if (ta != tb)
                return ta > tb ? -1 : 1;
            break;
        }
        case SORT_PID:
            break; // pid tiebreak below is the whole sort
        case SORT_CPU:
        default:
            if (a->cpu_delta != b->cpu_delta)
                return a->cpu_delta > b->cpu_delta ? -1 : 1;
            break;
    }
    return a->pid < b->pid ? -1 : (a->pid > b->pid ? 1 : 0);
}

// ---- formatting helpers ------------------------------------------------------

static const char *username_for(uid_t uid, char *buf, size_t bufsize) {
    if (uid == (uid_t) -1) {
        snprintf(buf, bufsize, "?");
        return buf;
    }
    struct passwd *pw = getpwuid(uid);
    if (pw != NULL) {
        snprintf(buf, bufsize, "%s", pw->pw_name);
        return buf;
    }
    snprintf(buf, bufsize, "%lu", (unsigned long) uid);
    return buf;
}

static void format_uptime(char *buf, size_t bufsize) {
    FILE *f = fopen("/proc/uptime", "r");
    double seconds = 0;
    if (f != NULL) {
        if (fscanf(f, "%lf", &seconds) != 1)
            seconds = 0;
        fclose(f);
    }
    long total = (long) seconds;
    long days = total / 86400;
    long hours = (total % 86400) / 3600;
    long mins = (total % 3600) / 60;
    if (days > 0)
        snprintf(buf, bufsize, "%ld days, %02ld:%02ld", days, hours, mins);
    else
        snprintf(buf, bufsize, "%02ld:%02ld", hours, mins);
}

// Human-ish size like htop's: plain KiB up to 6 digits, then M / G with one
// decimal. Always fits in 7 columns.
static void format_kb(unsigned long kb, char *buf, size_t bufsize) {
    if (kb < 1000000UL)
        snprintf(buf, bufsize, "%lu", kb);
    else if (kb < 10UL * 1024 * 1024)
        snprintf(buf, bufsize, "%.1fM", (double) kb / 1024.0);
    else
        snprintf(buf, bufsize, "%.1fG", (double) kb / (1024.0 * 1024.0));
}

// Same, but always with an explicit K/M/G unit -- for meter bar text, where
// a bare number would be ambiguous.
//
// Scales at 1 MiB, NOT at format_kb's six digits. That threshold exists so the
// process table's VIRT/RES columns stay 7 wide, which meter text does not care
// about, and inheriting it here meant anything under ~977 MB printed as raw
// kilobytes: a 256 MB swap area read "262144K", and the Mem meter managed
// "479629K/2098.0M" -- the same function scaling one number and not the other,
// in one line. Reported from a device as the swap figure being wrong; the value
// was right all along and only the unit was unreadable.
static void format_kb_unit(unsigned long kb, char *buf, size_t bufsize) {
    if (kb < 1024UL)
        snprintf(buf, bufsize, "%luK", kb);
    else if (kb < 1024UL * 1024)
        snprintf(buf, bufsize, "%.1fM", (double) kb / 1024.0);
    else
        // Two decimals at gigabyte scale: one turns a 2098 MB ceiling into a
        // flat "2.0G" and hides the difference between devices.
        snprintf(buf, bufsize, "%.2fG", (double) kb / (1024.0 * 1024.0));
}

// htop TIME+ format: mm:ss.cc, rolling to h:mm:ss past an hour.
static void format_time_plus(unsigned long long jiffies, long clk_tck,
                             char *buf, size_t bufsize) {
    double secs = clk_tck > 0 ? (double) jiffies / (double) clk_tck : 0;
    unsigned long long total = (unsigned long long) secs;
    if (total >= 3600) {
        snprintf(buf, bufsize, "%lluh%02llu:%02llu",
                 total / 3600, (total % 3600) / 60, total % 60);
    } else {
        unsigned cents = (unsigned) ((secs - (double) total) * 100);
        snprintf(buf, bufsize, "%llu:%02llu.%02u", total / 60, total % 60, cents);
    }
}

// ---- terminal ---------------------------------------------------------------

static void term_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 24;
        *cols = 80;
    }
}

static struct termios orig_termios;
static bool termios_saved = false;

// The dispositions enable_raw_stdin displaced, so teardown_terminal can put
// them back. ktop used to leave its own handlers installed and let process
// exit clean them up; that is true of a program that owns its process and
// false of one running as a native program (kernel/native.h), where the shell
// this returns to would inherit them.
static void (*prev_sigint)(int) = SIG_DFL;
static void (*prev_sigterm)(int) = SIG_DFL;
static void (*prev_sighup)(int) = SIG_DFL;
static bool handlers_installed = false;

// Undoes everything interactive mode did to the terminal: leave the
// alternate screen (restoring whatever was on screen before ktop ran, like
// htop), reset attributes, and restore canonical/echo mode. Reached from
// teardown_terminal on normal quit and from the signal handler on
// SIGINT/SIGTERM/SIGHUP -- without the latter, a Ctrl-C would leave the tty
// raw with echo off.
static void restore_terminal(void) {
    // write(2), not stdio: also called from a signal handler.
    static const char leave[] = "\033[0m\033[?1049l";
    ssize_t ignored = write(STDOUT_FILENO, leave, sizeof(leave) - 1);
    (void) ignored;
    if (termios_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

static void exit_signal_handler(int sig) {
    restore_terminal();
    // Re-raise with default disposition so the exit status is correct.
    signal(sig, SIG_DFL);
    raise(sig);
}

// Everything enable_raw_stdin changed, put back: the screen and tty by
// restore_terminal, and the three signal dispositions it displaced. Called on
// the way out of interactive mode. Idempotent, so an exit path that is not
// sure whether raw mode was ever entered can just call it.
//
// Deliberately not atexit(): a native program returns to a process that keeps
// running, so an atexit handler would fire at the app's exit rather than this
// program's -- writing escape bytes at a descriptor that has since become
// something else -- and would stack up one more registration per run.
static void teardown_terminal(void) {
    if (termios_saved)
        restore_terminal();
    termios_saved = false;
    if (handlers_installed) {
        signal(SIGINT, prev_sigint);
        signal(SIGTERM, prev_sigterm);
        signal(SIGHUP, prev_sighup);
        handlers_installed = false;
    }
}

static void enable_raw_stdin(void) {
    if (!isatty(STDIN_FILENO))
        return;
    if (tcgetattr(STDIN_FILENO, &orig_termios) != 0)
        return;
    termios_saved = true;
    prev_sigint = signal(SIGINT, exit_signal_handler);
    prev_sigterm = signal(SIGTERM, exit_signal_handler);
    prev_sighup = signal(SIGHUP, exit_signal_handler);
    handlers_installed = true;
    struct termios raw = orig_termios;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

// Input events, decoded from raw bytes (arrow keys arrive as ESC [ X).
enum key {
    KEY_NONE = 0, KEY_QUIT, KEY_UP, KEY_DOWN, KEY_PGUP, KEY_PGDN,
    KEY_HOME, KEY_END, KEY_SORT_CPU, KEY_SORT_MEM, KEY_SORT_TIME,
    KEY_SORT_PID, KEY_KILL, KEY_CMDLINE, KEY_TOGGLE_CPU, KEY_OTHER
};

// Waits up to `seconds` for one key. Returns KEY_NONE on timeout.
static enum key read_key(double seconds) {
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    tv.tv_sec = (time_t) seconds;
    tv.tv_usec = (suseconds_t) ((seconds - (double) tv.tv_sec) * 1e6);
    int r = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    if (r <= 0)
        return KEY_NONE;
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1)
        return KEY_NONE;
    switch (c) {
        case 'q': case 'Q': return KEY_QUIT;
        case 'P': case 'p': return KEY_SORT_CPU;
        case 'M': case 'm': return KEY_SORT_MEM;
        case 'T': case 't': return KEY_SORT_TIME;
        case 'N': case 'n': return KEY_SORT_PID;
        case 'k': case 'K': return KEY_KILL;
        case 'c': case 'C': return KEY_CMDLINE;
        case '1': return KEY_TOGGLE_CPU;
        case '\033': break; // fall through to escape-sequence decode
        default: return KEY_OTHER;
    }
    // Decode CSI sequences: ESC [ A/B (up/down), 5~/6~ (pgup/pgdn),
    // H/F or 1~/4~ (home/end). Non-blocking reads: a bare ESC decodes as
    // KEY_OTHER.
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1 || seq[0] != '[')
        return KEY_OTHER;
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
        return KEY_OTHER;
    switch (seq[1]) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        case '1': case '4': case '5': case '6':
            if (read(STDIN_FILENO, &seq[2], 1) == 1 && seq[2] == '~') {
                switch (seq[1]) {
                    case '1': return KEY_HOME;
                    case '4': return KEY_END;
                    case '5': return KEY_PGUP;
                    case '6': return KEY_PGDN;
                }
            }
            return KEY_OTHER;
        default: return KEY_OTHER;
    }
}

// ---- htop-style meters -------------------------------------------------------

// Draws one bracketed meter: label, colored '|' fill segments, right-aligned
// text inside the bar. fracs/colors describe up to 3 stacked fill fractions
// (of the total width) drawn left to right.
static void draw_meter(const char *label, int inner_width,
                       const double *fracs, const char **colors, int nfrac,
                       const char *text) {
    printf("%s%3s%s[", C_LABEL, label, C_RESET);
    int cells[3] = {0};
    int used = 0;
    for (int i = 0; i < nfrac && i < 3; i++) {
        double f = fracs[i];
        // Clamp to [0,1] BEFORE the int conversion: a bogus fraction (e.g.
        // from a kernel counter glitch) otherwise converts to INT_MAX/UB,
        // and `used + cells[i]` below signed-overflows past the clamp --
        // which once sent the fill loop off the top of the stack. The !(f>0)
        // form also catches NaN.
        if (!(f > 0))
            f = 0;
        if (f > 1)
            f = 1;
        cells[i] = (int) (f * inner_width + 0.5);
        if (cells[i] > inner_width - used)
            cells[i] = inner_width - used;
        used += cells[i];
    }
    int text_len = (int) strlen(text);
    if (text_len > inner_width)
        text_len = inner_width;
    int text_start = inner_width - text_len;

    int pos = 0;
    for (int i = 0; i < nfrac && i < 3; i++) {
        if (cells[i] <= 0)
            continue;
        fputs(colors[i], stdout);
        for (int j = 0; j < cells[i]; j++, pos++) {
            if (pos >= text_start)
                putchar(text[pos - text_start]);
            else
                putchar('|');
        }
        fputs(C_RESET, stdout);
    }
    fputs(C_DIM, stdout);
    for (; pos < inner_width; pos++) {
        if (pos >= text_start)
            putchar(text[pos - text_start]);
        else
            putchar(' ');
    }
    fputs(C_RESET, stdout);
    putchar(']');
}

// ---- interactive (htop-style) drawing ----------------------------------------

static bool show_cmdline = true;
// Real top's '1' expands one combined meter into per-cpu bars; ktop's default
// is already per-cpu, so '1' here does the reverse and collapses them into
// one overall-load summary meter.
static bool show_cpu_summary = false;

// One transient status message shown in the key bar until the next keypress.
static char status_msg[128];

static int header_rows_drawn; // set by draw_header, read by the scroller

static void draw_header(int cols, int ncpu,
                        const struct cpu_ticks *cur_cpu,
                        const struct cpu_ticks *prev_cpu,
                        int task_total, int task_running,
                        const struct meminfo *mi) {
    char timebuf[16], uptime[32];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm_now);
    format_uptime(uptime, sizeof(uptime));
    double l1, l5, l15;
    read_loadavg(&l1, &l5, &l15);

    int rows = 0;
    printf("\033[K%sktop%s - %s up %s   Tasks: %s%d%s, %s%d%s running   Load: %.2f %.2f %.2f\n",
           C_LABEL, C_RESET, timebuf, uptime,
           C_LABEL, task_total, C_RESET, C_LABEL, task_running, C_RESET,
           l1, l5, l15);
    rows++;

    // CPU meters, two per row. Meter inner width sized so two meters + labels
    // + brackets fit the terminal.
    int per_row = cols >= 60 ? 2 : 1;
    int inner = cols / per_row - 7; // 3 label + '[' + ']' + 2 gutter
    if (inner < 10)
        inner = 10;
    if (show_cpu_summary) {
        // Collapsed view: sum ticks across all CPUs into one overall-load
        // meter instead of looping per-cpu below.
        unsigned long long du = 0, ds = 0, didle = 0;
        for (int i = 0; i < ncpu; i++) {
            unsigned long long cu = cur_cpu[i].user + cur_cpu[i].nice;
            unsigned long long pu = prev_cpu[i].user + prev_cpu[i].nice;
            unsigned long long cs = cur_cpu[i].system + cur_cpu[i].irq + cur_cpu[i].softirq;
            unsigned long long ps = prev_cpu[i].system + prev_cpu[i].irq + prev_cpu[i].softirq;
            unsigned long long ci = cur_cpu[i].idle + cur_cpu[i].iowait;
            unsigned long long pi = prev_cpu[i].idle + prev_cpu[i].iowait;
            du += cu >= pu ? cu - pu : 0;
            ds += cs >= ps ? cs - ps : 0;
            didle += ci >= pi ? ci - pi : 0;
        }
        unsigned long long dtotal = du + ds + didle;
        double fu = dtotal > 0 ? (double) du / (double) dtotal : 0;
        double fs = dtotal > 0 ? (double) ds / (double) dtotal : 0;

        char text[16];
        snprintf(text, sizeof(text), "%.1f%%", (fu + fs) * 100.0);
        const double fracs[2] = {fu, fs};
        const char *colors[2] = {C_BAR_GREEN, C_BAR_RED};
        fputs("\033[K", stdout);
        draw_meter("Avg", inner, fracs, colors, 2, text);
        putchar('\n');
        rows++;
    } else {
        for (int i = 0; i < ncpu; i++) {
            // Saturating deltas: a counter that steps backward (kernel glitch,
            // counter reset) must read as 0 ticks, not wrap to ~2^64 and blow up
            // the fractions below.
            unsigned long long cu = cur_cpu[i].user + cur_cpu[i].nice;
            unsigned long long pu = prev_cpu[i].user + prev_cpu[i].nice;
            unsigned long long cs = cur_cpu[i].system + cur_cpu[i].irq + cur_cpu[i].softirq;
            unsigned long long ps = prev_cpu[i].system + prev_cpu[i].irq + prev_cpu[i].softirq;
            unsigned long long ci = cur_cpu[i].idle + cur_cpu[i].iowait;
            unsigned long long pi = prev_cpu[i].idle + prev_cpu[i].iowait;
            unsigned long long du = cu >= pu ? cu - pu : 0;
            unsigned long long ds = cs >= ps ? cs - ps : 0;
            unsigned long long didle = ci >= pi ? ci - pi : 0;
            unsigned long long dtotal = du + ds + didle;
            double fu = dtotal > 0 ? (double) du / (double) dtotal : 0;
            double fs = dtotal > 0 ? (double) ds / (double) dtotal : 0;

            char label[16], text[16];
            snprintf(label, sizeof(label), "%d", i);
            snprintf(text, sizeof(text), "%.1f%%", (fu + fs) * 100.0);
            const double fracs[2] = {fu, fs};
            const char *colors[2] = {C_BAR_GREEN, C_BAR_RED};
            if (i % per_row == 0)
                fputs("\033[K", stdout);
            draw_meter(label, inner, fracs, colors, 2, text);
            if (i % per_row == per_row - 1 || i == ncpu - 1) {
                putchar('\n');
                rows++;
            } else {
                fputs("  ", stdout);
            }
        }
    }

    // Memory: green = apps (used minus buffers/cache), blue = buffers,
    // yellow = cache -- htop's scheme.
    int mem_inner = cols - 7;
    if (mem_inner < 10)
        mem_inner = 10;
    unsigned long used_kb = mi->total_kb > mi->free_kb ? mi->total_kb - mi->free_kb : 0;
    unsigned long cachebuf = mi->buffers_kb + mi->cached_kb;
    unsigned long apps_kb = used_kb > cachebuf ? used_kb - cachebuf : 0;
    double ft = mi->total_kb > 0 ? (double) mi->total_kb : 1;
    char mem_used[16], mem_total[16], text[40];
    format_kb_unit(apps_kb, mem_used, sizeof(mem_used));
    format_kb_unit(mi->total_kb, mem_total, sizeof(mem_total));
    snprintf(text, sizeof(text), "%s/%s", mem_used, mem_total);
    {
        const double fracs[3] = {apps_kb / ft, mi->buffers_kb / ft, mi->cached_kb / ft};
        const char *colors[3] = {C_BAR_GREEN, C_BAR_BLUE, C_BAR_YELLOW};
        fputs("\033[K", stdout);
        draw_meter("Mem", mem_inner, fracs, colors, 3, text);
        putchar('\n');
        rows++;
    }
    if (mi->swap_total_kb > 0) {
        unsigned long sw_used = mi->swap_total_kb - mi->swap_free_kb;
        char su[16], st[16];
        format_kb_unit(sw_used, su, sizeof(su));
        format_kb_unit(mi->swap_total_kb, st, sizeof(st));
        snprintf(text, sizeof(text), "%s/%s", su, st);
        const double fracs[1] = {(double) sw_used / (double) mi->swap_total_kb};
        const char *colors[1] = {C_BAR_RED};
        fputs("\033[K", stdout);
        draw_meter("Swp", mem_inner, fracs, colors, 1, text);
        putchar('\n');
        rows++;
    }

    header_rows_drawn = rows;
}

static void draw_column_header(int cols) {
    // Reproduce COL_FMT per column so the active sort column can get its own
    // highlight color.
    char line[64];
    fputs("\033[K", stdout);
    fputs(C_HDR, stdout);
    snprintf(line, sizeof(line), "%6s %-8.8s %3s %3s %7s %7s %1s %-7s ",
             "PID", "USER", "PR", "NI", "VIRT", "RES", "S", "ARCH");
    fputs(line, stdout);
    printf("%s%5s%s%s %s%5s%s%s %s%8s%s%s ",
           sort_mode == SORT_CPU ? C_HDR_SORT : "", "%CPU", C_RESET, C_HDR,
           sort_mode == SORT_MEM ? C_HDR_SORT : "", "%MEM", C_RESET, C_HDR,
           sort_mode == SORT_TIME ? C_HDR_SORT : "", "TIME+", C_RESET, C_HDR);
    // Pad "Command" out to the full width so the green bar spans the line.
    int rest = cols - command_column_start();
    if (rest < 7)
        rest = 7;
    printf("%-*.*s%s\n", rest, rest, "Command", C_RESET);
}

static void draw_key_bar(int cols) {
    fputs("\033[K", stdout);
    if (status_msg[0] != '\0') {
        printf("%s%-*.*s%s", C_KEYBAR, cols, cols, status_msg, C_RESET);
        fflush(stdout);
        return;
    }
    static const struct { const char *key, *label; } keys[] = {
        {"P", "SortCpu"}, {"M", "SortMem"}, {"T", "SortTime"}, {"N", "SortPid"},
        {"c", "Cmdline"}, {"1", "CpuSum"}, {"k", "Kill"}, {"q", "Quit"},
    };
    int used = 0;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        int w = (int) (strlen(keys[i].key) + strlen(keys[i].label)) + 1;
        if (used + w > cols)
            break;
        printf("%s%s%s%s%s%s ", C_KEY, keys[i].key, C_RESET,
               C_KEYBAR, keys[i].label, C_RESET);
        used += w;
    }
    fflush(stdout);
}

static void draw_interactive(struct proc_sample *procs, int n,
                             int ncpu, const struct cpu_ticks *cur_cpu,
                             const struct cpu_ticks *prev_cpu,
                             const struct meminfo *mi,
                             long clk_tck, long page_kb, double elapsed_sec,
                             int selected, int *scroll_top) {
    int rows, cols;
    term_size(&rows, &cols);

    int running = 0;
    for (int i = 0; i < n; i++)
        if (procs[i].state == 'R')
            running++;

    fputs("\033[H", stdout); // home; each line clears itself with \033[K
    draw_header(cols, ncpu, cur_cpu, prev_cpu, n, running, mi);
    draw_column_header(cols);

    // Total lines drawn each frame is header_rows_drawn + list_rows + 2 (column
    // header + key bar), and must never exceed the terminal's actual row count:
    // ktop repaints in place (cursor-home + per-line clear-to-EOL, no full-screen
    // clear each frame), so one line too many scrolls the whole terminal up by
    // that much instead of just being clipped -- the display visibly shifts.
    // Clamping the floor to 3 (instead of 0) used to do exactly that: whenever a
    // resize left rows - header_rows_drawn - 2 at 0..2, list_rows got forced up
    // to 3, overshooting the terminal height by 1-3 lines and shifting the whole
    // frame up by that much on every redraw until the window grew past it again.
    int list_rows = rows - header_rows_drawn - 2; // column header + key bar
    if (list_rows < 0)
        list_rows = 0;
    if (selected < *scroll_top)
        *scroll_top = selected;
    if (selected >= *scroll_top + list_rows)
        *scroll_top = selected - list_rows + 1;
    if (*scroll_top > n - list_rows)
        *scroll_top = n - list_rows;
    if (*scroll_top < 0)
        *scroll_top = 0;

    for (int row = 0; row < list_rows; row++) {
        int i = *scroll_top + row;
        fputs("\033[K", stdout);
        if (i >= n) {
            putchar('\n');
            continue;
        }
        double cpu_pct = cpu_percent(procs[i].cpu_delta, clk_tck, elapsed_sec);
        double mem_pct = mi->total_kb > 0
            ? (double) procs[i].rss_pages * (double) page_kb * 100.0 / (double) mi->total_kb
            : 0;

        char userbuf[32], virt[16], res[16], timebuf[32], state[2];
        username_for(procs[i].uid, userbuf, sizeof(userbuf));
        format_kb(procs[i].vsize / 1024, virt, sizeof(virt));
        format_kb((unsigned long) (procs[i].rss_pages * page_kb), res, sizeof(res));
        format_time_plus(procs[i].utime + procs[i].stime, clk_tck,
                         timebuf, sizeof(timebuf));
        state[0] = procs[i].state;
        state[1] = '\0';

        const char *cmd = (show_cmdline && procs[i].cmdline[0] != '\0')
            ? procs[i].cmdline : procs[i].comm;

        // Truncate the command to the column the header reserved for it. The
        // whole-line clamp below is not enough on its own: a command longer
        // than the line buffer would otherwise be cut at an arbitrary point,
        // and the selected row's padding is computed from this length.
        int cmd_width = cols - command_column_start();
        if (cmd_width < 7)
            cmd_width = 7;

        char cmdbuf[CMDLINE_MAX];
        truncate_display(cmdbuf, sizeof(cmdbuf), cmd, cmd_width);

        char line[512];
        int len = snprintf(line, sizeof(line),
                           "%6d %-8.8s %3ld %3ld %7s %7s %1s %-7s %5.1f %5.1f %8s %s",
                           (int) procs[i].pid, userbuf,
                           procs[i].priority, procs[i].nice,
                           virt, res, state, procs[i].arch,
                           cpu_pct, mem_pct, timebuf, cmdbuf);
        if (len > cols)
            len = cols;
        if (i == selected)
            printf("%s%-*.*s%s\n", C_SEL, cols, len, line, C_RESET);
        else
            printf("%.*s\n", len, line);
    }
    draw_key_bar(cols);
    fflush(stdout);
}

// Prompt (on the key bar line) for a signal to send to pid. Blocks on stdin;
// Enter with no digits sends SIGTERM, Esc/q cancels.
static void kill_prompt(pid_t pid, const char *comm, int rows, int cols) {
    char prompt[160];
    snprintf(prompt, sizeof(prompt),
             "Send signal to %d (%s): 15=TERM 9=KILL 1=HUP 2=INT, Enter=15, Esc=cancel > ",
             (int) pid, comm);
    printf("\033[%d;1H\033[K%s%-*.*s%s\033[%d;%dH",
           rows, C_KEYBAR, cols, cols, prompt, C_RESET,
           rows, (int) strlen(prompt) + 1);
    fflush(stdout);

    char digits[8] = {0};
    size_t ndig = 0;
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, NULL) <= 0)
            return;
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1)
            continue;
        if (c == '\033' || c == 'q') {
            snprintf(status_msg, sizeof(status_msg), "Kill cancelled");
            return;
        }
        if (c == '\r' || c == '\n')
            break;
        if (isdigit((unsigned char) c) && ndig < sizeof(digits) - 1) {
            digits[ndig++] = c;
            putchar(c);
            fflush(stdout);
        }
    }
    int sig = ndig > 0 ? atoi(digits) : SIGTERM;
    if (sig <= 0 || sig >= 64) {
        snprintf(status_msg, sizeof(status_msg), "Bad signal %s", digits);
        return;
    }
    if (kill(pid, sig) == 0)
        snprintf(status_msg, sizeof(status_msg), "Sent signal %d to %d (%s)",
                 sig, (int) pid, comm);
    else
        snprintf(status_msg, sizeof(status_msg), "kill %d: %s",
                 (int) pid, strerror(errno));
}

// ---- batch mode (original plain top-style output, kept for scripts) ----------

static void print_batch(struct proc_sample *cur, int cur_n,
                        const struct meminfo *mi,
                        long clk_tck, long page_kb, double elapsed_sec) {
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char timebuf[16];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm_now);

    char uptime[32];
    format_uptime(uptime, sizeof(uptime));
    double l1, l5, l15;
    read_loadavg(&l1, &l5, &l15);
    printf("ktop - %s up %s, load average: %.2f, %.2f, %.2f\n",
           timebuf, uptime, l1, l5, l15);

    int running = 0, sleeping = 0, stopped = 0, zombie = 0;
    for (int i = 0; i < cur_n; i++) {
        switch (cur[i].state) {
            case 'R': running++; break;
            case 'S': case 'D': sleeping++; break;
            case 'T': case 't': stopped++; break;
            case 'Z': zombie++; break;
            default: break;
        }
    }
    printf("Tasks: %d total, %d running, %d sleeping, %d stopped, %d zombie\n",
           cur_n, running, sleeping, stopped, zombie);

    unsigned long used_kb = mi->total_kb > mi->free_kb ? mi->total_kb - mi->free_kb : 0;
    printf("Mem: %lukB total, %lukB used, %lukB free\n",
           mi->total_kb, used_kb, mi->free_kb);

    printf("\n%6s %-8s %3s %3s %8s %8s %-7s %5s %5s  %s\n",
           "PID", "USER", "PR", "NI", "VIRT(K)", "RES(K)", "ARCH", "%CPU", "%MEM", "COMMAND");

    for (int i = 0; i < cur_n; i++) {
        double cpu_pct = cpu_percent(cur[i].cpu_delta, clk_tck, elapsed_sec);
        double mem_pct = mi->total_kb > 0
            ? (double) cur[i].rss_pages * (double) page_kb * 100.0 / (double) mi->total_kb
            : 0;
        char userbuf[32];
        username_for(cur[i].uid, userbuf, sizeof(userbuf));
        // The fixed columns above occupy 60; give the command whatever is
        // left of the terminal. When stdout is not a tty there is no width to
        // respect, so nothing is cut -- a redirected snapshot should keep the
        // whole name.
        char cmdbuf[CMDLINE_MAX];
        struct winsize bws;
        int bcols = 0;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &bws) == 0 && bws.ws_col > 0)
            bcols = bws.ws_col;
        if (bcols > 0) {
            int w = bcols - 60;
            truncate_display(cmdbuf, sizeof(cmdbuf), cur[i].comm, w < 7 ? 7 : w);
        } else {
            snprintf(cmdbuf, sizeof(cmdbuf), "%s", cur[i].comm);
        }
        printf("%6d %-8s %3ld %3ld %8lu %8ld %-7s %5.1f %5.1f  %s\n",
               (int) cur[i].pid, userbuf, cur[i].priority, cur[i].nice,
               cur[i].vsize / 1024, cur[i].rss_pages * page_kb,
               cur[i].arch, cpu_pct, mem_pct, cmdbuf);
    }
    fflush(stdout);
}

// ---- main --------------------------------------------------------------------

// Every file-scope static this program keeps, put back to the value the C
// runtime would have given it.
//
// ktop is built two ways from this one source: as an ordinary guest binary,
// where a fresh process makes this a no-op, and compiled into iSH-AOK as a
// native program (kernel/native.h), where `ktop` is a C function called on a
// guest task's thread inside a process that may have run it before. There the
// initialisers above ran once, at app start, and every later run inherits
// whatever the previous one left.
//
// Both halves of this were measured, not assumed, by running one ktop under a
// pty and looking at what the NEXT one did:
//
//  - sort_mode, show_cmdline and show_cpu_summary are the view. Sort by %MEM,
//    quit, start ktop again: without the reset the second run came up sorted
//    by %MEM, not the %CPU the manual documents.
//  - enable_colors is one-way and only runs when stdout is a tty. A later run
//    whose stdout is REDIRECTED therefore never turns colour on, but still
//    found it on: 42 SGR sequences in the output file, 0 with the reset.
//    (Batch mode is not affected either way -- print_batch references none of
//    the colour variables.)
static void reset_state(void) {
    disable_colors();
    guest_arch_cached = NULL;
    host_arch_cached = NULL;
    sort_mode = SORT_CPU;
    show_cmdline = true;
    show_cpu_summary = false;
    status_msg[0] = '\0';
    header_rows_drawn = 0;
    memset(&orig_termios, 0, sizeof(orig_termios));
    termios_saved = false;
    prev_sigint = prev_sigterm = prev_sighup = SIG_DFL;
    handlers_installed = false;
}

int main(int argc, char **argv) {
    reset_state();
    bool batch = false;
    int iterations = -1; // -1 = unbounded (interactive default)
    double delay = 3.0;

    int opt;
    while ((opt = getopt(argc, argv, "bn:d:h")) != -1) {
        switch (opt) {
            case 'b': batch = true; break;
            case 'n': iterations = atoi(optarg); break;
            case 'd': delay = atof(optarg); break;
            case 'h':
                printf("usage: %s [-b] [-n iterations] [-d seconds]\n", argv[0]);
                return 0;
            default:
                fprintf(stderr, "usage: %s [-b] [-n iterations] [-d seconds]\n", argv[0]);
                return 2;
        }
    }
    if (batch && iterations < 0)
        iterations = 1;

    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0)
        clk_tck = 100;
    long page_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (page_kb <= 0)
        page_kb = 4;

    static struct proc_sample bufs[2][MAX_PROCS];
    int counts[2] = {0, 0};
    int cur_buf = 0;

    static struct cpu_ticks cpu_bufs[2][MAX_CPUS];
    int ncpu = 0;
    int cur_cpu_buf = 0;

    struct meminfo mi;

    if (batch) {
        // ---- original plain-top behavior, unchanged for scripts ----
        struct timespec sampled_at;
        clock_gettime(CLOCK_MONOTONIC, &sampled_at);
        counts[cur_buf] = collect(bufs[cur_buf], MAX_PROCS);
        fill_cpu_deltas(bufs[cur_buf], counts[cur_buf], NULL, 0);
        sort_mode = SORT_CPU;
        qsort(bufs[cur_buf], (size_t) counts[cur_buf], sizeof(bufs[0][0]), cmp_procs);
        read_meminfo(&mi);
        // First snapshot has no previous sample, so every delta is 0 and the
        // elapsed value is irrelevant -- pass the delay so the column is 0.0
        // rather than a division by zero.
        print_batch(bufs[cur_buf], counts[cur_buf], &mi, clk_tck, page_kb, delay);
        for (int iter = 1; iterations < 0 || iter < iterations; iter++) {
            struct timespec ts = {(time_t) delay, (long) ((delay - (time_t) delay) * 1e9)};
            nanosleep(&ts, NULL);
            int prev_buf = cur_buf;
            cur_buf ^= 1;
            double elapsed = elapsed_since(&sampled_at);
            clock_gettime(CLOCK_MONOTONIC, &sampled_at);
            counts[cur_buf] = collect(bufs[cur_buf], MAX_PROCS);
            fill_cpu_deltas(bufs[cur_buf], counts[cur_buf],
                            bufs[prev_buf], counts[prev_buf]);
            qsort(bufs[cur_buf], (size_t) counts[cur_buf], sizeof(bufs[0][0]), cmp_procs);
            read_meminfo(&mi);
            print_batch(bufs[cur_buf], counts[cur_buf], &mi, clk_tck, page_kb, elapsed);
        }
        return 0;
    }

    // ---- interactive htop-style mode ----
    if (isatty(STDOUT_FILENO))
        enable_colors();
    enable_raw_stdin();
    // Alternate screen buffer + clear, like htop: quitting restores whatever
    // was on the terminal before ktop started (see restore_terminal).
    fputs("\033[?1049h\033[2J", stdout);

    counts[cur_buf] = collect(bufs[cur_buf], MAX_PROCS);
    fill_cpu_deltas(bufs[cur_buf], counts[cur_buf], NULL, 0);
    ncpu = read_cpu_ticks(cpu_bufs[cur_cpu_buf], MAX_CPUS);
    if (ncpu == 0)
        ncpu = 1;
    // First draw has no previous CPU sample; diff against itself (all-zero
    // deltas, meters empty) rather than garbage.
    memcpy(cpu_bufs[cur_cpu_buf ^ 1], cpu_bufs[cur_cpu_buf],
           sizeof(cpu_bufs[0][0]) * (size_t) ncpu);
    read_meminfo(&mi);

    qsort(bufs[cur_buf], (size_t) counts[cur_buf], sizeof(bufs[0][0]), cmp_procs);

    int selected = 0;
    int scroll_top = 0;
    pid_t selected_pid = counts[cur_buf] > 0 ? bufs[cur_buf][0].pid : -1;
    int iter = 1;

    // Wall time covered by the deltas currently on screen. The first draw has
    // no previous sample (all deltas 0), so any positive value does.
    double sample_elapsed = delay;
    struct timespec sampled_at;
    clock_gettime(CLOCK_MONOTONIC, &sampled_at);

    draw_interactive(bufs[cur_buf], counts[cur_buf], ncpu,
                     cpu_bufs[cur_cpu_buf], cpu_bufs[cur_cpu_buf ^ 1],
                     &mi, clk_tck, page_kb, sample_elapsed, selected, &scroll_top);

    struct timespec next_refresh;
    clock_gettime(CLOCK_MONOTONIC, &next_refresh);
    next_refresh.tv_sec += (time_t) delay;

    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double until = (double) (next_refresh.tv_sec - now.tv_sec)
                     + (double) (next_refresh.tv_nsec - now.tv_nsec) / 1e9;
        bool need_redraw = false;
        bool need_resort = false;

        if (until <= 0) {
            if (iterations >= 0 && iter >= iterations)
                break;
            int prev_buf = cur_buf;
            cur_buf ^= 1;
            // Measured, not assumed: a redraw can be late (a slow /proc walk,
            // a busy device), and the reading must stay honest when it is.
            sample_elapsed = elapsed_since(&sampled_at);
            clock_gettime(CLOCK_MONOTONIC, &sampled_at);
            counts[cur_buf] = collect(bufs[cur_buf], MAX_PROCS);
            fill_cpu_deltas(bufs[cur_buf], counts[cur_buf],
                            bufs[prev_buf], counts[prev_buf]);
            cur_cpu_buf ^= 1;
            int n = read_cpu_ticks(cpu_bufs[cur_cpu_buf], MAX_CPUS);
            if (n > 0)
                ncpu = n;
            read_meminfo(&mi);
            need_resort = true;
            iter++;
            clock_gettime(CLOCK_MONOTONIC, &next_refresh);
            next_refresh.tv_sec += (time_t) delay;
            next_refresh.tv_nsec += (long) ((delay - (time_t) delay) * 1e9);
            if (next_refresh.tv_nsec >= 1000000000L) {
                next_refresh.tv_nsec -= 1000000000L;
                next_refresh.tv_sec++;
            }
        } else {
            enum key k = read_key(until > 0.25 ? 0.25 : until);
            int n = counts[cur_buf];
            switch (k) {
                case KEY_QUIT:
                    goto done;
                case KEY_UP:
                    if (selected > 0)
                        selected--;
                    need_redraw = true;
                    break;
                case KEY_DOWN:
                    if (selected < n - 1)
                        selected++;
                    need_redraw = true;
                    break;
                case KEY_PGUP: {
                    int rows, cols;
                    term_size(&rows, &cols);
                    selected -= rows - header_rows_drawn - 2;
                    if (selected < 0)
                        selected = 0;
                    need_redraw = true;
                    break;
                }
                case KEY_PGDN: {
                    int rows, cols;
                    term_size(&rows, &cols);
                    selected += rows - header_rows_drawn - 2;
                    if (selected > n - 1)
                        selected = n - 1;
                    need_redraw = true;
                    break;
                }
                case KEY_HOME:
                    selected = 0;
                    need_redraw = true;
                    break;
                case KEY_END:
                    selected = n - 1;
                    need_redraw = true;
                    break;
                case KEY_SORT_CPU: sort_mode = SORT_CPU; need_resort = true; break;
                case KEY_SORT_MEM: sort_mode = SORT_MEM; need_resort = true; break;
                case KEY_SORT_TIME: sort_mode = SORT_TIME; need_resort = true; break;
                case KEY_SORT_PID: sort_mode = SORT_PID; need_resort = true; break;
                case KEY_CMDLINE:
                    show_cmdline = !show_cmdline;
                    need_redraw = true;
                    break;
                case KEY_TOGGLE_CPU:
                    show_cpu_summary = !show_cpu_summary;
                    need_redraw = true;
                    break;
                case KEY_KILL:
                    if (n > 0 && selected < n) {
                        int rows, cols;
                        term_size(&rows, &cols);
                        kill_prompt(bufs[cur_buf][selected].pid,
                                    bufs[cur_buf][selected].comm, rows, cols);
                        need_redraw = true;
                    }
                    break;
                case KEY_OTHER:
                    status_msg[0] = '\0';
                    need_redraw = true;
                    break;
                case KEY_NONE:
                default:
                    break;
            }
            if (selected >= 0 && selected < n && k != KEY_NONE)
                selected_pid = bufs[cur_buf][selected].pid;
        }

        if (need_resort) {
            qsort(bufs[cur_buf], (size_t) counts[cur_buf], sizeof(bufs[0][0]), cmp_procs);
            // Keep the cursor on the same process across refreshes/resorts,
            // like htop; fall back to clamping the index if it's gone.
            int found = -1;
            for (int i = 0; i < counts[cur_buf]; i++) {
                if (bufs[cur_buf][i].pid == selected_pid) {
                    found = i;
                    break;
                }
            }
            if (found >= 0)
                selected = found;
            else if (selected > counts[cur_buf] - 1)
                selected = counts[cur_buf] > 0 ? counts[cur_buf] - 1 : 0;
            need_redraw = true;
        }
        if (need_redraw) {
            draw_interactive(bufs[cur_buf], counts[cur_buf], ncpu,
                             cpu_bufs[cur_cpu_buf], cpu_bufs[cur_cpu_buf ^ 1],
                             &mi, clk_tck, page_kb, sample_elapsed,
                             selected, &scroll_top);
        }
    }
done:
    // Flush before teardown_terminal, so anything still buffered is written
    // while the alternate screen is still up rather than spilling onto the
    // restored one.
    fflush(stdout);
    teardown_terminal();
    return 0;
}
