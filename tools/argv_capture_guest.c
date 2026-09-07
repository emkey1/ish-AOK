// argv_capture_guest.c -- record how a program was actually started, then run it.
//
// WHY THIS EXISTS. On 2026-09-07 a login MOTD on the 5th-gen iPad printed a
// line that could not be produced by the command the script contains. The
// script runs
//
//     command df -h / 2>/dev/null | awk 'NR==2{print $3" / "$2" ("$5")"}'
//
// and what came out was df's *1K-blocks* header -- so no `-h' had reached df,
// no row for `/' was printed, awk had not filtered anything, and df's stderr
// reached the terminal despite the 2>/dev/null. On the same screen `uptime'
// printed nothing. The next login was clean and it has not been seen since.
//
// Every one of those symptoms is about how the CHILD was started rather than
// about what the child did: its argument vector, and the descriptors it was
// handed. That is what this records. Root's login shell there is
// /AOK/native/bash, which cannot fork (kernel/native_libc.c's nlibc_fork), so
// every pipeline stage and every $( ) is a posix_spawn whose argv and whose
// fd wiring are assembled by the parent shell -- deps/bash/aok_fork.c. A
// wrong argv or a mis-wired descriptor is therefore a plausible defect and not
// a wild guess, and this is the cheapest instrument that can tell the two
// apart the next time it fires.
//
// WHY A C PROGRAM AND NOT A SHELL WRAPPER. A shell wrapper would change the
// thing being measured: the shell would exec an interpreter, and the failure
// under investigation is in the exec/spawn path itself. This is an ordinary
// dynamically linked ELF, so the shell's spawn of it has exactly the shape its
// spawn of the real binary had.
//
// WHAT IT RECORDS, and why each field earns its place:
//
//   argv[]                  -- what main() actually received. This is the
//                              primary evidence: the reported failure is an
//                              argument vector that cannot have come from the
//                              script.
//   /proc/self/cmdline      -- what the kernel recorded for the same exec.
//                              Logged SEPARATELY rather than instead, because
//                              the two disagreeing would itself be the finding
//                              (kernel/exec.c copies argv into the new mm; if
//                              that copy is what is wrong, only this shows it).
//   fd 0,1,2                -- where the standard descriptors point. This is
//                              what separates "argv was wrong" from "the pipe
//                              was mis-wired": in the observed failure df's
//                              output reached the command substitution without
//                              passing through awk, which is a descriptor
//                              question, and its stderr escaped a 2>/dev/null,
//                              which is another one.
//   ppid and the parent's cmdline
//                           -- names the shell that did the spawning, so a
//                              failure can be attributed to a re-launched
//                              subshell rather than to the login shell.
//   loadavg                 -- the one observation was at load 3.96 on two
//                              cores, and the suspicion is a race, so the load
//                              at the moment of the exec is evidence.
//
// It is written with ONE write(2) of one assembled buffer to a file opened
// O_APPEND. Concurrent writers are the normal case here -- a pipeline is
// several of these at once -- and an append-mode write of a single buffer is
// the only cheap way to keep their records from interleaving.
//
// It is FAIL-OPEN throughout. Every failure to record is ignored and the real
// program runs anyway: an instrument that can break the system it observes
// would be turned off, and then it would not be there when the bug fires.
//
// USE: tools/install-argv-capture.sh installs it as /usr/local/bin/<name> for
// each command to watch; /usr/local/bin precedes /usr/bin on the guest's PATH,
// so `command df' finds this and this finds the real df. Recording is gated on
// the flag file existing, so it can be silenced without uninstalling.

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Overridable at compile time only, so that the self-test below can point them
// somewhere writable. NOT read from the environment: this program stands in
// front of commands the shell under investigation runs, and letting that
// shell's environment redirect the log would make the instrument part of the
// thing being measured.
#ifndef LOG_PATH
#define LOG_PATH  "/var/log/aok-argv-capture.log"
#endif
#ifndef FLAG_PATH
#define FLAG_PATH "/var/log/aok-argv-capture.on"
#endif

// The directories a real command may live in. /usr/local/* is deliberately NOT
// here: that is where this program is installed, and searching it would find
// this program again and exec it forever.
static const char *const REAL_DIRS[] = {
    "/usr/bin", "/bin", "/usr/sbin", "/sbin", NULL,
};

// One record can be long -- a shell's cmdline plus an argv -- but it must stay
// one write(), so it is bounded rather than grown.
#define REC_MAX 8192

struct rec {
    char buf[REC_MAX];
    size_t n;
};

static void rec_puts(struct rec *r, const char *s) {
    if (s == NULL)
        return;
    size_t len = strlen(s);
    if (len > REC_MAX - 1 - r->n)
        len = REC_MAX - 1 - r->n;   // truncate; never overflow
    memcpy(r->buf + r->n, s, len);
    r->n += len;
}

static void rec_printf(struct rec *r, const char *fmt, ...)
        __attribute__((format(printf, 2, 3)));

static void rec_printf(struct rec *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t room = REC_MAX - 1 - r->n;
    int got = vsnprintf(r->buf + r->n, room + 1, fmt, ap);
    va_end(ap);
    if (got < 0)
        return;
    r->n += ((size_t) got > room) ? room : (size_t) got;
}

// A NUL-separated /proc file (cmdline) rendered as bracketed fields, which is
// what makes an empty or a whitespace-bearing argument visible. Reading it into
// a fixed buffer is deliberate: this must not allocate on a path that runs
// while the system is under the memory pressure the bug appeared under.
static void rec_proc_nul(struct rec *r, const char *path) {
    char buf[4096];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        rec_puts(r, " <unreadable>");
        return;
    }
    ssize_t got = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (got <= 0) {
        rec_puts(r, " <empty>");
        return;
    }
    // Terminates the last field even when the read filled the buffer, so the
    // walk below can rely on every field being a C string.
    buf[got] = '\0';
    // Every field is bracketed, INCLUDING an empty one: `df ""' and `df' with
    // the argument missing entirely are different failures and have to look
    // different in the log.
    for (ssize_t pos = 0; pos < got; pos += (ssize_t) strlen(buf + pos) + 1) {
        rec_puts(r, " [");
        rec_puts(r, buf + pos);
        rec_puts(r, "]");
    }
}

static void rec_readlink(struct rec *r, const char *path) {
    char buf[1024];
    ssize_t got = readlink(path, buf, sizeof(buf) - 1);
    if (got < 0) {
        rec_puts(r, "<none>");
        return;
    }
    buf[got] = '\0';
    rec_puts(r, buf);
}

static void rec_file_line(struct rec *r, const char *path) {
    char buf[256];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    ssize_t got = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (got <= 0)
        return;
    buf[got] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl != NULL)
        *nl = '\0';
    rec_puts(r, buf);
}

static void capture(int argc, char **argv) {
    // Gated so the instrument can be silenced in place. Checked before any
    // other work so the disabled cost is one stat().
    struct stat st;
    if (stat(FLAG_PATH, &st) != 0)
        return;

    struct rec r = {.n = 0};

    struct timespec ts = {0, 0};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    char when[64] = "?";
    if (gmtime_r(&ts.tv_sec, &tm) != NULL)
        strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%S", &tm);

    rec_printf(&r, "%s.%03ldZ pid=%ld ppid=%ld uid=%ld load=",
               when, ts.tv_nsec / 1000000L, (long) getpid(), (long) getppid(),
               (long) getuid());
    rec_file_line(&r, "/proc/loadavg");
    rec_puts(&r, "\n");

    // What main() received.
    rec_puts(&r, "  argv   :");
    for (int i = 0; i < argc; i++) {
        rec_puts(&r, " [");
        rec_puts(&r, argv[i] != NULL ? argv[i] : "<null>");
        rec_puts(&r, "]");
    }
    rec_puts(&r, "\n");

    // What the kernel recorded for the same exec. Disagreement with the line
    // above is itself the finding -- see the header.
    rec_puts(&r, "  cmdline:");
    rec_proc_nul(&r, "/proc/self/cmdline");
    rec_puts(&r, "\n");

    // Where the standard descriptors point: pipe, tty, /dev/null or a file.
    for (int fd = 0; fd < 3; fd++) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        rec_printf(&r, "  fd%d    : ", fd);
        rec_readlink(&r, path);
        rec_puts(&r, "\n");
    }

    // The shell that spawned this.
    char ppath[64];
    snprintf(ppath, sizeof(ppath), "/proc/%ld/cmdline", (long) getppid());
    rec_puts(&r, "  parent :");
    rec_proc_nul(&r, ppath);
    rec_puts(&r, "\n");

    int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    // One write of one buffer, to a descriptor opened O_APPEND: that is what
    // keeps the records of a pipeline's concurrent stages from interleaving.
    // The result is deliberately unused -- see the fail-open note above.
    ssize_t wrote = write(fd, r.buf, r.n);
    (void) wrote;
    close(fd);
}

// The real program this stands in front of: the same basename, in the first of
// REAL_DIRS that has it. Resolved at run time rather than baked in at install
// time so one binary serves every command it is installed as.
static const char *find_real(const char *argv0, char *out, size_t outlen) {
    const char *base = strrchr(argv0, '/');
    base = (base != NULL) ? base + 1 : argv0;
    for (int i = 0; REAL_DIRS[i] != NULL; i++) {
        snprintf(out, outlen, "%s/%s", REAL_DIRS[i], base);
        if (access(out, X_OK) == 0)
            return out;
    }
    return NULL;
}

int main(int argc, char **argv) {
    capture(argc, argv);

    char real[512];
    if (find_real(argv[0], real, sizeof(real)) == NULL) {
        fprintf(stderr, "argv-capture: no real program behind %s\n", argv[0]);
        return 127;
    }
    execv(real, argv);
    // Only reached if the exec failed, and then the message is the useful part.
    fprintf(stderr, "argv-capture: exec %s failed\n", real);
    return 126;
}
