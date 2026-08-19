/*
 * proc_field_layout -- procfs files whose COLUMN POSITIONS carry the meaning.
 *
 * /proc/net/dev, /proc/<pid>/stat and /proc/<pid>/status are all positional
 * formats: a consumer reads field N, not a label. A kernel that emits the
 * right number of fields with the wrong values in them looks perfectly well
 * formed and is completely wrong, so a structural check alone cannot catch it.
 *
 * The bug this test was written for: proc_show_dev() in fs/proc/net.c passed
 * EIGHTEEN arguments to a format string with SIXTEEN conversions. Linux folds
 * four of its own counters into the single "frame" column and four more into
 * "carrier"; AOK's port had given each half of those sums an argument of its
 * own. printf silently dropped the last two, so every transmit column sat one
 * place to the left of where the header said it did -- multicast appeared as
 * tx_bytes, rx_bytes as tx_packets, and so on down the line. btop showed an
 * empty network panel and ifconfig reported a loopback that had received
 * 259 MiB and transmitted 33 kB.
 *
 * Note what does NOT catch that: counting fields. The line still had sixteen
 * of them. The check has to be semantic -- push a known number of bytes
 * through loopback and require the column the header calls tx_bytes to move
 * by at least that much. Under the shifted format that column held the
 * multicast count, which loopback traffic does not touch.
 *
 * Also covers /proc/<pid>/status's signal masks, which were printed as eight
 * hex digits from a 64-bit sigset_t_ (Linux's render_sigset_t always emits
 * sixteen), and /proc/<pid>/stat's field count.
 *
 * Passes on real Linux: every invariant here is one Linux itself guarantees.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define PAYLOAD_BYTES (1024 * 1024)

// The two column groups /proc/net/dev's header promises, per Linux's
// dev_seq_printf_stats(): 8 receive fields then 8 transmit ones.
#define RX_COLUMNS 8
#define TX_COLUMNS 8

// Zero-based indices into the 16 data fields.
#define F_RX_BYTES   0
#define F_RX_PACKETS 1
#define F_TX_BYTES   8
#define F_TX_PACKETS 9

struct dev_line {
    int found;
    unsigned long long f[RX_COLUMNS + TX_COLUMNS];
};

static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    failures_total++;
}

// True for the loopback device under either spelling: "lo" on Linux, "lo0"
// on Darwin, whose names AOK reports verbatim from getifaddrs().
static int is_loopback_name(const char *name) {
    if (strncmp(name, "lo", 2) != 0)
        return 0;
    for (const char *p = name + 2; *p != '\0'; p++)
        if (!isdigit((unsigned char) *p))
            return 0;
    return 1;
}

// Reads /proc/net/dev. Fills `lo` with the loopback line if present, and
// checks every data line's structure on the way past. Returns -1 if the file
// could not be read at all.
static int read_net_dev(struct dev_line *lo, int check_structure) {
    FILE *f = fopen("/proc/net/dev", "r");
    if (f == NULL) {
        fail("/proc/net/dev: fopen: %s", strerror(errno));
        return -1;
    }

    lo->found = 0;
    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        lineno++;
        if (lineno == 1)
            continue; // "Inter-|   Receive ... |  Transmit"

        if (lineno == 2) {
            if (!check_structure)
                continue;
            // " face |<8 receive names>|<8 transmit names>"
            char *first = strchr(line, '|');
            char *second = first != NULL ? strchr(first + 1, '|') : NULL;
            if (first == NULL || second == NULL) {
                fail("/proc/net/dev header has no two '|' separators: %s", line);
                continue;
            }
            *second = '\0';
            int rx = 0, tx = 0;
            for (char *t = strtok(first + 1, " \t\n"); t != NULL; t = strtok(NULL, " \t\n"))
                rx++;
            for (char *t = strtok(second + 1, " \t\n"); t != NULL; t = strtok(NULL, " \t\n"))
                tx++;
            if (rx != RX_COLUMNS || tx != TX_COLUMNS)
                fail("/proc/net/dev header names %d receive and %d transmit columns, want %d and %d",
                     rx, tx, RX_COLUMNS, TX_COLUMNS);
            continue;
        }

        char *colon = strchr(line, ':');
        if (colon == NULL)
            continue;

        // Linux writes "%6s: %7llu ...". The literal space matters: without it
        // an 8-digit rx_bytes glues itself to the interface name and
        // busybox/net-tools ifconfig parses the name as garbage, then fails
        // its SIOCGIFFLAGS lookup with "Device not found".
        if (check_structure && colon[1] != ' ')
            fail("/proc/net/dev: no space after the name colon, ifconfig will misparse: %s", line);

        *colon = '\0';
        char *name = line;
        while (*name == ' ' || *name == '\t')
            name++;

        unsigned long long v[RX_COLUMNS + TX_COLUMNS];
        int n = 0;
        for (char *t = strtok(colon + 1, " \t\n"); t != NULL; t = strtok(NULL, " \t\n")) {
            if (n < RX_COLUMNS + TX_COLUMNS)
                v[n] = strtoull(t, NULL, 10);
            n++;
        }
        if (check_structure && n != RX_COLUMNS + TX_COLUMNS)
            fail("/proc/net/dev: %s has %d fields, want %d", name, n, RX_COLUMNS + TX_COLUMNS);

        if (is_loopback_name(name) && n == RX_COLUMNS + TX_COLUMNS) {
            lo->found = 1;
            memcpy(lo->f, v, sizeof(v));
            test_logf("  lo line: %s rx_bytes=%llu tx_bytes=%llu\n",
                      name, v[F_RX_BYTES], v[F_TX_BYTES]);
        }
    }
    fclose(f);
    return 0;
}

// The counters come from Darwin's struct if_data, whose byte fields are 32
// bits wide, so a delta has to be taken modulo 2^32. Linux's are 64-bit, and
// a true delta below 2^32 survives the same arithmetic unchanged.
static unsigned long long delta32(unsigned long long before, unsigned long long after) {
    return (unsigned long long) (uint32_t) (after - before);
}

// Pushes PAYLOAD_BYTES through a TCP connection to 127.0.0.1, so both the
// receive and the transmit side of loopback must move by at least that much.
static int push_loopback_traffic(void) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        fail("socket(listener): %s", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // any
    if (bind(listener, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        fail("bind(127.0.0.1): %s", strerror(errno));
        close(listener);
        return -1;
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(listener, (struct sockaddr *) &addr, &alen) < 0) {
        fail("getsockname: %s", strerror(errno));
        close(listener);
        return -1;
    }
    if (listen(listener, 1) < 0) {
        fail("listen: %s", strerror(errno));
        close(listener);
        return -1;
    }

    pid_t child = fork();
    if (child < 0) {
        fail("fork: %s", strerror(errno));
        close(listener);
        return -1;
    }
    if (child == 0) {
        // Drain everything the parent sends, then exit.
        int conn = accept(listener, NULL, NULL);
        if (conn < 0)
            _exit(1);
        close(listener);
        static char sink[65536];
        size_t got = 0;
        while (got < (size_t) PAYLOAD_BYTES) {
            ssize_t r = read(conn, sink, sizeof(sink));
            if (r <= 0)
                break;
            got += (size_t) r;
        }
        close(conn);
        _exit(got >= (size_t) PAYLOAD_BYTES ? 0 : 1);
    }

    int sender = socket(AF_INET, SOCK_STREAM, 0);
    int rc = -1;
    if (sender < 0) {
        fail("socket(sender): %s", strerror(errno));
        goto out;
    }
    if (connect(sender, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        fail("connect(127.0.0.1): %s", strerror(errno));
        goto out;
    }
    {
        static char payload[65536];
        memset(payload, 'x', sizeof(payload));
        size_t sent = 0;
        while (sent < (size_t) PAYLOAD_BYTES) {
            size_t want = sizeof(payload);
            if (want > (size_t) PAYLOAD_BYTES - sent)
                want = (size_t) PAYLOAD_BYTES - sent;
            ssize_t w = write(sender, payload, want);
            if (w <= 0) {
                fail("write to loopback after %zu bytes: %s", sent, strerror(errno));
                goto out;
            }
            sent += (size_t) w;
        }
        rc = 0;
    }

out:
    if (sender >= 0)
        close(sender);
    close(listener);
    int status = 0;
    waitpid(child, &status, 0);
    if (rc == 0 && !(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        fail("loopback receiver did not drain %d bytes (status %d)", PAYLOAD_BYTES, status);
        rc = -1;
    }
    return rc;
}

static void check_net_dev(void) {
    struct dev_line before, after;

    if (read_net_dev(&before, 1) < 0)
        return;
    if (!before.found) {
        printf("SKIP /proc/net/dev has no loopback line; structure checked only\n");
        return;
    }
    if (push_loopback_traffic() < 0)
        return;
    if (read_net_dev(&after, 0) < 0)
        return;
    if (!after.found) {
        fail("/proc/net/dev lost its loopback line between reads");
        return;
    }

    // The discriminating check. Every one of these four columns tracks
    // loopback traffic on a correct kernel; under the shifted format,
    // tx_bytes held the multicast count and did not move at all.
    static const struct { int idx; const char *name; unsigned long long least; } want[] = {
        {F_RX_BYTES,   "rx_bytes",   PAYLOAD_BYTES},
        {F_TX_BYTES,   "tx_bytes",   PAYLOAD_BYTES},
        {F_RX_PACKETS, "rx_packets", 1},
        {F_TX_PACKETS, "tx_packets", 1},
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        unsigned long long d = delta32(before.f[want[i].idx], after.f[want[i].idx]);
        test_logf("  lo %s moved by %llu (want >= %llu)\n", want[i].name, d, want[i].least);
        if (d < want[i].least)
            fail("/proc/net/dev loopback %s moved by %llu after %d bytes of loopback traffic, want >= %llu"
                 " (column %d is misaligned if this is a stale counter)",
                 want[i].name, d, PAYLOAD_BYTES, want[i].least, want[i].idx);
    }
}

// Linux's render_sigset_t() prints the whole 64-bit sigset: 16 hex digits,
// on every architecture. A narrower field tells a consumer the mask is
// narrower than it is.
static void check_status_sigsets(void) {
    static const char *fields[] = {"SigPnd:", "ShdPnd:", "SigBlk:", "SigIgn:", "SigCgt:"};
    int seen[sizeof(fields) / sizeof(fields[0])];
    memset(seen, 0, sizeof(seen));

    FILE *f = fopen("/proc/self/status", "r");
    if (f == NULL) {
        fail("/proc/self/status: fopen: %s", strerror(errno));
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
            size_t klen = strlen(fields[i]);
            if (strncmp(line, fields[i], klen) != 0)
                continue;
            seen[i] = 1;
            const char *v = line + klen;
            while (*v == ' ' || *v == '\t')
                v++;
            int digits = 0;
            while (isxdigit((unsigned char) v[digits]))
                digits++;
            test_logf("  %s %d hex digits\n", fields[i], digits);
            if (digits != 16)
                fail("/proc/self/status %s is %d hex digits, want 16 (a 64-bit sigset)",
                     fields[i], digits);
            if (v[digits] != '\n' && v[digits] != '\0')
                fail("/proc/self/status %s has trailing junk: %s", fields[i], line);
        }
    }
    fclose(f);
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        if (!seen[i])
            fail("/proc/self/status is missing %s", fields[i]);
}

// 52 fields since Linux 3.5. htop and friends index from the end, so a short
// record silently gives them somebody else's column.
static void check_stat_field_count(void) {
    FILE *f = fopen("/proc/self/stat", "r");
    if (f == NULL) {
        fail("/proc/self/stat: fopen: %s", strerror(errno));
        return;
    }
    char line[4096];
    if (fgets(line, sizeof(line), f) == NULL) {
        fail("/proc/self/stat: empty");
        fclose(f);
        return;
    }
    fclose(f);

    // comm (field 2) is parenthesized and may contain anything, so count from
    // the LAST ')' -- which is what Linux's own consumers do.
    char *rparen = strrchr(line, ')');
    if (rparen == NULL) {
        fail("/proc/self/stat has no ')' ending comm: %s", line);
        return;
    }
    int n = 2; // pid and comm
    for (char *t = strtok(rparen + 1, " \t\n"); t != NULL; t = strtok(NULL, " \t\n"))
        n++;
    test_logf("  /proc/self/stat has %d fields\n", n);
    if (n < 52)
        fail("/proc/self/stat has %d fields, want at least 52", n);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    check_net_dev();
    check_status_sigsets();
    check_stat_field_count();

    return finish_suite("proc_field_layout");
}
