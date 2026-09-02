/*
 * taskstats_genl.c — self-checking regression lock for generic netlink
 * (NETLINK_GENERIC) and the TASKSTATS family, iotop's only per-pid data
 * source (iotop-c hard-exits in nl_init() if either is missing).
 *
 * Verifies:
 *   - socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC) opens and binds
 *   - genl ctrl CTRL_CMD_GETFAMILY resolves "TASKSTATS" to a family id
 *   - GETFAMILY for a bogus family name errors with -ENOENT (fallback path
 *     for other genl users like nl80211 probes)
 *   - TASKSTATS_CMD_GET + TASKSTATS_CMD_ATTR_PID returns a
 *     TASKSTATS_TYPE_AGGR_PID nest with our pid and a struct taskstats
 *     whose version >= 8 and whose io fields reflect file I/O we just did
 *   - the TGID variant works too (AGGR_TGID)
 *   - GET for a nonexistent pid errors with -ESRCH
 *
 * Self-contained ABI definitions (no linux-headers dependency, builds on
 * musl-only Alpine). Also passes on a real Linux kernel when run as root
 * (kernel taskstats requires CAP_NET_ADMIN since CVE-2011-2494); non-root
 * runs skip the taskstats half after checking the family resolves.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "test_common.h"

#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
#ifndef NETLINK_GENERIC
#define NETLINK_GENERIC 16
#endif
#ifndef SOL_NETLINK
#define SOL_NETLINK 270
#endif

struct nlmsghdr_t {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
};
struct nlattr_t {
    uint16_t nla_len;
    uint16_t nla_type;
};
struct genlmsghdr_t {
    uint8_t cmd;
    uint8_t version;
    uint16_t reserved;
};
struct sockaddr_nl_t {
    uint16_t nl_family;
    uint16_t nl_pad;
    uint32_t nl_pid;
    uint32_t nl_groups;
};

#define NLMSG_ERROR_T 2
#define NLM_F_REQUEST_T 1
#define NLA_ALIGN4(x) (((x) + 3u) & ~3u)
#define NLA_TYPE_MASK_T 0x3fff

#define GENL_ID_CTRL_T 0x10
#define CTRL_CMD_GETFAMILY_T 3
#define CTRL_ATTR_FAMILY_ID_T 1
#define CTRL_ATTR_FAMILY_NAME_T 2

#define TASKSTATS_CMD_GET_T 1
#define TASKSTATS_CMD_ATTR_PID_T 1
#define TASKSTATS_CMD_ATTR_TGID_T 2
#define TASKSTATS_TYPE_PID_T 1
#define TASKSTATS_TYPE_TGID_T 2
#define TASKSTATS_TYPE_STATS_T 3
#define TASKSTATS_TYPE_AGGR_PID_T 4
#define TASKSTATS_TYPE_AGGR_TGID_T 5

/* struct taskstats prefix through v8; append-only ABI, aligned(8) mirrors
 * the uapi header so the layout matches on every arch. */
struct taskstats_v8 {
    uint16_t version;
    uint32_t ac_exitcode;
    uint8_t ac_flag;
    uint8_t ac_nice;
    uint64_t cpu_count __attribute__((aligned(8)));
    uint64_t cpu_delay_total;
    uint64_t blkio_count;
    uint64_t blkio_delay_total;
    uint64_t swapin_count;
    uint64_t swapin_delay_total;
    uint64_t cpu_run_real_total;
    uint64_t cpu_run_virtual_total;
    char ac_comm[32];
    uint8_t ac_sched __attribute__((aligned(8)));
    uint8_t ac_pad[3];
    uint32_t ac_uid __attribute__((aligned(8)));
    uint32_t ac_gid;
    uint32_t ac_pid;
    uint32_t ac_ppid;
    uint32_t ac_btime;
    uint64_t ac_etime __attribute__((aligned(8)));
    uint64_t ac_utime;
    uint64_t ac_stime;
    uint64_t ac_minflt;
    uint64_t ac_majflt;
    uint64_t coremem;
    uint64_t virtmem;
    uint64_t hiwater_rss;
    uint64_t hiwater_vm;
    uint64_t read_char;
    uint64_t write_char;
    uint64_t read_syscalls;
    uint64_t write_syscalls;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t cancelled_write_bytes;
    uint64_t nvcsw;
    uint64_t nivcsw;
    uint64_t ac_utimescaled;
    uint64_t ac_stimescaled;
    uint64_t cpu_scaled_run_real_total;
    uint64_t freepages_count;
    uint64_t freepages_delay_total;
};

static int check(const char *label, int cond) {
    if (cond) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

/* Build and send one genl request with a single attribute. */
static int genl_send(int fd, uint16_t nl_type, uint8_t cmd, uint32_t seq,
        uint16_t attr_type, const void *attr_data, uint16_t attr_len) {
    char buf[256] = {0};
    struct nlmsghdr_t *nlh = (struct nlmsghdr_t *) buf;
    struct genlmsghdr_t *genl = (struct genlmsghdr_t *) (buf + sizeof(*nlh));
    struct nlattr_t *attr = (struct nlattr_t *) (buf + sizeof(*nlh) + sizeof(*genl));
    genl->cmd = cmd;
    genl->version = 1;
    attr->nla_type = attr_type;
    attr->nla_len = sizeof(*attr) + attr_len;
    memcpy(attr + 1, attr_data, attr_len);
    nlh->nlmsg_len = sizeof(*nlh) + sizeof(*genl) + NLA_ALIGN4(attr->nla_len);
    nlh->nlmsg_type = nl_type;
    nlh->nlmsg_flags = NLM_F_REQUEST_T;
    nlh->nlmsg_seq = seq;
    ssize_t r = send(fd, buf, nlh->nlmsg_len, 0);
    return r == (ssize_t) nlh->nlmsg_len ? 0 : -1;
}

/* Receive one reply. Returns bytes, or 0 and sets *nl_errno if NLMSG_ERROR. */
static ssize_t genl_recv(int fd, char *buf, size_t cap, int *nl_errno) {
    *nl_errno = 0;
    ssize_t r = recv(fd, buf, cap, 0);
    if (r < (ssize_t) sizeof(struct nlmsghdr_t))
        return -1;
    struct nlmsghdr_t *nlh = (struct nlmsghdr_t *) buf;
    if (nlh->nlmsg_type == NLMSG_ERROR_T) {
        int32_t err;
        memcpy(&err, buf + sizeof(*nlh), sizeof(err));
        *nl_errno = -err;
        return 0;
    }
    return r;
}

static const struct nlattr_t *attr_find(const void *attrs, size_t len, uint16_t type) {
    size_t off = 0;
    while (off + sizeof(struct nlattr_t) <= len) {
        const struct nlattr_t *attr = (const struct nlattr_t *) ((const char *) attrs + off);
        if (attr->nla_len < sizeof(*attr) || off + attr->nla_len > len)
            break;
        if ((attr->nla_type & NLA_TYPE_MASK_T) == type)
            return attr;
        off += NLA_ALIGN4(attr->nla_len);
    }
    return NULL;
}

/* Ask for taskstats of id; on success copy the stats prefix out. */
static int taskstats_get(int fd, uint16_t family, int use_tgid, uint32_t id,
        uint32_t seq, struct taskstats_v8 *ts, uint32_t *out_id, int *nl_errno) {
    uint16_t attr = use_tgid ? TASKSTATS_CMD_ATTR_TGID_T : TASKSTATS_CMD_ATTR_PID_T;
    if (genl_send(fd, family, TASKSTATS_CMD_GET_T, seq, attr, &id, sizeof(id)) != 0)
        return -1;
    char buf[2048];
    ssize_t r = genl_recv(fd, buf, sizeof(buf), nl_errno);
    if (r <= 0)
        return r == 0 ? 1 : -1; /* 1 = NLMSG_ERROR received */
    struct nlmsghdr_t *nlh = (struct nlmsghdr_t *) buf;
    if (nlh->nlmsg_type != family)
        return -1;
    const char *attrs = buf + sizeof(*nlh) + sizeof(struct genlmsghdr_t);
    size_t attrs_len = nlh->nlmsg_len - sizeof(*nlh) - sizeof(struct genlmsghdr_t);
    uint16_t aggr_type = use_tgid ? TASKSTATS_TYPE_AGGR_TGID_T : TASKSTATS_TYPE_AGGR_PID_T;
    const struct nlattr_t *aggr = attr_find(attrs, attrs_len, aggr_type);
    if (aggr == NULL)
        return -1;
    const void *nested = aggr + 1;
    size_t nested_len = aggr->nla_len - sizeof(*aggr);
    uint16_t id_type = use_tgid ? TASKSTATS_TYPE_TGID_T : TASKSTATS_TYPE_PID_T;
    const struct nlattr_t *id_attr = attr_find(nested, nested_len, id_type);
    const struct nlattr_t *stats = attr_find(nested, nested_len, TASKSTATS_TYPE_STATS_T);
    if (id_attr == NULL || stats == NULL)
        return -1;
    memcpy(out_id, id_attr + 1, sizeof(*out_id));
    size_t stats_len = stats->nla_len - sizeof(*stats);
    if (stats_len > sizeof(*ts))
        stats_len = sizeof(*ts);
    memset(ts, 0, sizeof(*ts));
    memcpy(ts, stats + 1, stats_len);
    return 0;
}

#define CHUNK 4096
#define CHUNKS 32

int main(int argc, char **argv) {
    test_init(argc, argv);

    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (!check("genl.socket", fd >= 0)) {
        printf("taskstats_genl: FAIL (socket: %s)\n", strerror(errno));
        return 1;
    }
    struct sockaddr_nl_t addr = { .nl_family = AF_NETLINK };
    check("genl.bind", bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0);

    /* resolve TASKSTATS */
    uint16_t family = 0;
    {
        static const char name[] = "TASKSTATS";
        if (!check("ctrl.getfamily.send",
                genl_send(fd, GENL_ID_CTRL_T, CTRL_CMD_GETFAMILY_T, 1,
                        CTRL_ATTR_FAMILY_NAME_T, name, sizeof(name)) == 0))
            goto done;
        char buf[2048];
        int nl_errno;
        ssize_t r = genl_recv(fd, buf, sizeof(buf), &nl_errno);
        if (!check("ctrl.getfamily.reply", r > 0)) {
            test_log_if(1, "  (netlink error %d: %s)\n", nl_errno, strerror(nl_errno));
            goto done;
        }
        struct nlmsghdr_t *nlh = (struct nlmsghdr_t *) buf;
        const char *attrs = buf + sizeof(*nlh) + sizeof(struct genlmsghdr_t);
        size_t attrs_len = nlh->nlmsg_len - sizeof(*nlh) - sizeof(struct genlmsghdr_t);
        const struct nlattr_t *id_attr = attr_find(attrs, attrs_len, CTRL_ATTR_FAMILY_ID_T);
        if (check("ctrl.getfamily.id_attr", id_attr != NULL)) {
            memcpy(&family, id_attr + 1, sizeof(family));
            test_logf("ok   ctrl.family_id = %u\n", family);
        }
        check("ctrl.family_id.range", family >= 0x10);
    }
    if (family == 0)
        goto done;

    /* unknown family name resolves like an unregistered family; the name
     * must fit GENL_NAMSIZ (16) or real kernels EINVAL in policy validation
     * before the lookup */
    {
        static const char bogus[] = "ISH_NOFAM";
        int nl_errno = 0;
        char buf[512];
        if (genl_send(fd, GENL_ID_CTRL_T, CTRL_CMD_GETFAMILY_T, 2,
                CTRL_ATTR_FAMILY_NAME_T, bogus, sizeof(bogus)) == 0) {
            ssize_t r = genl_recv(fd, buf, sizeof(buf), &nl_errno);
            check("ctrl.bogus_family.enoent", r == 0 && nl_errno == ENOENT);
        }
        static const char toolong[] = "ISH_NAME_LONGER_THAN_GENL_NAMSIZ";
        if (genl_send(fd, GENL_ID_CTRL_T, CTRL_CMD_GETFAMILY_T, 6,
                CTRL_ATTR_FAMILY_NAME_T, toolong, sizeof(toolong)) == 0) {
            ssize_t r = genl_recv(fd, buf, sizeof(buf), &nl_errno);
            check("ctrl.oversize_family.einval", r == 0 && nl_errno == EINVAL);
        }
    }

    if (geteuid() != 0) {
        printf("taskstats_genl: family resolved; skipping GET checks (need root)\n");
        goto done;
    }

    /* generate known file I/O, then query ourselves */
    char path[] = "taskstatsXXXXXX";
    int wfd = mkstemp(path);
    if (check("io.tempfile", wfd >= 0)) {
        static char chunk[CHUNK];
        memset(chunk, 'q', sizeof(chunk));
        long long wrote = 0;
        for (int i = 0; i < CHUNKS; i++) {
            ssize_t w = write(wfd, chunk, sizeof(chunk));
            if (w > 0)
                wrote += w;
        }
        check("io.wrote", wrote == (long long) CHUNK * CHUNKS);
        close(wfd);

        struct taskstats_v8 ts;
        uint32_t out_id = 0;
        int nl_errno = 0;
        int r = taskstats_get(fd, family, 0, (uint32_t) getpid(), 3, &ts, &out_id, &nl_errno);
        if (check("get.pid.ok", r == 0)) {
            check("get.pid.id", out_id == (uint32_t) getpid());
            check("get.pid.version", ts.version >= 8);
            check("get.pid.ac_pid", ts.ac_pid == (uint32_t) getpid());
            check("get.pid.write_char", ts.write_char >= (uint64_t) CHUNK * CHUNKS);
            check("get.pid.write_bytes", ts.write_bytes >= (uint64_t) CHUNK * CHUNKS);
            /* not asserted: read/write_syscalls are zero on some real
             * kernels (config-dependent), though AOK fills them */
            check("get.pid.comm", ts.ac_comm[0] != '\0');
            test_logf("     comm=%s uid=%u write_char=%llu write_bytes=%llu blkio_count=%llu blkio_delay=%lluns\n",
                    ts.ac_comm, ts.ac_uid,
                    (unsigned long long) ts.write_char,
                    (unsigned long long) ts.write_bytes,
                    (unsigned long long) ts.blkio_count,
                    (unsigned long long) ts.blkio_delay_total);
        } else if (r == 1) {
            test_log_if(1, "  (netlink error %d: %s)\n", nl_errno, strerror(nl_errno));
        }

        /* TGID aggregate parses too. Real kernels only aggregate delay
         * fields for TGID queries (io stays zero); AOK fills io as a
         * superset, so io contents are logged, not asserted. */
        r = taskstats_get(fd, family, 1, (uint32_t) getpid(), 4, &ts, &out_id, &nl_errno);
        if (check("get.tgid.ok", r == 0)) {
            check("get.tgid.id", out_id == (uint32_t) getpid());
            test_logf("     tgid write_char=%llu\n", (unsigned long long) ts.write_char);
        }
        unlink(path);
    }

    /* nonexistent pid: ESRCH */
    {
        struct taskstats_v8 ts;
        uint32_t out_id;
        int nl_errno = 0;
        int r = taskstats_get(fd, family, 0, 999999, 5, &ts, &out_id, &nl_errno);
        check("get.badpid.esrch", r == 1 && nl_errno == ESRCH);
    }

done:
    close(fd);
    if (failures_total) {
        printf("taskstats_genl: %u FAILURES\n", failures_total);
        return 1;
    }
    printf("taskstats_genl: PASS\n");
    return 0;
}
