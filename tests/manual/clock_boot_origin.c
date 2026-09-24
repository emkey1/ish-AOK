/*
 * clock_boot_origin -- the guest's boot-relative clocks measure time since the
 * GUEST booted, and absolute deadlines on them still mean what they say.
 *
 * AOK served CLOCK_MONOTONIC and CLOCK_BOOTTIME straight from the host's
 * clock of the same name, so a guest read the HOST's uptime. On a Mac that had
 * been up 14.6 days a guest two seconds old reported:
 *
 *     clock_gettime(CLOCK_BOOTTIME)  -> 1264336.353625000
 *     clock_gettime(CLOCK_MONOTONIC) -> 1264336.353646000
 *     /proc/uptime                   -> 0.77 2.60
 *     /proc/stat btime               -> 1789675963   (correct)
 *
 * Relative measurements were all fine, which is why this survived: only
 * absolute ones were wrong. Anything that reconstructs a wall-clock instant as
 * "now minus CLOCK_BOOTTIME" landed 14.6 days in the past, and util-linux's
 * dmesg does exactly that in get_boot_time_hires() -- so `dmesg -T` in a
 * Devuan guest printed "Thu Sep  3" for records logged on Sep 17.
 *
 * Linux is the oracle for every expectation here; measured on Devuan 6 /
 * Linux 6.12, where CLOCK_BOOTTIME and /proc/uptime agreed to within the gap
 * between the two reads and floor(CLOCK_REALTIME - uptime) was btime exactly.
 *
 * The second half is the other side of the same change, and the reason it is
 * risky. CLOCK_MONOTONIC backs timers, futex timeouts, condvar waits, timerfd
 * and POSIX timers. Moving the origin the guest READS without also moving the
 * one the kernel uses to INTERPRET a guest deadline turns every absolute
 * monotonic wait into a deadline 14 days expired: measured with exactly that
 * half-change in place, clock_nanosleep(TIMER_ABSTIME), timerfd and
 * pthread_cond_timedwait all returned in 0.000 s instead of the half second
 * asked for, and a POSIX timer never fired at all. So this asserts DURATIONS,
 * not error codes -- an instant return is a success return.
 *
 * The CLOCK_REALTIME waits at the end are controls: that path has no boot
 * origin and must be untouched by any of this.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW 4
#endif
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE 6
#endif

static double clock_read(clockid_t c) {
    struct timespec ts;
    if (clock_gettime(c, &ts) != 0)
        return -1;
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static struct timespec clock_plus(clockid_t c, double delta) {
    struct timespec ts;
    clock_gettime(c, &ts);
    ts.tv_sec += (time_t) delta;
    ts.tv_nsec += (long) ((delta - (double) (long) delta) * 1e9);
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_nsec -= 1000000000;
        ts.tv_sec++;
    }
    return ts;
}

static double read_uptime(void) {
    FILE *f = fopen("/proc/uptime", "r");
    if (f == NULL)
        return -1;
    double up;
    int ok = fscanf(f, "%lf", &up) == 1;
    fclose(f);
    return ok ? up : -1;
}

static long read_btime(void) {
    FILE *f = fopen("/proc/stat", "r");
    if (f == NULL)
        return -1;
    char line[512];
    long btime = -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "btime ", 6) == 0) {
            btime = strtol(line + 6, NULL, 10);
            break;
        }
    }
    fclose(f);
    return btime;
}

// Milliseconds, so failf's hex fields read as a number a human can check.
static uint64_t ms(double seconds) {
    double v = seconds < 0 ? -seconds : seconds;
    return (uint64_t) (v * 1000.0 + 0.5);
}

// A wait is correct if it lasted about as long as asked. The floor is what
// catches a mis-converted origin (those returned in well under a millisecond);
// the ceiling is loose because this runs under emulation on a shared host.
static void check_waited(const char *what, double elapsed, double want) {
    // The FLOOR is the assertion: a deadline converted against the wrong clock
    // is already expired, and those returned in under a millisecond. The
    // ceiling only says the wait was not wildly overshot, so it scales with
    // the same knob the watchdogs use -- overshoot measures how busy the host
    // is, and the release suite runs four of these at once.
    if (elapsed < want * 0.5 || elapsed > want + 2.0 * test_watchdog_secs(1)) {
        test_log_if(1, "  %s: waited %.3f s for a %.2f s deadline\n", what, elapsed, want);
        failf(what, ms(elapsed), 0, 0, ms(want), 0, 0);
    } else {
        test_logf("  %s: %.3f s (asked %.2f)\n", what, elapsed, want);
    }
}

static void onsig(int sig) { (void) sig; }

// The kernel log's own wall-clock stamp, read straight out of the byte stream
// with no dmesg in the way.
//
// syslog(2)'s READ_ALL hands back the log's raw bytes -- what `dmesg --syslog`,
// busybox's dmesg and `cat /proc/kmsg` all show -- and every line in it carries
// the stamp kernel/log.c's output_line() writes. That stamp is guest-visible
// text, so it is in UTC: the host's timezone is not something a guest can
// learn, and the guest's own is a userspace file no kernel reads. Read back
// with timegm() it must therefore agree with the guest's own clock.
//
// It did not. output_line() rendered the HOST's local time with ctime(3), so on
// a host an hour off UTC every line read an hour into the FUTURE: `dmesg -S -T`
// printed [Fri Sep 18 06:29:43 2026] in a guest whose own `date` said 05:29:43,
// and check_dmesg_ctime below failed at -3600 s. That check needs util-linux's
// dmesg AND some earlier test to have logged something, which is why it fired
// only in a full suite run on one root; this one runs on every root and logs
// its own line.
//
// A host that is itself on UTC cannot show the difference -- there is nothing
// to see. To exercise it deliberately, force the host's zone far from UTC:
// TZ=Pacific/Kiritimati ./build/ish -f <root> ...
static void check_log_stamp_clock(double up) {
    // /dev/kmsg is 0644 and root-owned, as on Linux. An unprivileged run just
    // reads whatever is already in the log, which is no weaker an assertion --
    // the boot banner alone is enough.
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) {
        dprintf(fd, "clock_boot_origin stamp probe\n");
        close(fd);
    }

    static char buf[65536];
    // READ_ALL is a peek: it does not consume, unlike READ, so it cannot steal
    // records from anything else reading the log.
    int n = (int) syscall(SYS_syslog, 3 /* SYSLOG_ACTION_READ_ALL */,
                          buf, (int) sizeof(buf) - 1);
    double now = clock_read(CLOCK_REALTIME);
    if (n <= 0) {
        test_log_if(1, "  SKIP kernel-log stamp: syslog(READ_ALL) returned %d (%s)\n",
                    n, n < 0 ? strerror(errno) : "empty log");
        return;
    }
    buf[n] = '\0';

    // The newest stamp in the buffer. A truncated first line (READ_ALL gives
    // the LAST n bytes) simply does not parse, and neither does a '[' inside a
    // message, so scanning every one of them and keeping the latest is safe.
    time_t newest = 0;
    char newest_stamp[64] = "";
    for (const char *p = buf; (p = strchr(p, '[')) != NULL; p++) {
        struct tm tm;
        memset(&tm, 0, sizeof(tm));
        const char *end = strptime(p + 1, "%a %b %e %H:%M:%S %Y", &tm);
        if (end == NULL || *end != ']')
            continue;
        time_t t = timegm(&tm);
        if (t == (time_t) -1 || t < newest)
            continue;
        newest = t;
        snprintf(newest_stamp, sizeof(newest_stamp), "%.*s",
                 (int) (end - (p + 1)), p + 1);
    }
    if (newest == 0) {
        test_log_if(1, "  SKIP kernel-log stamp: no stamped line in %d bytes of log\n", n);
        return;
    }

    double behind = now - (double) newest;
    test_logf("kernel log's newest stamp [%s] is %.0f s behind the guest's clock\n",
              newest_stamp, behind);
    // Exact bounds, not a tolerance: the stamp is whole seconds, so it reads up
    // to a second EARLY and never late, and no line can predate boot, so it is
    // never more than uptime old. An hour either way is 3600 times the slack.
    if (behind < -1.0 || behind > up + 1.0) {
        test_log_if(1, "  the log stamps [%s], the guest's clock says %.0f: %.0f s "
                       "apart, and the guest booted %.1f s ago\n",
                    newest_stamp, now, behind, up);
        // Both instants in epoch seconds rather than a signed difference --
        // failf's fields are unsigned, and the two numbers say which way and
        // by how much without any decoding.
        failf("the kernel log's stamp is on the guest's clock (epoch s)",
              (uint64_t) newest, 0, 0, (uint64_t) now, 0, 0);
    }
}

// The symptom itself, through the program that showed it. `dmesg -T` renders a
// record's monotonic stamp as a wall-clock time by adding get_boot_time_hires()
// to it, in LOCAL time -- which is also how mktime reads it back, so the two
// cancel and no timezone question arises.
//
// ONLY for dmesg's OWN stamp, which is the one at column 0. There are two
// bracketed stamps on a line here and they are in different zones:
//
//   [Fri Sep 18 07:05:39 2026] [Fri Sep 18 14:05:28 2026] iSH-AOK 5.20.66 ...
//    dmesg -T, LOCAL             kernel/log.c output_line(), UTC
//
// AOK stamps every stored line itself, and util-linux treats a leading `[` as
// its own timestamp only when the next byte is a digit or a space -- so
// `[Fri ...]` stays in the message text and dmesg prepends its rendering in
// front of it. Continuation lines of a multi-line record get NO stamp from
// dmesg, so the first `[...]` on those is the UTC one.
//
// Scanning for the first `[...]` on every line therefore mixes local and UTC
// stamps, and picking the newest of that mixture reads a UTC stamp as local --
// a record 7 hours in the future on a PDT guest. It passed everywhere it was
// run because every Mac test root is Etc/UTC, where the two zones coincide;
// the device is America/Los_Angeles and failed it at once. Take the stamp at
// column 0 and skip any line that has not got one. See kernel/log.c and
// fs/mem.c's kmsg_line_time(), which round-trip the UTC stamp with timegm();
// check_log_stamp_clock() above tests that one directly.
//
// Not every guest has a dmesg that can do this: busybox's takes no -T. That is
// reported rather than silently passed, because a check that never ran looks
// exactly like one that succeeded. The C-level assertion above covers the same
// arithmetic on every root.
//
// The newest record is the one to test: no record can predate boot, so a
// correct timestamp lies between "now minus uptime" and now. The bug put it
// 14.6 days before both.
static void check_dmesg_ctime(double up) {
    static const char *const cmds[] = {
        // -S forces syslog(2). Plain `dmesg -T` reads /dev/kmsg, which is a
        // separate concern and currently prints nothing on a glibc guest.
        "dmesg -S -T 2>/dev/null",
        "dmesg -T 2>/dev/null",
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        FILE *p = popen(cmds[i], "r");
        if (p == NULL)
            continue;
        char line[1024], newest[256] = "";
        time_t newest_t = 0;
        while (fgets(line, sizeof(line), p) != NULL) {
            // Column 0 only: that is dmesg's own rendering, in local time.
            // A line starting with anything else is a continuation, whose
            // only bracketed stamp is the message's own UTC one.
            if (line[0] != '[')
                continue;
            const char *open_bracket = line;
            const char *close_bracket = open_bracket != NULL ? strchr(open_bracket, ']') : NULL;
            if (open_bracket == NULL || close_bracket == NULL ||
                    close_bracket - open_bracket > (long) sizeof(newest))
                continue;
            char stamp[256];
            size_t n = (size_t) (close_bracket - open_bracket - 1);
            memcpy(stamp, open_bracket + 1, n);
            stamp[n] = '\0';
            struct tm tm;
            memset(&tm, 0, sizeof(tm));
            tm.tm_isdst = -1;
            if (strptime(stamp, "%a %b %e %H:%M:%S %Y", &tm) == NULL)
                continue;
            time_t t = mktime(&tm);
            if (t == (time_t) -1)
                continue;
            if (t >= newest_t) {
                newest_t = t;
                snprintf(newest, sizeof(newest), "%s", stamp);
            }
        }
        pclose(p);
        if (newest_t == 0)
            continue;
        time_t now = time(NULL);
        double behind = (double) now - (double) newest_t;
        test_logf("`%s` newest record: [%s], %.0f s behind the wall clock\n",
                  cmds[i], newest, behind);
        // Allow a minute either way for the record's own age and for rounding.
        if (behind > up + 60.0 || behind < -60.0) {
            test_log_if(1, "  dmesg -T says [%s] but the guest booted %.1f s ago "
                           "-- the record reads %.2f days off\n",
                        newest, up, behind / 86400.0);
            failf("dmesg -T timestamps track the wall clock (s behind)",
                  ms(behind) / 1000, 0, 0, 0, 0, 0);
        }
        return;
    }
    test_log_if(1, "  SKIP dmesg -T: no dmesg here that renders ctime timestamps\n");
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    // ---- the origin ------------------------------------------------------
    //
    // Read uptime between two clock reads so the comparison cannot be blamed
    // on the gap between them: whatever the ordering, all three are from the
    // same instant to within a couple of reads.
    double boot0 = clock_read(CLOCK_BOOTTIME);
    double mono = clock_read(CLOCK_MONOTONIC);
    double raw = clock_read(CLOCK_MONOTONIC_RAW);
    double coarse = clock_read(CLOCK_MONOTONIC_COARSE);
    double mono1 = clock_read(CLOCK_MONOTONIC);
    double up = read_uptime();
    double boot1 = clock_read(CLOCK_BOOTTIME);
    double real = clock_read(CLOCK_REALTIME);
    long btime = read_btime();
    // /proc/uptime truncates to hundredths; nothing else here needs slack.
    const double eps = 0.02;

    test_logf("BOOTTIME %.6f  MONOTONIC %.6f  RAW %.6f (%+.6f)  /proc/uptime %.6f  btime %ld\n",
              boot0, mono, raw, raw - mono, up, btime);

    if (up < 0 || btime <= 0) {
        failf("read /proc/uptime and btime", 0, 0, 0, 1, 0, 0);
        return finish_suite("clock_boot_origin");
    }

    // The headline: CLOCK_BOOTTIME *is* uptime. Before the fix this differed
    // by the host's uptime -- 1265947 s on the machine that found it.
    //
    // /proc/uptime was read strictly between the two CLOCK_BOOTTIME reads
    // above, so on a correct kernel it MUST fall between them, however slow
    // the machine is. That makes this a bracket rather than a tolerance: it
    // needs no allowance for load, and a run competing with a compile storm
    // cannot fail it by being slow. (An earlier version allowed a fixed 0.5 s
    // for the gap between the reads and flaked when a concurrent build blew
    // through that -- a tolerance on elapsed time is a tolerance on how busy
    // the host is, which is not a property of the clock.)
    if (up < boot0 - eps || up > boot1 + eps) {
        test_log_if(1, "  /proc/uptime %.6f is outside CLOCK_BOOTTIME [%.6f, %.6f]\n",
                    up, boot0, boot1);
        failf("CLOCK_BOOTTIME is the guest's uptime (ms off)",
              ms(up < boot0 ? up - boot0 : up - boot1), 0, 0, 0, 0, 0);
    }

    // CLOCK_MONOTONIC was read inside the same bracket, so it may not read
    // LATER than the guest's uptime. That is the direction the bug went --
    // every boot-relative clock reported the host's uptime, which is
    // necessarily larger -- and it is also Linux's ordering rule for
    // MONOTONIC against BOOTTIME.
    //
    // No tight lower bound: CLOCK_MONOTONIC legitimately falls BEHIND uptime,
    // by however long the host spent suspended during the guest's life (or a
    // checkpoint sat on disk), and Linux promises only MONOTONIC <= BOOTTIME.
    // What stops a clock stuck near zero from passing is the rate and
    // monotonicity check below, not a floor here.
    if (mono1 > boot1 + eps) {
        test_log_if(1, "  CLOCK_MONOTONIC %.6f is past the guest's uptime %.6f (by %.1f days)\n",
                    mono1, boot1, (mono1 - boot1) / 86400.0);
        failf("a boot-relative clock is not past uptime (ms over)",
              ms(mono1 - boot1), 0, 0, 0, 0, 0);
    }

    // CLOCK_MONOTONIC_COARSE is CLOCK_MONOTONIC as of the last tick, so it
    // lies between the two CLOCK_MONOTONIC reads around it, less up to one
    // tick at the bottom (10 ms at Linux's slowest HZ). Uptime does not bound
    // it from below, for the same reason it does not bound CLOCK_MONOTONIC.
    if (coarse >= 0 && (coarse < mono - eps || coarse > mono1 + eps)) {
        test_log_if(1, "  CLOCK_MONOTONIC_COARSE %.6f is outside CLOCK_MONOTONIC [%.6f, %.6f]\n",
                    coarse, mono, mono1);
        failf("CLOCK_MONOTONIC_COARSE is CLOCK_MONOTONIC to a tick (ms off)",
              ms(coarse < mono ? mono - coarse : coarse - mono1), 0, 0, 0, 0, 0);
    }

    // CLOCK_MONOTONIC_RAW is held to CLOCK_MONOTONIC, the clock Linux keeps
    // it with: both start at boot and both stop across suspend, and all that
    // separates them is that NTP steers MONOTONIC while RAW is the counter
    // unslewed. So they drift apart, and Linux has no RAW <= BOOTTIME rule --
    // RAW reads ahead of uptime whenever the counter runs fast. An earlier
    // version held it to uptime + 20 ms and failed every leg of the parallel
    // gate on 2026-09-24: RAW 142-146 ms ahead after 5750-6430 s, 22-25 ppm,
    // on a Mac whose RAW had been running 3 ppm SLOW over its 21-day uptime.
    // (Linux does the same: camd, with no NTP daemon at all, had RAW 67 ms
    // behind MONOTONIC after 4 days.) The same drift is why AOK gives RAW an
    // origin of its own: one shared with MONOTONIC's would carry the host's
    // whole-uptime difference, 5.5 s on that Mac.
    //
    // The bound is the NTP discipline's own, which Linux and Darwin (whose
    // kern_ntptime.c is FreeBSD's) share: the frequency correction is clamped
    // to MAXFREQ, 500 ppm, and a phase correction to MAXPHASE, 0.5 s -- time
    // daemons step a larger offset instead, which moves CLOCK_REALTIME and
    // neither of these. It needs both terms. On the gate Mac one phase
    // correction moved RAW against MONOTONIC 9.5 ms in its first 10 s,
    // 950 ppm, and 23 ms in all; one a few times that size would outrun
    // 500 ppm plus 20 ms in a guest a few seconds old. It still catches what
    // this test is for: a clock carrying the host's uptime is days out, and
    // one stuck at zero is out by all of the guest's once that passes half a
    // second (before then, the rate check below has it).
    const double maxphase = 0.5, maxfreq = 500e-6;
    double raw_slack = maxphase + maxfreq * mono1;
    if (raw >= 0 && (raw < mono - raw_slack || raw > mono1 + raw_slack)) {
        double off = raw < mono ? mono - raw : raw - mono1;
        test_log_if(1, "  CLOCK_MONOTONIC_RAW %.6f is %.3f s outside CLOCK_MONOTONIC "
                       "[%.6f, %.6f] (%.1f days); NTP slew allows %.3f s\n",
                    raw, off, mono, mono1, off / 86400.0, raw_slack);
        failf("CLOCK_MONOTONIC_RAW tracks CLOCK_MONOTONIC to within NTP slew (ms off)",
              ms(off), 0, 0, ms(raw_slack), 0, 0);
    }

    // ---- what dmesg -T does ----------------------------------------------
    //
    // util-linux's get_boot_time_hires(): gettimeofday() minus CLOCK_BOOTTIME
    // is the instant the machine booted, which is btime. This is the whole
    // user-visible symptom in one line.
    struct timeval now_tv;
    struct timespec boot_ts;
    gettimeofday(&now_tv, NULL);
    clock_gettime(CLOCK_BOOTTIME, &boot_ts);
    double dmesg_boot = ((double) now_tv.tv_sec + (double) now_tv.tv_usec / 1e6) -
                        ((double) boot_ts.tv_sec + (double) boot_ts.tv_nsec / 1e9);
    test_logf("get_boot_time_hires = %.3f, btime %ld, off %.3f s\n",
              dmesg_boot, btime, dmesg_boot - (double) btime);
    if (dmesg_boot < (double) btime - 1.0 || dmesg_boot > (double) btime + 1.5) {
        test_log_if(1, "  realtime - CLOCK_BOOTTIME = %.3f, btime = %ld (off %.1f s, %.2f days)\n",
                    dmesg_boot, btime, dmesg_boot - (double) btime,
                    (dmesg_boot - (double) btime) / 86400.0);
        failf("realtime - CLOCK_BOOTTIME is btime (ms off)",
              ms(dmesg_boot - (double) btime), 0, 0, 0, 0, 0);
    }
    (void) real;

    check_log_stamp_clock(up);
    check_dmesg_ctime(up);

    // ---- monotonic, and running at one second per second -----------------
    double t0 = clock_read(CLOCK_MONOTONIC), raw0 = clock_read(CLOCK_MONOTONIC_RAW);
    double r0 = clock_read(CLOCK_REALTIME);
    double prev = t0, prev_boot = clock_read(CLOCK_BOOTTIME);
    int backward = 0, boot_backward = 0, reads = 0;
    while (clock_read(CLOCK_MONOTONIC) - t0 < 0.5) {
        double m = clock_read(CLOCK_MONOTONIC), b = clock_read(CLOCK_BOOTTIME);
        reads++;
        if (m < prev) {
            backward++;
            test_log_if(backward <= 3, "  CLOCK_MONOTONIC went back: %.9f -> %.9f\n", prev, m);
        }
        if (b < prev_boot) {
            boot_backward++;
            test_log_if(boot_backward <= 3, "  CLOCK_BOOTTIME went back: %.9f -> %.9f\n", prev_boot, b);
        }
        prev = m;
        prev_boot = b;
        usleep(1000);
    }
    double elapsed_mono = clock_read(CLOCK_MONOTONIC) - t0;
    double elapsed_raw = clock_read(CLOCK_MONOTONIC_RAW) - raw0;
    double elapsed_real = clock_read(CLOCK_REALTIME) - r0;
    test_logf("%d reads, %d backward, monotonic advanced %.4f s while realtime advanced %.4f s\n",
              reads, backward, elapsed_mono, elapsed_real);
    if (backward != 0)
        failf("CLOCK_MONOTONIC never goes backward", (uint64_t) backward, 0, 0, 0, 0, 0);
    if (boot_backward != 0)
        failf("CLOCK_BOOTTIME never goes backward", (uint64_t) boot_backward, 0, 0, 0, 0, 0);
    // A rebased clock must keep the host's RATE, not just a plausible value.
    if (elapsed_real > 0.05 && (elapsed_mono < elapsed_real * 0.5 || elapsed_mono > elapsed_real * 2.0)) {
        test_log_if(1, "  monotonic advanced %.4f s while realtime advanced %.4f s\n",
                    elapsed_mono, elapsed_real);
        failf("CLOCK_MONOTONIC advances at the wall clock's rate (ms)",
              ms(elapsed_mono), 0, 0, ms(elapsed_real), 0, 0);
    }
    // CLOCK_MONOTONIC_RAW too. This is what catches a RAW stuck at zero in a
    // guest younger than the half second of slew the origin check allows,
    // which is how young the guest is when the runner finds this test already
    // built. A kernel that measured RAW from MONOTONIC's origin read 0.000000
    // there -- 5.5 s below zero, clamped -- and passed the origin check.
    if (raw0 >= 0 && elapsed_mono > 0.05 &&
            (elapsed_raw < elapsed_mono * 0.5 || elapsed_raw > elapsed_mono * 2.0)) {
        test_log_if(1, "  CLOCK_MONOTONIC_RAW advanced %.4f s while "
                       "CLOCK_MONOTONIC advanced %.4f s\n", elapsed_raw, elapsed_mono);
        failf("CLOCK_MONOTONIC_RAW advances at CLOCK_MONOTONIC's rate (ms)",
              ms(elapsed_raw), 0, 0, ms(elapsed_mono), 0, 0);
    }

    // ---- absolute deadlines on the rebased clocks ------------------------
    //
    // Everything below asks for half a second and must take about half a
    // second. With the guest's origin moved but the kernel still reading the
    // host's for these conversions, every one returned instantly.
    {
        struct timespec ts = clock_plus(CLOCK_MONOTONIC, 0.5);
        double s = clock_read(CLOCK_REALTIME);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        check_waited("clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)",
                     clock_read(CLOCK_REALTIME) - s, 0.5);
    }
    {
        struct timespec ts = clock_plus(CLOCK_BOOTTIME, 0.5);
        double s = clock_read(CLOCK_REALTIME);
        clock_nanosleep(CLOCK_BOOTTIME, TIMER_ABSTIME, &ts, NULL);
        check_waited("clock_nanosleep(CLOCK_BOOTTIME, TIMER_ABSTIME)",
                     clock_read(CLOCK_REALTIME) - s, 0.5);
    }
    {
        int fd = timerfd_create(CLOCK_MONOTONIC, 0);
        if (fd < 0) {
            failf("timerfd_create(CLOCK_MONOTONIC)", (uint64_t) errno, 0, 0, 0, 0, 0);
        } else {
            struct itimerspec its;
            memset(&its, 0, sizeof(its));
            its.it_value = clock_plus(CLOCK_MONOTONIC, 0.5);
            double s = clock_read(CLOCK_REALTIME);
            if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &its, NULL) != 0) {
                failf("timerfd_settime(TFD_TIMER_ABSTIME)", (uint64_t) errno, 0, 0, 0, 0, 0);
            } else {
                struct pollfd p = { fd, POLLIN, 0 };
                uint64_t ticks;
                if (poll(&p, 1, 5000) > 0 && read(fd, &ticks, sizeof(ticks)) != sizeof(ticks))
                    test_log_if(1, "  short read from timerfd\n");
                check_waited("timerfd_settime(CLOCK_MONOTONIC, TFD_TIMER_ABSTIME)",
                             clock_read(CLOCK_REALTIME) - s, 0.5);
            }
            close(fd);
        }
    }
    {
        // A POSIX timer whose deadline converts to a negative interval does
        // not fire late -- it does not fire at all, so this one needs the
        // sigtimedwait cap to be well above the deadline to tell the two apart.
        signal(SIGALRM, onsig);
        struct sigevent sev;
        memset(&sev, 0, sizeof(sev));
        sev.sigev_notify = SIGEV_SIGNAL;
        sev.sigev_signo = SIGALRM;
        timer_t tid;
        if (timer_create(CLOCK_MONOTONIC, &sev, &tid) != 0) {
            failf("timer_create(CLOCK_MONOTONIC)", (uint64_t) errno, 0, 0, 0, 0, 0);
        } else {
            sigset_t mask, old;
            sigemptyset(&mask);
            sigaddset(&mask, SIGALRM);
            sigprocmask(SIG_BLOCK, &mask, &old);
            struct itimerspec its;
            memset(&its, 0, sizeof(its));
            its.it_value = clock_plus(CLOCK_MONOTONIC, 0.5);
            double s = clock_read(CLOCK_REALTIME);
            if (timer_settime(tid, TIMER_ABSTIME, &its, NULL) != 0) {
                failf("timer_settime(TIMER_ABSTIME)", (uint64_t) errno, 0, 0, 0, 0, 0);
            } else {
                siginfo_t si;
                struct timespec cap = { 5, 0 };
                if (sigtimedwait(&mask, &si, &cap) < 0)
                    test_log_if(1, "  the timer never fired (%s)\n", strerror(errno));
                check_waited("timer_settime(CLOCK_MONOTONIC, TIMER_ABSTIME)",
                             clock_read(CLOCK_REALTIME) - s, 0.5);
            }
            sigprocmask(SIG_SETMASK, &old, NULL);
            timer_delete(tid);
        }
    }
    {
        // glibc and musl both turn this into FUTEX_WAIT_BITSET with an
        // absolute CLOCK_MONOTONIC deadline, which is a different kernel path
        // from every timer above.
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
            test_log_if(1, "  pthread_condattr_setclock(CLOCK_MONOTONIC) unsupported\n");
        } else {
            pthread_cond_t cond;
            pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
            pthread_cond_init(&cond, &attr);
            struct timespec ts = clock_plus(CLOCK_MONOTONIC, 0.5);
            double s = clock_read(CLOCK_REALTIME);
            pthread_mutex_lock(&mutex);
            int rc = pthread_cond_timedwait(&cond, &mutex, &ts);
            pthread_mutex_unlock(&mutex);
            if (rc != ETIMEDOUT)
                test_log_if(1, "  pthread_cond_timedwait returned %d (%s)\n", rc, strerror(rc));
            check_waited("pthread_cond_timedwait(CLOCK_MONOTONIC)",
                         clock_read(CLOCK_REALTIME) - s, 0.5);
        }
        pthread_condattr_destroy(&attr);
    }

    // ---- controls: CLOCK_REALTIME has no boot origin ---------------------
    {
        pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
        pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
        struct timespec ts = clock_plus(CLOCK_REALTIME, 0.5);
        double s = clock_read(CLOCK_REALTIME);
        pthread_mutex_lock(&mutex);
        pthread_cond_timedwait(&cond, &mutex, &ts);
        pthread_mutex_unlock(&mutex);
        check_waited("pthread_cond_timedwait(CLOCK_REALTIME)",
                     clock_read(CLOCK_REALTIME) - s, 0.5);
    }
    {
        sem_t sem;
        if (sem_init(&sem, 0, 0) == 0) {
            struct timespec ts = clock_plus(CLOCK_REALTIME, 0.5);
            double s = clock_read(CLOCK_REALTIME);
            sem_timedwait(&sem, &ts);
            check_waited("sem_timedwait(CLOCK_REALTIME)", clock_read(CLOCK_REALTIME) - s, 0.5);
            sem_destroy(&sem);
        }
    }

    return finish_suite("clock_boot_origin");
}
