// proc_pid_io.c — self-checking regression lock for per-task I/O accounting
// (/proc/<pid>/io) and the /proc/vmstat pgpgin/pgpgout totals that feed
// iotop/pidstat:
//
//   - /proc/self/io exists and has all 7 canonical fields
//   - file writes bump wchar/syscw and write_bytes (file-backed I/O)
//   - file reads bump rchar/syscr and read_bytes
//   - pipe traffic bumps rchar/wchar but NOT read_bytes/write_bytes
//     (only file-backed fds count as "storage" I/O)
//   - a live child's counters are visible via /proc/<pid>/io from the parent
//   - a thread's I/O survives the thread's exit in the process totals
//     (tgroup io_dead rollup)
//   - /proc/vmstat pgpgout grows (in KiB) when files are written
//
// The same source passes on a real Linux kernel, with one caveat: on real
// hardware read_bytes only counts page-cache misses, so the file-read check
// uses rchar (which is exact on both) and only requires read_bytes to be
// parseable, not to grow.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

struct io_counters {
    unsigned long long rchar, wchar, syscr, syscw;
    unsigned long long read_bytes, write_bytes, cancelled_write_bytes;
};

static int read_io(const char *path, struct io_counters *io) {
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    memset(io, 0, sizeof(*io));
    int seen = 0;
    char key[64];
    unsigned long long val;
    while (fscanf(f, "%63[^:]: %llu\n", key, &val) == 2) {
        if (!strcmp(key, "rchar")) { io->rchar = val; seen |= 1 << 0; }
        else if (!strcmp(key, "wchar")) { io->wchar = val; seen |= 1 << 1; }
        else if (!strcmp(key, "syscr")) { io->syscr = val; seen |= 1 << 2; }
        else if (!strcmp(key, "syscw")) { io->syscw = val; seen |= 1 << 3; }
        else if (!strcmp(key, "read_bytes")) { io->read_bytes = val; seen |= 1 << 4; }
        else if (!strcmp(key, "write_bytes")) { io->write_bytes = val; seen |= 1 << 5; }
        else if (!strcmp(key, "cancelled_write_bytes")) { io->cancelled_write_bytes = val; seen |= 1 << 6; }
    }
    fclose(f);
    return seen == 0x7f ? 0 : -1;
}

static int check(const char *label, int cond) {
    if (cond) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

static unsigned long long read_vmstat_field(const char *want) {
    FILE *f = fopen("/proc/vmstat", "r");
    if (f == NULL)
        return (unsigned long long) -1;
    char key[64];
    unsigned long long val, res = (unsigned long long) -1;
    while (fscanf(f, "%63s %llu\n", key, &val) == 2)
        if (!strcmp(key, want))
            res = val;
    fclose(f);
    return res;
}

#define CHUNK 4096
#define CHUNKS 32          // 128 KiB per phase
static char buf[CHUNK];

// write CHUNKS * CHUNK bytes to fd with plain write()
static long long write_file(int fd) {
    long long total = 0;
    for (int i = 0; i < CHUNKS; i++) {
        ssize_t r = write(fd, buf, CHUNK);
        if (r < 0)
            return -1;
        total += r;
    }
    return total;
}

static void *thread_write_main(void *arg) {
    const char *path = arg;
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
        return (void *) -1L;
    long long n = write_file(fd);
    close(fd);
    return (void *) (long) (n > 0 ? 0 : -1);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    memset(buf, 'x', sizeof(buf));

    // Deliberately NOT /tmp: that's often tmpfs, which (correctly) doesn't
    // count as storage I/O. The cwd is fakefs/realfs on device and in the
    // local harness, and a real filesystem on Linux.
    char dir[] = "procioXXXXXX";
    if (mkdtemp(dir) == NULL) {
        printf("FAIL mkdtemp: %s\n", strerror(errno));
        return 1;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/data", dir);

    // ---- /proc/self/io exists with all fields ----
    struct io_counters before, after;
    if (read_io("/proc/self/io", &before) != 0) {
        printf("FAIL /proc/self/io missing or missing fields\n");
        return 1;
    }
    test_logf("ok   proc.self.io.parse\n");

    // ---- file writes count in wchar/syscw/write_bytes ----
    unsigned long long pgpgout0 = read_vmstat_field("pgpgout");
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        printf("FAIL open %s: %s\n", path, strerror(errno));
        return 1;
    }
    long long wrote = write_file(fd);
    close(fd);
    check("file.write_ok", wrote == (long long) CHUNKS * CHUNK);
    read_io("/proc/self/io", &after);
    check("file.wchar", after.wchar >= before.wchar + (unsigned long long) wrote);
    check("file.syscw", after.syscw >= before.syscw + CHUNKS);
    check("file.write_bytes", after.write_bytes >= before.write_bytes + (unsigned long long) wrote);

    // ---- file reads count in rchar/syscr (read_bytes exact under AOK,
    //      cache-dependent on real Linux, so only rchar is asserted) ----
    before = after;
    fd = open(path, O_RDONLY);
    long long got = 0;
    ssize_t r;
    while ((r = read(fd, buf, CHUNK)) > 0)
        got += r;
    close(fd);
    check("file.read_ok", got == wrote);
    read_io("/proc/self/io", &after);
    check("file.rchar", after.rchar >= before.rchar + (unsigned long long) got);
    check("file.syscr", after.syscr >= before.syscr + CHUNKS);

    // ---- pipe traffic: rchar/wchar move, read_bytes/write_bytes don't ----
    // No stdout output between the two snapshots: if stdout is redirected to
    // a file on disk, a verbose log line would move write_bytes.
    fflush(stdout);
    read_io("/proc/self/io", &before);
    int pfd[2];
    if (pipe(pfd) == 0) {
        // Stay under the pipe buffer so a single-threaded write-then-read
        // round trip can't block.
        char pbuf[8192];
        memset(pbuf, 'p', sizeof(pbuf));
        ssize_t pw = write(pfd[1], pbuf, sizeof(pbuf));
        ssize_t pr = read(pfd[0], pbuf, sizeof(pbuf));
        close(pfd[0]);
        close(pfd[1]);
        read_io("/proc/self/io", &after);
        check("pipe.roundtrip", pw == (ssize_t) sizeof(pbuf) && pr == pw);
        check("pipe.wchar", after.wchar >= before.wchar + sizeof(pbuf));
        check("pipe.rchar", after.rchar >= before.rchar + sizeof(pbuf));
        check("pipe.write_bytes_unchanged", after.write_bytes == before.write_bytes);
        // The /proc/self/io reads above are not disk-backed either:
        check("pipe.read_bytes_unchanged", after.read_bytes == before.read_bytes);
    }

    // ---- a live child's counters via /proc/<pid>/io ----
    int sync_pipe[2], exit_pipe[2];
    if (pipe(sync_pipe) != 0 || pipe(exit_pipe) != 0) {
        printf("FAIL pipe: %s\n", strerror(errno));
        return 1;
    }
    pid_t child = fork();
    if (child == 0) {
        close(sync_pipe[0]);
        close(exit_pipe[1]);
        char cpath[256];
        snprintf(cpath, sizeof(cpath), "%s/child", dir);
        int cfd = open(cpath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        long long n = cfd >= 0 ? write_file(cfd) : -1;
        if (cfd >= 0)
            close(cfd);
        // tell the parent we're done writing, then wait to be released
        char c = n == (long long) CHUNKS * CHUNK ? '1' : '0';
        if (write(sync_pipe[1], &c, 1) != 1)
            _exit(1);
        if (read(exit_pipe[0], &c, 1) < 0)
            _exit(1);
        _exit(0);
    }
    close(sync_pipe[1]);
    close(exit_pipe[0]);
    char c = 0;
    check("child.wrote", read(sync_pipe[0], &c, 1) == 1 && c == '1');
    char child_io_path[64];
    snprintf(child_io_path, sizeof(child_io_path), "/proc/%d/io", (int) child);
    struct io_counters child_io;
    if (check("child.io.parse", read_io(child_io_path, &child_io) == 0)) {
        check("child.wchar", child_io.wchar >= (unsigned long long) CHUNKS * CHUNK);
        check("child.write_bytes", child_io.write_bytes >= (unsigned long long) CHUNKS * CHUNK);
    }
    check("child.release", write(exit_pipe[1], "g", 1) == 1);
    int status;
    check("child.reaped", waitpid(child, &status, 0) == child &&
            WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(sync_pipe[0]);
    close(exit_pipe[1]);

    // ---- exited thread's I/O rolls up into the process totals ----
    read_io("/proc/self/io", &before);
    char tpath[256];
    snprintf(tpath, sizeof(tpath), "%s/thread", dir);
    pthread_t th;
    void *tret = (void *) -1L;
    if (check("thread.create", pthread_create(&th, NULL, thread_write_main, tpath) == 0)) {
        pthread_join(th, &tret);
        check("thread.wrote", tret == NULL);
        read_io("/proc/self/io", &after);
        check("thread.rollup.wchar", after.wchar >= before.wchar + (unsigned long long) CHUNKS * CHUNK);
        check("thread.rollup.write_bytes",
                after.write_bytes >= before.write_bytes + (unsigned long long) CHUNKS * CHUNK);
    }

    // ---- vmstat pgpgout moved with all that file writing (KiB units) ----
    // On real Linux pgpgout only moves at writeback time; force it so the
    // check is portable. Under AOK the accounting is immediate and sync is
    // harmless.
    sync();
    unsigned long long pgpgout1 = read_vmstat_field("pgpgout");
    if (pgpgout0 != (unsigned long long) -1 && pgpgout1 != (unsigned long long) -1) {
        // at least the first phase's 128 KiB should show up
        check("vmstat.pgpgout", pgpgout1 >= pgpgout0 + (CHUNKS * CHUNK) / 1024);
    }

    // cleanup
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);

    if (failures_total) {
        printf("proc_pid_io: %u FAILURES\n", failures_total);
        return 1;
    }
    printf("proc_pid_io: PASS\n");
    return 0;
}
