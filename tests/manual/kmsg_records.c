// /dev/kmsg hands back RECORDS, not the log's raw bytes.
//
//   Linux's /dev/kmsg frames every message:
//
//       prio,seq,timestamp_usec,flag;text\n
//
//   util-linux's dmesg parses that field by field -- facility/level, the
//   sequence number, the microsecond timestamp, an optional flag -- and takes
//   everything after the ';' as the message. AOK served the log's own
//   human-readable bytes there instead, "[Thu Sep 17 21:01:27 2026] text", so
//   dmesg found no ';' at all and read EVERY line as an empty message.
//
//   Measured in a Devuan guest (util-linux 2.41) before the fix:
//
//       /bin/dmesg              2 bytes, "\n\n"     <- its DEFAULT path
//       /bin/dmesg --syslog     the whole log       <- the klogctl path
//
//   busybox's dmesg only knows klogctl, so an Alpine guest never saw it, and
//   on Devuan `dmesg` on PATH is /AOK/native/smallclue -- which is why this
//   file runs /bin/dmesg by name.
//
// Measured against x86_64 glibc on Linux 6.12 (camd) as root. Two differences
// from that oracle are deliberate and checked loosely rather than exactly,
// each noted where it is asserted:
//
//   * the priority is a fixed 6 (kern.info). AOK's printk carries no level --
//     a byte-stream log has nowhere to keep one -- so every record gets the
//     one that describes them all. Linux answers 14 for the same write, its
//     facility being LOG_USER.
//   * the timestamp has whole-second resolution. It is recovered from the
//     ctime(3) stamp the log already carries, which is the only per-line time
//     AOK records; Linux stamps each record from a monotonic clock directly.
//
// /proc/kmsg is deliberately NOT framed this way and is checked for it: the
// record header is what a machine needs and what makes `cat /dev/kmsg` less
// readable than it was, so the plain byte stream has to stay somewhere.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

// Linux 3.1 and later; not every libc exposes it without extra feature tests.
#ifndef SEEK_DATA
#define SEEK_DATA 3
#endif

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-56s got=%-12ld want=%ld\n", label, got, want);
}

static void ck_ge(const char *label, long long got, long long floor) {
    if (got < floor)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) floor, 0, 0);
    test_logf("  %-56s got=%-12lld >= %lld\n", label, got, floor);
}

// /dev/kmsg is 0644 and root-owned, as on Linux, so an unprivileged caller
// legitimately cannot write to it. That is the caller's privilege, not the
// behaviour under test -- the suite does not always run as root -- so say so
// and stop rather than reporting a wall of failures.
static int kmsg_open_write(void) {
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0)
        return fd;
    if ((errno == EACCES || errno == EPERM) && geteuid() != 0)
        return -1;
    ck("/dev/kmsg opens for writing", 0, 1);
    return -1;
}

// ---- the record, taken apart --------------------------------------------

struct kmsg_rec {
    unsigned long long prio, seq, usec;
    const char *flag;      // the fourth field, however long it is
    size_t flag_len;
    const char *text;
    size_t text_len;       // up to the newline that ends the record
};

// Strictly: three decimal fields separated by commas, then a fourth field that
// runs to the next ',' or ';' (Linux may append further key=value fields
// there, which util-linux skips), then ';', then the text.
static bool kmsg_parse(const char *buf, size_t n, struct kmsg_rec *r) {
    const char *p = buf, *end = buf + n;
    unsigned long long *fields[] = { &r->prio, &r->seq, &r->usec };
    for (int i = 0; i < 3; i++) {
        unsigned long long v = 0;
        const char *start = p;
        while (p < end && *p >= '0' && *p <= '9')
            v = v * 10 + (unsigned) (*p++ - '0');
        if (p == start || p == end || *p != ',')
            return false;
        *fields[i] = v;
        p++;
    }
    r->flag = p;
    while (p < end && *p != ',' && *p != ';')
        p++;
    if (p == end)
        return false;
    r->flag_len = (size_t) (p - r->flag);
    while (p < end && *p != ';')
        p++;
    if (p == end)
        return false;
    p++;
    r->text = p;
    const char *nl = memchr(p, '\n', (size_t) (end - p));
    if (nl == NULL)
        return false;
    r->text_len = (size_t) (nl - p);
    return true;
}

// A fresh fd per line, deliberately. Linux gives every OPEN of /dev/kmsg its
// own ratelimit bucket -- ten messages per five seconds (devkmsg_open,
// ratelimit_default_init) -- and silently drops the rest while reporting the
// write as consumed. Measured: 30 writes down one fd stored 10 records, 30
// writes down 30 fds stored all 30. Reusing one fd made this file's own
// markers vanish on the oracle and looked like four unrelated kernel bugs.
// AOK has no such limit, so this only makes the two behave alike.
static int kmsg_say(const char *text) {
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd < 0)
        return -1;
    char line[512];
    int n = snprintf(line, sizeof line, "<6>%s\n", text);
    ssize_t w = write(fd, line, (size_t) n);
    close(fd);
    return w == n ? 0 : -1;
}

// Read past everything already logged, so what comes next is ours alone.
// Through EPIPE, which is not "caught up": in a full log each new line pushes
// the oldest out, and a reader still there is told so once and moved on.
// Bounded by time rather than a count, since a full 1 MiB log is tens of
// thousands of records.
static void kmsg_drain(int rd) {
    char buf[8192];
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        errno = 0;
        ssize_t n = read(rd, buf, sizeof buf);
        if (n <= 0 && !(n < 0 && errno == EPIPE))
            return;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (t.tv_sec - t0.tv_sec > 20)
            return;
    }
}

// Write `text` and return the record carrying it. Other threads can log in
// between, so skip anything that is not ours rather than assuming the very
// next record is.
// `match` is what to look for in the record's text, which is not always the
// string that was written: the text comes back escaped.
static bool kmsg_say_and_match(int rd, const char *text, const char *match,
                               char *buf, size_t bufsize, struct kmsg_rec *r) {
    if (kmsg_say(text) != 0)
        return false;
    size_t mlen = strlen(match);
    for (int i = 0; i < 200; i++) {
        errno = 0;
        ssize_t n = read(rd, buf, bufsize);
        if (n < 0 && errno == EPIPE)
            continue;  // lapped by other logging; see kmsg_drain
        if (n <= 0)
            return false;
        if (!kmsg_parse(buf, (size_t) n, r))
            continue;
        if (r->text_len >= mlen &&
            memmem(r->text, r->text_len, match, mlen) != NULL)
            return true;
    }
    return false;
}

static bool kmsg_say_and_read(int rd, const char *text,
                              char *buf, size_t bufsize, struct kmsg_rec *r) {
    return kmsg_say_and_match(rd, text, text, buf, bufsize, r);
}

// ---- the end-to-end claim ------------------------------------------------

// The bug as a user meets it: `dmesg`, with no arguments, in a Devuan guest.
// Runs /bin/dmesg by name because on Devuan the `dmesg` on PATH is
// /usr/local/native-bin/dmesg -> /AOK/native/smallclue, which is a different
// program and was never broken.
static const char *find_dmesg(void) {
    static const char *candidates[] = { "/bin/dmesg", "/usr/bin/dmesg", NULL };
    for (int i = 0; candidates[i] != NULL; i++)
        if (access(candidates[i], X_OK) == 0)
            return candidates[i];
    return NULL;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(180));

    int wr = kmsg_open_write();
    if (wr < 0) {
        printf("kmsg_records: SKIP (unprivileged: /dev/kmsg is 0644 root-owned)\n");
        return 0;
    }
    int rd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (rd < 0) {
        ck("/dev/kmsg opens for reading", 0, 1);
        return finish_suite("kmsg_records");
    }

    char buf[16384];
    struct kmsg_rec rec, rec2;

    // ---- a record is a record -------------------------------------------
    {
        kmsg_drain(rd);
        bool got = kmsg_say_and_read(rd, "kmsg_records MARKER-ONE",
                                     buf, sizeof buf, &rec);
        ck("a written line comes back as a parseable record", got ? 1 : 0, 1);
        if (got) {
            // The priority is facility*8+level, so it cannot exceed 191. The
            // exact value is the deliberate difference: AOK says 6 (kern.info)
            // for everything, Linux says 14 (user.info) for this same write.
            ck("  the priority field is a valid facility/level",
               rec.prio <= 191 ? 1 : 0, 1);
            ck("  the flag field is one character", (long) rec.flag_len, 1);
            ck("  and it is '-', a normal record",
               rec.flag_len == 1 && rec.flag[0] == '-' ? 1 : 0, 1);
            // Everything after the ';' is the message, with no ctime stamp
            // left in it -- that time is what the timestamp field carries now,
            // and dmesg prints its own rendering of it in front of the text.
            ck("  the text is the message, and only the message",
               rec.text_len == strlen("kmsg_records MARKER-ONE") ? 1 : 0, 1);
            ck("  with no leading '[' from the log's own stamp",
               rec.text_len > 0 && rec.text[0] == '[' ? 0 : 1, 1);
            test_logf("  record: %.*s\n", (int) strlen("kmsg_records MARKER-ONE") + 40, buf);
        }
    }

    // ---- exactly one record per read -------------------------------------
    // Not "as many bytes as fit". A reader whose buffer straddled two messages
    // would have to re-frame them itself, and none of them do.
    {
        bool got = kmsg_say_and_read(rd, "kmsg_records MARKER-TWO",
                                     buf, sizeof buf, &rec);
        if (got) {
            ssize_t n = (ssize_t) (rec.text - buf) + (ssize_t) rec.text_len + 1;
            ck("the record ends at its newline", buf[n - 1] == '\n' ? 1 : 0, 1);
            ck("  and carries no second line",
               memchr(buf, '\n', (size_t) n - 1) == NULL ? 1 : 0, 1);
        } else {
            ck("one record per read", 0, 1);
        }
    }

    // ---- the sequence number counts, and every reader agrees -------------
    {
        bool a = kmsg_say_and_read(rd, "kmsg_records SEQ-A", buf, sizeof buf, &rec);
        char buf2[16384];
        bool b = kmsg_say_and_read(rd, "kmsg_records SEQ-B", buf2, sizeof buf2, &rec2);
        ck("two consecutive records read back", a && b ? 1 : 0, 1);
        if (a && b)
            ck("  the sequence number counts up by one",
               (long) (rec2.seq - rec.seq), 1);

        // A second, independent reader must number the same message the same
        // way. The sequence number is recovered from a byte position here, so
        // two readers agreeing is the whole point of recovering it rather than
        // counting per-fd.
        int rd2 = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
        if (rd2 >= 0) {
            unsigned long long found = 0;
            bool hit = false;
            for (int i = 0; i < 200000; i++) {
                ssize_t n = read(rd2, buf2, sizeof buf2);
                if (n <= 0)
                    break;
                struct kmsg_rec r;
                if (kmsg_parse(buf2, (size_t) n, &r) && r.text_len >= 18 &&
                    memmem(r.text, r.text_len, "kmsg_records SEQ-B", 18) != NULL) {
                    found = r.seq;
                    hit = true;
                    break;
                }
            }
            ck("  a second reader finds the same record", hit ? 1 : 0, 1);
            if (hit && b)
                ck("  and gives it the same sequence number",
                   found == rec2.seq ? 1 : 0, 1);
            close(rd2);
        }
    }

    // ---- the timestamp is a real time ------------------------------------
    // Whole seconds here against Linux's microseconds -- the deliberate
    // difference above -- so this asserts that it MOVES and in which
    // direction, not how finely.
    {
        bool a = kmsg_say_and_read(rd, "kmsg_records TIME-A", buf, sizeof buf, &rec);
        sleep(2);
        char buf2[16384];
        bool b = kmsg_say_and_read(rd, "kmsg_records TIME-B", buf2, sizeof buf2, &rec2);
        ck("two records two seconds apart", a && b ? 1 : 0, 1);
        if (a && b) {
            ck_ge("  the timestamp advanced by at least a second",
                  (long long) (rec2.usec - rec.usec), 1000000);
            // And it is a time since boot, not a wall clock: a wall clock in
            // microseconds would be around 1.8e15 by now.
            ck("  and is measured from boot, not from the epoch",
               rec2.usec < 1000000000000000ULL ? 1 : 0, 1);
        }
    }

    // ---- the text is escaped so it cannot forge the framing ---------------
    // A guest can put any byte in a message. An unescaped one could be read as
    // the newline that ends the record or the space that starts a continuation
    // line, which is why Linux escapes control bytes, the high half, and the
    // backslash itself as \xNN. The comma and semicolon do NOT need it: they
    // are unambiguous once the header is past.
    {
        if (kmsg_say_and_match(rd, "kmsg_records ESC-A\tB\\C\x01" "D , ; =",
                               "kmsg_records ESC-A", buf, sizeof buf, &rec)) {
            char text[512];
            size_t n = rec.text_len < sizeof text - 1 ? rec.text_len : sizeof text - 1;
            memcpy(text, rec.text, n);
            text[n] = '\0';
            test_logf("  escaped text: [%s]\n", text);
            ck("a tab is escaped", strstr(text, "\\x09") != NULL ? 1 : 0, 1);
            ck("a backslash is escaped", strstr(text, "\\x5c") != NULL ? 1 : 0, 1);
            ck("a control byte is escaped", strstr(text, "\\x01") != NULL ? 1 : 0, 1);
            ck("no raw control byte survives",
               strpbrk(text, "\t\x01") == NULL ? 1 : 0, 1);
            ck("a comma passes through", strchr(text, ',') != NULL ? 1 : 0, 1);
            ck("a semicolon passes through", strchr(text, ';') != NULL ? 1 : 0, 1);
        } else {
            ck("the escaping check ran", 0, 1);
        }
    }

    // ---- a buffer too small for a record is a bad argument ----------------
    // Not a short read and not an empty one: the reader would have no way to
    // tell a truncated record from a whole one.
    //
    // And the record is CONSUMED. Linux advances the reader past it before it
    // checks the size (devkmsg_read), so the message the caller could not
    // receive is gone -- measured, because it reads like a wart and the
    // friendlier choice was tempting: keeping the record would hand the same
    // EINVAL back to any reader that retried, forever.
    {
        kmsg_drain(rd);
        ck("wrote the first line", kmsg_say("kmsg_records EINVAL-A"), 0);
        ck("wrote the second", kmsg_say("kmsg_records EINVAL-B"), 0);

        char tiny[4];
        errno = 0;
        ssize_t n = read(rd, tiny, sizeof tiny);
        ck("a 4-byte buffer is EINVAL", n < 0 ? -errno : n, -EINVAL);

        // Whichever of the two comes back first says what that EINVAL did.
        char which = '?';
        for (int i = 0; i < 200; i++) {
            n = read(rd, buf, sizeof buf);
            if (n <= 0)
                break;
            struct kmsg_rec r;
            if (!kmsg_parse(buf, (size_t) n, &r))
                continue;
            const char *hit = memmem(r.text, r.text_len, "kmsg_records EINVAL-", 20);
            if (hit != NULL) {
                which = hit[20];
                break;
            }
        }
        ck("  and it consumed the record it could not deliver",
           which == 'B' ? 1 : 0, 1);
        test_logf("  %-56s first line back was EINVAL-%c\n", "", which);

        // A zero-length buffer can never hold a record, so it is the same
        // answer -- not the 0 that POSIX gives a zero-length read of a plain
        // file, and not the 0 /proc/kmsg gives.
        ck("wrote another line", kmsg_say("kmsg_records EINVAL-C"), 0);
        errno = 0;
        n = read(rd, tiny, 1);
        ck("a 1-byte buffer is EINVAL", n < 0 ? -errno : n, -EINVAL);
        ck("wrote one more", kmsg_say("kmsg_records EINVAL-D"), 0);
        errno = 0;
        n = read(rd, tiny, 0);
        ck("a zero-length read is EINVAL", n < 0 ? -errno : n, -EINVAL);
    }

    // ---- nothing past the record is touched -------------------------------
    {
        memset(buf, 0x5a, sizeof buf);
        if (kmsg_say("kmsg_records UNTOUCHED") == 0) {
            ssize_t n = read(rd, buf, sizeof buf);
            if (n > 0) {
                ck("the byte after the record is untouched",
                   (unsigned char) buf[n] == 0x5a ? 1 : 0, 1);
                ck("  so the length returned covers no terminator",
                   buf[n - 1] == '\n' ? 1 : 0, 1);
            }
        }
    }

    // ---- a caught-up reader is told to wait -------------------------------
    {
        kmsg_drain(rd);
        errno = 0;
        ssize_t n = read(rd, buf, sizeof buf);
        ck("a drained non-blocking read is EAGAIN", n < 0 ? -errno : n, -EAGAIN);
    }

    // ---- lseek names records, not bytes -----------------------------------
    // /dev/kmsg's seek takes no offset at all: each whence names a fixed point
    // in the log and a successful seek answers 0. util-linux's dmesg opens the
    // node and immediately seeks SEEK_DATA, so this is on the path of every
    // plain `dmesg` in a guest.
    {
        ck("SEEK_SET 0 succeeds and returns 0", (long) lseek(rd, 0, SEEK_SET), 0);
        ck("SEEK_END 0 succeeds and returns 0", (long) lseek(rd, 0, SEEK_END), 0);
        ck("SEEK_DATA 0 succeeds and returns 0", (long) lseek(rd, 0, SEEK_DATA), 0);

        errno = 0;
        ck("SEEK_SET with an offset is ESPIPE",
           lseek(rd, 100, SEEK_SET) < 0 ? -errno : 0, -ESPIPE);
        errno = 0;
        ck("SEEK_SET with a negative offset is ESPIPE",
           lseek(rd, -1, SEEK_SET) < 0 ? -errno : 0, -ESPIPE);
        errno = 0;
        ck("SEEK_CUR with an offset is ESPIPE",
           lseek(rd, 5, SEEK_CUR) < 0 ? -errno : 0, -ESPIPE);
        errno = 0;
        ck("SEEK_DATA with an offset is ESPIPE",
           lseek(rd, 42, SEEK_DATA) < 0 ? -errno : 0, -ESPIPE);
        errno = 0;
        ck("SEEK_CUR at 0 is EINVAL -- there is no 'here'",
           lseek(rd, 0, SEEK_CUR) < 0 ? -errno : 0, -EINVAL);
        errno = 0;
        ck("an unknown whence is EINVAL",
           lseek(rd, 0, 99) < 0 ? -errno : 0, -EINVAL);

        // SEEK_END parks the reader past everything, so only what is logged
        // afterwards comes back.
        ck("SEEK_END again", (long) lseek(rd, 0, SEEK_END), 0);
        errno = 0;
        ssize_t n = read(rd, buf, sizeof buf);
        ck("  a read after SEEK_END is EAGAIN", n < 0 ? -errno : n, -EAGAIN);
        if (kmsg_say_and_read(rd, "kmsg_records AFTER-END", buf, sizeof buf, &rec))
            ck("  and the next line logged arrives", 1, 1);
        else
            ck("  and the next line logged arrives", 0, 1);

        // SEEK_SET goes back to the oldest record still buffered, which is
        // before that one.
        ck("SEEK_SET back to the start", (long) lseek(rd, 0, SEEK_SET), 0);
        n = read(rd, buf, sizeof buf);
        struct kmsg_rec first;
        ck("  a read there gives a record", n > 0 && kmsg_parse(buf, (size_t) n, &first) ? 1 : 0, 1);
        if (n > 0 && kmsg_parse(buf, (size_t) n, &first))
            ck("  older than the one just read",
               first.seq < rec.seq ? 1 : 0, 1);
    }

    // ---- pread ignores the offset -----------------------------------------
    // Linux's /dev/kmsg never looks at the offset: a pread reads the next
    // record and advances the reader exactly as a plain read does. It matters
    // because the alternative -- letting a generic pread emulate itself with a
    // pair of lseeks -- cannot work on a file whose lseek takes no offsets.
    {
        kmsg_drain(rd);
        ck("wrote a line to pread", kmsg_say("kmsg_records PREAD-ONE"), 0);
        ck("wrote a second", kmsg_say("kmsg_records PREAD-TWO"), 0);
        errno = 0;
        ssize_t n = pread(rd, buf, sizeof buf, 12345);
        ck("a pread at a nonsense offset still reads", n > 0 ? 1 : -errno, 1);
        if (n > 0) {
            struct kmsg_rec r;
            ck("  and gives a whole record", kmsg_parse(buf, (size_t) n, &r) ? 1 : 0, 1);
            // ...the FIRST one, not something at byte 12345.
            n = read(rd, buf, sizeof buf);
            struct kmsg_rec r2;
            bool ok = n > 0 && kmsg_parse(buf, (size_t) n, &r2);
            ck("  a read after it gives the next record", ok ? 1 : 0, 1);
            if (ok)
                ck("    one sequence number on", (long) (r2.seq - r.seq), 1);
        }
    }

    // ---- one write is one record, and a record has a ceiling --------------
    // Linux caps it at PRINTKRB_RECORD_MAX and rejects anything longer rather
    // than storing it truncated -- losing the tail of a message without
    // telling anyone is worse than refusing it.
    {
        char big[2048];
        memset(big, 'B', sizeof big);
        memcpy(big, "<6>kmsg_records BIG", 19);

        big[1023] = '\n';
        errno = 0;
        ssize_t n = write(wr, big, 1024);
        ck("a 1024-byte write is accepted", n < 0 ? -errno : n, 1024);

        big[1024] = '\n';
        errno = 0;
        n = write(wr, big, 1025);
        ck("a 1025-byte write is EINVAL", n < 0 ? -errno : n, -EINVAL);

        errno = 0;
        n = write(wr, "", 0);
        ck("an empty write is accepted and writes nothing", n < 0 ? -errno : n, 0);
    }

    // ---- /proc/kmsg is deliberately NOT framed this way -------------------
    // The record header is what makes `cat /dev/kmsg` less readable than it
    // used to be. That is the price of a parseable device, and it is only
    // acceptable because the plain byte stream is still served somewhere:
    // /proc/kmsg, and `dmesg --syslog` through syslog(2), carry the lines AOK
    // has always printed, stamp and all.
    {
        int pk = open("/proc/kmsg", O_RDONLY | O_NONBLOCK);
        if (pk < 0) {
            test_logf("  %-56s SKIP (%s)\n", "/proc/kmsg is not record-framed",
                      strerror(errno));
        } else {
            // Caught up first, then the marker, so the line is the next thing
            // there rather than somewhere past the end of a count. A fresh
            // reader starts at the oldest line buffered and gets one line per
            // read, and a device's full 1 MiB log is more lines than the
            // 20000 reads this once allowed: the marker was never reached, on
            // every device leg of 555 and 556. Bounded by time, not by reads.
            struct timespec t0, t;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (;;) {
                if (read(pk, buf, sizeof buf - 1) <= 0)
                    break;
                clock_gettime(CLOCK_MONOTONIC, &t);
                if (t.tv_sec - t0.tv_sec > 20)
                    break;
            }
            if (kmsg_say("kmsg_records PROC-PLAIN") == 0) {
                bool seen = false, framed = false;
                for (int i = 0; i < 20000; i++) {
                    ssize_t n = read(pk, buf, sizeof buf - 1);
                    if (n <= 0)
                        break;
                    buf[n] = '\0';
                    if (strstr(buf, "kmsg_records PROC-PLAIN") == NULL)
                        continue;
                    seen = true;
                    struct kmsg_rec r;
                    framed = kmsg_parse(buf, (size_t) n, &r);
                    break;
                }
                ck("/proc/kmsg still carries the line", seen ? 1 : 0, 1);
                ck("  and does NOT frame it as a kmsg record", framed ? 1 : 0, 0);
            }
            close(pk);
        }
    }

    // ---- what the user sees: `/bin/dmesg`, with no arguments --------------
    // The bug's signature was output that was ALL NEWLINES: one empty message
    // per line, correctly counted and entirely blank. So it is not enough for
    // the marker to be missing-or-present; the output has to have content.
    {
        const char *dmesg = find_dmesg();
        if (dmesg == NULL) {
            test_logf("  %-56s SKIP (no /bin/dmesg)\n", "dmesg prints the log");
        } else {
            ck("wrote the line dmesg should print",
               kmsg_say("kmsg_records DMESG-SHOULD-PRINT-THIS"), 0);
            char cmd[256];
            snprintf(cmd, sizeof cmd, "%s 2>/dev/null", dmesg);
            FILE *p = popen(cmd, "r");
            if (p == NULL) {
                test_logf("  %-56s SKIP (no popen)\n", "dmesg prints the log");
            } else {
                static char out[1 << 20];
                size_t got = fread(out, 1, sizeof out - 1, p);
                out[got] = '\0';
                int status = pclose(p);
                size_t printable = 0;
                for (size_t i = 0; i < got; i++)
                    if (out[i] != '\n' && out[i] != ' ')
                        printable++;
                test_logf("  %-56s %s: %zu bytes, %zu printable\n",
                          "dmesg output", dmesg, got, printable);
                if (status != 0 && got == 0) {
                    printf("kmsg_records: note: no usable dmesg (status %d), "
                           "skipping the end-to-end check\n", status);
                } else {
                    ck("dmesg printed something", got > 0 ? 1 : 0, 1);
                    // The bug exactly: bytes, all of them newlines.
                    ck("  that is not just blank lines", printable > 0 ? 1 : 0, 1);
                    ck("  and it contains the line just logged",
                       strstr(out, "kmsg_records DMESG-SHOULD-PRINT-THIS") != NULL ? 1 : 0, 1);
                }
            }
        }
    }

    // ---- a reader left behind is told, once -------------------------------
    // Last, because it fills the log and evicts everything above. A reader
    // whose position has fallen off the back gets EPIPE and is moved to the
    // oldest record still there; the read after that one succeeds. util-linux
    // retries on EPIPE and on nothing else, so getting this wrong would end
    // dmesg's output at the overrun instead of resuming past it.
    {
        int buffer_size = (int) syscall(SYS_syslog, 10, NULL, 0);
        if (buffer_size <= 0) {
            test_logf("  %-56s SKIP (no buffer size)\n", "an overrun is EPIPE");
        } else {
            int old = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
            if (old >= 0) {
                ck("a reader parked at the oldest record",
                   (long) lseek(old, 0, SEEK_SET), 0);
                char line[1000];
                memset(line, 'F', sizeof line);
                memcpy(line, "<7>kmsg_records flood", 21);
                line[sizeof line - 1] = '\n';
                // Comfortably more than the whole buffer, so the parked
                // reader's position is certainly gone.
                int writes = buffer_size / (int) sizeof line + 128;
                for (int i = 0; i < writes; i++) {
                    // A fresh fd per line, for the ratelimit reason above:
                    // down one fd the oracle stored ten of these and the
                    // parked reader was never left behind at all.
                    int w = open("/dev/kmsg", O_WRONLY);
                    if (w < 0)
                        break;
                    ssize_t wn = write(w, line, sizeof line);
                    close(w);
                    if (wn < 0)
                        break;
                }
                errno = 0;
                ssize_t n = read(old, buf, sizeof buf);
                ck("a read from a position that is gone is EPIPE",
                   n < 0 ? -errno : n, -EPIPE);
                errno = 0;
                n = read(old, buf, sizeof buf);
                ck("  and the very next read succeeds", n > 0 ? 1 : 0, 1);
                if (n > 0)
                    ck("  with a whole record", kmsg_parse(buf, (size_t) n, &rec) ? 1 : 0, 1);
                close(old);
            }
        }
    }

    close(rd);
    close(wr);
    return finish_suite("kmsg_records");
}
