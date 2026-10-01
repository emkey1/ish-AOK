// Local time for native programs, in the GUEST's time zone.
//
// localtime, mktime and friends used to go straight to the host libc, and the
// host's tzset reads the app process's zone -- never the guest's TZ or
// /etc/localtime. A guest in UTC on a Mac in BST saw `ls -l` from SmallCLUE an
// hour off from /usr/bin/ls; every native program that printed a local time
// (date, ls, find -printf, tar tv) carried the host's offset.
//
// Setting the host's TZ is not an answer: it is process-wide, shared with the
// app's UI and with every other native program on its own thread. So the zone
// is resolved and converted here, from the guest's own files: TZ (a zoneinfo
// name, a path, or a POSIX rule string such as "EST5EDT,M3.2.0,M11.1.0"), else
// /etc/localtime, else UTC -- glibc's order. Zoneinfo is TZif (RFC 8536);
// version 2+ data and its POSIX footer, which covers times past the last
// transition, are used.
//
// This file is compiled WITHOUT native_libc.h's force-include: the names it
// calls (gmtime_r, timegm) are the host's own, which are zone-free.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kernel/fs.h"
#include "kernel/native_io.h"
#include "kernel/native_libc.h"
#include "fs/stat.h"
#include "kernel/errno.h"

struct ntz_type {
    int32_t off;            // seconds east of UTC
    bool dst;
    uint8_t abbr;           // index into abbrs
};

struct ntz_rule_date {
    char kind;              // 'M' month.week.day, 'J' julian 1-365, 'N' 0-365
    int m, w, d, n;
    int32_t time;           // seconds after local midnight
};

struct ntz_rule {
    bool valid, has_dst;
    const char *std_name, *dst_name;
    int32_t std_off, dst_off;   // seconds east of UTC
    struct ntz_rule_date start, end;
};

struct ntz_zone {
    int64_t *trans;
    uint8_t *trans_type;
    size_t ntrans;
    struct ntz_type *types;
    size_t ntypes;
    const char **abbr_of_type;  // interned
    struct ntz_rule rule;
};

static pthread_mutex_t ntz_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ntz_zone ntz_zone;
static char ntz_key[512];
static bool ntz_loaded;
static time_t ntz_checked;

// tm_zone must outlive the call, as it does in every libc: abbreviations are
// interned for the life of the process. Zones have a handful each.
#define NTZ_INTERN_MAX 256
static char *ntz_interned[NTZ_INTERN_MAX];
static size_t ntz_ninterned;

static const char *ntz_intern(const char *s, size_t len) {
    for (size_t i = 0; i < ntz_ninterned; i++)
        if (strlen(ntz_interned[i]) == len && !strncmp(ntz_interned[i], s, len))
            return ntz_interned[i];
    if (ntz_ninterned == NTZ_INTERN_MAX)
        return "???";
    char *copy = strndup(s, len);
    if (copy == NULL)
        return "???";
    ntz_interned[ntz_ninterned++] = copy;
    return copy;
}

static void ntz_free(struct ntz_zone *z) {
    free(z->trans);
    free(z->trans_type);
    free(z->types);
    free(z->abbr_of_type);
    memset(z, 0, sizeof(*z));
}

// --- POSIX TZ strings ------------------------------------------------------

static const char *ntz_name(const char *p, const char **name) {
    const char *start;
    size_t len;
    if (*p == '<') {
        start = ++p;
        while (*p && *p != '>')
            p++;
        len = (size_t) (p - start);
        if (*p != '>')
            return NULL;
        p++;
    } else {
        start = p;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))
            p++;
        len = (size_t) (p - start);
        if (len < 3)
            return NULL;
    }
    *name = ntz_intern(start, len);
    return p;
}

// [+-]hh[:mm[:ss]] as seconds.
static const char *ntz_hms(const char *p, int32_t *out) {
    int sign = 1;
    if (*p == '+' || *p == '-')
        sign = *p++ == '-' ? -1 : 1;
    if (*p < '0' || *p > '9')
        return NULL;
    int32_t v = 0, part = 0;
    while (*p >= '0' && *p <= '9')
        part = part * 10 + (*p++ - '0');
    v = part * 3600;
    for (int mult = 60; *p == ':' && mult >= 1; mult /= 60) {
        p++;
        part = 0;
        while (*p >= '0' && *p <= '9')
            part = part * 10 + (*p++ - '0');
        v += part * mult;
    }
    *out = sign * v;
    return p;
}

static const char *ntz_rule_date(const char *p, struct ntz_rule_date *d) {
    memset(d, 0, sizeof(*d));
    if (*p == 'M') {
        d->kind = 'M';
        if (sscanf(p + 1, "%d.%d.%d", &d->m, &d->w, &d->d) != 3)
            return NULL;
        p++;
        while ((*p >= '0' && *p <= '9') || *p == '.')
            p++;
    } else {
        d->kind = 'N';
        if (*p == 'J') {
            d->kind = 'J';
            p++;
        }
        if (*p < '0' || *p > '9')
            return NULL;
        while (*p >= '0' && *p <= '9')
            d->n = d->n * 10 + (*p++ - '0');
    }
    d->time = 7200;
    if (*p == '/') {
        p = ntz_hms(p + 1, &d->time);
        if (p == NULL)
            return NULL;
    }
    return p;
}

static bool ntz_parse_rule(const char *s, struct ntz_rule *r) {
    memset(r, 0, sizeof(*r));
    int32_t west;
    const char *p = ntz_name(s, &r->std_name);
    if (p == NULL || (p = ntz_hms(p, &west)) == NULL)
        return false;
    r->std_off = -west;
    r->dst_off = r->std_off + 3600;
    if (*p) {
        p = ntz_name(p, &r->dst_name);
        if (p == NULL)
            return false;
        r->has_dst = true;
        if (*p && *p != ',') {
            if ((p = ntz_hms(p, &west)) == NULL)
                return false;
            r->dst_off = -west;
        }
        // No rules given: POSIX leaves it to the implementation, and glibc
        // and musl both use the US rules.
        const char *rules = *p == ',' ? p + 1 : "M3.2.0,M11.1.0";
        if ((rules = ntz_rule_date(rules, &r->start)) == NULL || *rules++ != ',')
            return false;
        if ((rules = ntz_rule_date(rules, &r->end)) == NULL || *rules)
            return false;
    }
    r->valid = true;
    return true;
}

static bool ntz_leap(int64_t y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

// Days from 1970-01-01 to year-month-day (month 1-12).
static int64_t ntz_days(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// The UTC instant of a rule date in `year`, at local `off` seconds east.
static int64_t ntz_rule_instant(const struct ntz_rule_date *d, int64_t year, int32_t off) {
    int64_t day;
    if (d->kind == 'J') {
        day = ntz_days(year, 1, 1) + d->n - 1;
        if (ntz_leap(year) && d->n >= 60)
            day++;
    } else if (d->kind == 'N') {
        day = ntz_days(year, 1, 1) + d->n;
    } else {
        int64_t first = ntz_days(year, d->m, 1);
        int wday = (int) (((first + 4) % 7 + 7) % 7);   // 1970-01-01 was a Thursday
        int64_t date = first + ((d->d - wday + 7) % 7) + 7 * (d->w - 1);
        static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        int len = mdays[d->m - 1] + (d->m == 2 && ntz_leap(year));
        while (date >= first + len)
            date -= 7;
        day = date;
    }
    return day * 86400 + d->time - off;
}

static void ntz_rule_at(const struct ntz_rule *r, int64_t t, int32_t *off, bool *dst, const char **abbr) {
    if (!r->has_dst) {
        *off = r->std_off;
        *dst = false;
        *abbr = r->std_name;
        return;
    }
    int64_t year = 1970 + (t / 86400) / 366;
    // Find the year containing t precisely enough: step until bracketing.
    while (ntz_days(year + 1, 1, 1) * 86400 <= t)
        year++;
    while (ntz_days(year, 1, 1) * 86400 > t)
        year--;
    // glibc computes a rule's transitions for no year before 1970: earlier
    // times are measured against 1970's, so a southern-hemisphere rule reads
    // them as daylight time and a northern one as standard.
    if (year < 1970)
        year = 1970;
    int64_t start = ntz_rule_instant(&r->start, year, r->std_off);
    int64_t end = ntz_rule_instant(&r->end, year, r->dst_off);
    bool in_dst = start < end ? (t >= start && t < end) : !(t >= end && t < start);
    *off = in_dst ? r->dst_off : r->std_off;
    *dst = in_dst;
    *abbr = in_dst ? r->dst_name : r->std_name;
}

// --- TZif ----------------------------------------------------------------

static uint32_t ntz_be32(const uint8_t *p) {
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

static int64_t ntz_be64(const uint8_t *p) {
    return (int64_t) ((uint64_t) ntz_be32(p) << 32 | ntz_be32(p + 4));
}

static uint8_t *ntz_read_guest(const char *path, size_t *len) {
    struct fd *fd = NULL;
    if (native_open(path, O_RDONLY_, &fd) < 0)
        return NULL;
    size_t cap = 4096, n = 0;
    uint8_t *buf = malloc(cap);
    while (buf != NULL) {
        if (n == cap) {
            uint8_t *grown = cap < (1 << 22) ? realloc(buf, cap * 2) : NULL;
            if (grown == NULL) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = grown;
            cap *= 2;
        }
        ssize_t got = native_read(fd, buf + n, cap - n);
        if (got <= 0)
            break;
        n += (size_t) got;
    }
    native_close(fd);
    *len = n;
    return buf;
}

static bool ntz_parse_tzif(const uint8_t *b, size_t len, struct ntz_zone *z) {
    if (len < 44 || memcmp(b, "TZif", 4))
        return false;
    uint32_t c[6];
    for (int i = 0; i < 6; i++)
        c[i] = ntz_be32(b + 20 + 4 * i);
    // c: isutcnt, isstdcnt, leapcnt, timecnt, typecnt, charcnt
    size_t v1 = (size_t) c[3] * 5 + (size_t) c[4] * 6 + c[5] + (size_t) c[2] * 8 + c[1] + c[0];
    const uint8_t *h = b;
    int tsize = 4;
    if (b[4] >= '2' && 44 + v1 + 44 <= len && !memcmp(b + 44 + v1, "TZif", 4)) {
        h = b + 44 + v1;
        tsize = 8;
        for (int i = 0; i < 6; i++)
            c[i] = ntz_be32(h + 20 + 4 * i);
    }
    const uint8_t *p = h + 44;
    size_t need = (size_t) c[3] * (size_t) (tsize + 1) + (size_t) c[4] * 6 + c[5] +
                  (size_t) c[2] * (size_t) (tsize + 4) + c[1] + c[0];
    if (p + need > b + len || c[4] == 0)
        return false;
    z->ntrans = c[3];
    z->ntypes = c[4];
    z->trans = calloc(z->ntrans + 1, sizeof(int64_t));
    z->trans_type = calloc(z->ntrans + 1, 1);
    z->types = calloc(z->ntypes, sizeof(struct ntz_type));
    z->abbr_of_type = calloc(z->ntypes, sizeof(char *));
    if (!z->trans || !z->trans_type || !z->types || !z->abbr_of_type)
        return false;
    for (size_t i = 0; i < z->ntrans; i++, p += tsize)
        z->trans[i] = tsize == 8 ? ntz_be64(p) : (int64_t) (int32_t) ntz_be32(p);
    for (size_t i = 0; i < z->ntrans; i++)
        z->trans_type[i] = *p++ < z->ntypes ? p[-1] : 0;
    const uint8_t *types = p;
    p += (size_t) c[4] * 6;
    const char *abbrs = (const char *) p;
    for (size_t i = 0; i < z->ntypes; i++) {
        z->types[i].off = (int32_t) ntz_be32(types + 6 * i);
        z->types[i].dst = types[6 * i + 4] != 0;
        uint8_t ai = types[6 * i + 5];
        const char *a = ai < c[5] ? abbrs + ai : "";
        z->abbr_of_type[i] = ntz_intern(a, strnlen(a, c[5] - (ai < c[5] ? ai : 0)));
    }
    p += c[5] + (size_t) c[2] * (size_t) (tsize + 4) + c[1] + c[0];
    // The footer: "\nRULE\n", for times after the last transition.
    if (tsize == 8 && p < b + len && *p == '\n') {
        const uint8_t *end = memchr(p + 1, '\n', (size_t) (b + len - p - 1));
        if (end != NULL && end > p + 1) {
            char rule[128];
            snprintf(rule, sizeof(rule), "%.*s", (int) (end - p - 1), (const char *) p + 1);
            ntz_parse_rule(rule, &z->rule);
        }
    }
    return true;
}

// --- Choosing the zone ------------------------------------------------------

static void ntz_utc(struct ntz_zone *z, const char *name) {
    ntz_free(z);
    z->rule.valid = true;
    z->rule.std_name = name;
}

static bool ntz_load_file(const char *path, struct ntz_zone *z) {
    size_t len = 0;
    uint8_t *buf = ntz_read_guest(path, &len);
    if (buf == NULL)
        return false;
    struct ntz_zone fresh = {0};
    bool ok = ntz_parse_tzif(buf, len, &fresh);
    free(buf);
    if (!ok) {
        ntz_free(&fresh);
        return false;
    }
    ntz_free(z);
    *z = fresh;
    return true;
}

// The identity of the zone in force now, cheaply: TZ's value, or the
// /etc/localtime file's inode and mtime.
static void ntz_current_key(char *key, size_t size) {
    const char *tz = nlibc_getenv("TZ");
    if (tz != NULL) {
        snprintf(key, size, "TZ=%s", tz);
        return;
    }
    struct statbuf st;
    if (native_stat("/etc/localtime", &st, true) == 0)
        snprintf(key, size, "/etc/localtime %llu %u %u", (unsigned long long) st.inode, st.mtime, st.mtime_nsec);
    else
        snprintf(key, size, "UTC");
}

static void ntz_load(const char *key) {
    struct ntz_zone *z = &ntz_zone;
    if (!strncmp(key, "TZ=", 3)) {
        const char *tz = key + 3;
        if (*tz == ':')
            tz++;
        // An empty TZ is glibc's "Universal": that zoneinfo file if there is
        // one, else UTC under that name.
        if (*tz == '\0')
            tz = "Universal";
        if (*tz == '/') {
            if (!ntz_load_file(tz, z))
                ntz_utc(z, ntz_intern("UTC", 3));
            return;
        }
        if (!strstr(tz, "..")) {
            static const char *const dirs[] = {"/usr/share/zoneinfo/", "/usr/lib/zoneinfo/", "/usr/share/lib/zoneinfo/"};
            for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
                char path[512];
                snprintf(path, sizeof(path), "%s%s", dirs[i], tz);
                if (ntz_load_file(path, z))
                    return;
            }
        }
        struct ntz_rule r;
        if (ntz_parse_rule(tz, &r)) {
            ntz_free(z);
            z->rule = r;
            return;
        }
        // Unknown: UTC under the given name, as glibc does.
        size_t n = 0;
        while ((tz[n] >= 'a' && tz[n] <= 'z') || (tz[n] >= 'A' && tz[n] <= 'Z'))
            n++;
        ntz_utc(z, n >= 3 ? ntz_intern(tz, n) : ntz_intern("UTC", 3));
        return;
    }
    if (!strncmp(key, "/etc/localtime", 14) && ntz_load_file("/etc/localtime", z))
        return;
    ntz_utc(z, ntz_intern("UTC", 3));
}

// Make ntz_zone current. Caller holds ntz_lock.
static void ntz_refresh(bool force) {
    // /etc/localtime is re-checked at most once a second; TZ every call, as
    // reading it costs nothing and each program may set its own.
    time_t now = time(NULL);
    if (ntz_loaded && !force && now == ntz_checked && strncmp(ntz_key, "TZ=", 3) && nlibc_getenv("TZ") == NULL)
        return;
    char key[sizeof(ntz_key)];
    ntz_current_key(key, sizeof(key));
    ntz_checked = now;
    if (ntz_loaded && !force && !strcmp(key, ntz_key))
        return;
    snprintf(ntz_key, sizeof(ntz_key), "%s", key);
    ntz_load(key);
    ntz_loaded = true;
}

// The offset, DST flag and abbreviation in force at UTC instant t.
static void ntz_at(int64_t t, int32_t *off, bool *dst, const char **abbr) {
    const struct ntz_zone *z = &ntz_zone;
    if (z->ntypes == 0) {
        ntz_rule_at(&z->rule, t, off, dst, abbr);
        return;
    }
    if (z->ntrans == 0 || t < z->trans[0]) {
        // Before the first transition: the first standard-time type.
        size_t i = 0;
        while (i < z->ntypes && z->types[i].dst)
            i++;
        if (i == z->ntypes)
            i = 0;
        if (z->ntrans == 0 && z->rule.valid) {
            ntz_rule_at(&z->rule, t, off, dst, abbr);
            return;
        }
        *off = z->types[i].off;
        *dst = z->types[i].dst;
        *abbr = z->abbr_of_type[i];
        return;
    }
    if (t >= z->trans[z->ntrans - 1] && z->rule.valid) {
        ntz_rule_at(&z->rule, t, off, dst, abbr);
        return;
    }
    size_t lo = 0, hi = z->ntrans;     // last transition <= t
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (z->trans[mid] <= t)
            lo = mid;
        else
            hi = mid;
    }
    const struct ntz_type *ty = &z->types[z->trans_type[lo]];
    *off = ty->off;
    *dst = ty->dst;
    *abbr = z->abbr_of_type[z->trans_type[lo]];
}

static struct tm *ntz_fill(int64_t t, struct tm *out) {
    int32_t off;
    bool dst;
    const char *abbr;
    ntz_at(t, &off, &dst, &abbr);
    time_t shifted = (time_t) (t + off);
    if (gmtime_r(&shifted, out) == NULL)
        return NULL;
    out->tm_isdst = dst;
    out->tm_gmtoff = off;
    out->tm_zone = (char *) abbr;
    return out;
}

// --- The libc interface ---------------------------------------------------

struct tm *nlibc_localtime_r(const time_t *t, struct tm *out) {
    if (t == NULL || out == NULL) {
        errno = EFAULT;
        return NULL;
    }
    pthread_mutex_lock(&ntz_lock);
    ntz_refresh(false);
    struct tm *r = ntz_fill(*t, out);
    pthread_mutex_unlock(&ntz_lock);
    if (r == NULL)
        errno = EOVERFLOW;
    return r;
}

struct tm *nlibc_localtime(const time_t *t) {
    static __thread struct tm result;
    return nlibc_localtime_r(t, &result);
}

void nlibc_tzset(void) {
    pthread_mutex_lock(&ntz_lock);
    ntz_refresh(true);
    pthread_mutex_unlock(&ntz_lock);
}

// mktime: the local wall-clock fields to an instant. A time that occurs
// twice (the hour DST ends) takes tm_isdst's choice, standard time when it is
// -1; a time that never occurs (the hour DST starts) is read with the offset
// before the change, so it lands after it, as glibc does.
time_t nlibc_mktime(struct tm *tm) {
    if (tm == NULL) {
        errno = EFAULT;
        return (time_t) -1;
    }
    struct tm fields = *tm;
    fields.tm_isdst = 0;
    time_t local = timegm(&fields);    // normalises the fields too
    if (local == (time_t) -1 && !(fields.tm_year == 69 && fields.tm_mon == 11 && fields.tm_mday == 31)) {
        errno = EOVERFLOW;
        return (time_t) -1;
    }
    pthread_mutex_lock(&ntz_lock);
    ntz_refresh(false);
    int32_t offs[2];
    bool dsts[2];
    const char *abbr;
    ntz_at((int64_t) local - 86400, &offs[0], &dsts[0], &abbr);
    ntz_at((int64_t) local + 86400, &offs[1], &dsts[1], &abbr);
    int64_t pick = 0;
    bool found = false;
    if (tm->tm_isdst < 0) {
        // glibc's search, which decides the hour that occurs twice: start
        // from offset 0 and follow the offset found at each guess until it
        // holds. London's 01:30 on the last Sunday of October lands on GMT,
        // Los Angeles's on PDT -- the same rule, different answers.
        int64_t t = (int64_t) local;
        for (int probe = 0; probe < 6; probe++) {
            int32_t off;
            bool dst;
            ntz_at(t, &off, &dst, &abbr);
            int64_t next = (int64_t) local - off;
            if (next == t) {
                pick = t;
                found = true;
                break;
            }
            t = next;
        }
        if (found)
            goto fill;
    }
    for (int i = 0; i < 2; i++) {
        int64_t t = (int64_t) local - offs[i];
        int32_t off;
        bool dst;
        ntz_at(t, &off, &dst, &abbr);
        if (off != offs[i])
            continue;               // that offset does not hold there
        if (!found || (tm->tm_isdst > 0 ? dst : !dst)) {
            pick = t;
            found = true;
        }
    }
    if (!found)
        pick = (int64_t) local - offs[0];   // in the gap
    else if (tm->tm_isdst >= 0) {
        // A stated tm_isdst that the result contradicts shifts the instant by
        // the difference, glibc's reading of a caller's hint.
        int32_t off;
        bool dst;
        ntz_at(pick, &off, &dst, &abbr);
        if ((tm->tm_isdst > 0) != dst && offs[0] != offs[1]) {
            int32_t other = offs[0] == off ? offs[1] : offs[0];
            pick += off - other;
        }
    }
fill:;
    struct tm out;
    struct tm *ok = ntz_fill(pick, &out);
    pthread_mutex_unlock(&ntz_lock);
    if (ok == NULL) {
        errno = EOVERFLOW;
        return (time_t) -1;
    }
    *tm = out;
    return (time_t) pick;
}

char *nlibc_ctime_r(const time_t *t, char *buf) {
    struct tm tm;
    if (nlibc_localtime_r(t, &tm) == NULL)
        return NULL;
    return asctime_r(&tm, buf);
}

char *nlibc_ctime(const time_t *t) {
    static __thread char buf[64];
    return nlibc_ctime_r(t, buf);
}
