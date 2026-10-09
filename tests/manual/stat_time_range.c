// stat_time_range.c -- file times are signed and 64-bit. A time before 1970
// is negative and one past 2106 fits; utimensat, utimes and utime set them
// (a 32-bit task's time_t is signed too), and statx gives them back whole.
// A 64-bit task's stat does as well; i386's stat64 keeps the low 32 bits, as
// Linux's does. AOK kept them in 32 unsigned bits: `touch -d @-1` read back
// as 4294967295, 2106-02-07. Raw system calls, so that no libc conversion
// stands between; in /tmp (the root's own filesystem in AOK's roots: fakefs)
// and on a tmpfs (/dev/shm), where there is one. Checked on camd, 64- and 32-bit builds.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { if (failures++ < 40) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

struct k_statx_ts { int64_t tv_sec; uint32_t tv_nsec; int32_t pad; };
struct k_statx {
    uint32_t mask, blksize; uint64_t attributes; uint32_t nlink, uid, gid; uint16_t mode, pad1;
    uint64_t ino, size, blocks, attributes_mask;
    struct k_statx_ts atime, btime, ctime, mtime;
    uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
    uint64_t spare[14];
};

static int no_statx;                    // AOK's i386 statx is ENOSYS: glibc falls back to fstatat64
static int64_t native_mtime(const char *path);
static int64_t native_atime(const char *path);
static int64_t statx_mtime(const char *path, int64_t *atime) {
    struct k_statx sx;
    memset(&sx, 0, sizeof sx);
    if (no_statx || syscall(SYS_statx, AT_FDCWD, path, 0, 0x7ff, &sx) != 0) {
        if (!no_statx && errno != ENOSYS)
            return INT64_MIN;
        no_statx = 1;                   // then stat64's low 32 bits, sign-extended
        *atime = (int32_t) native_atime(path);
        return (int32_t) native_mtime(path);
    }
    *atime = sx.atime.tv_sec;
    return sx.mtime.tv_sec;
}

#if __SIZEOF_LONG__ == 8
// newfstatat: struct stat's times are signed 64-bit
static int64_t native_mtime(const char *path) {
    struct stat st;
    if (syscall(SYS_newfstatat, AT_FDCWD, path, &st, 0) != 0)
        return INT64_MIN;
    return st.st_mtime;
}
static int64_t native_atime(const char *path) {
    struct stat st;
    if (syscall(SYS_newfstatat, AT_FDCWD, path, &st, 0) != 0)
        return INT64_MIN;
    return st.st_atime;
}
#define NATIVE_EXPECT(v) (v)
#else
// stat64: the kernel's struct stat64 has 32-bit times
struct k_stat64 {
    unsigned long long st_dev; unsigned char pad0[4]; unsigned long __st_ino;
    unsigned int st_mode, st_nlink; unsigned long st_uid, st_gid;
    unsigned long long st_rdev; unsigned char pad3[4]; long long st_size;
    unsigned long st_blksize; unsigned long long st_blocks;
    unsigned long atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;  // (st_atime is a libc macro)
    unsigned long long st_ino;
} __attribute__((packed));
static int64_t native_mtime(const char *path) {
    struct k_stat64 st;
    if (syscall(SYS_stat64, path, &st) != 0)
        return INT64_MIN;
    return (int64_t) (uint32_t) st.mtime;
}
static int64_t native_atime(const char *path) {
    struct k_stat64 st;
    if (syscall(SYS_stat64, path, &st) != 0)
        return INT64_MIN;
    return (int64_t) (uint32_t) st.atime;
}
#define NATIVE_EXPECT(v) ((int64_t) (uint32_t) (v))
#endif

static void try_value(const char *dir, int64_t v) {
    char path[256];
    snprintf(path, sizeof path, "%s/stat_time_range.%d", dir, (int) getpid());
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        CHECK(0, "%s: cannot create", path);
        return;
    }
    close(fd);
    int64_t at = 0, mt;
    // utimensat with a 64-bit time
#if __SIZEOF_LONG__ == 8
    struct { int64_t sec; long nsec; } ts[2] = {{v, 0}, {v, 0}};
    long r = syscall(SYS_utimensat, AT_FDCWD, path, ts, 0);
#else
    struct { int64_t sec; int64_t nsec; } ts[2] = {{v, 0}, {v, 0}};
    long r = syscall(412 /* utimensat_time64 */, AT_FDCWD, path, ts, 0);
#endif
    CHECK(r == 0, "%s: utimensat(%lld) failed", dir, (long long) v);
    mt = statx_mtime(path, &at);
    int64_t want = no_statx ? (int32_t) v : v;     // (stat64 holds the low 32 bits)
    CHECK(mt == want && at == want, "%s: utimensat(%lld): statx mtime %lld atime %lld", dir, (long long) v,
          (long long) mt, (long long) at);
    mt = native_mtime(path);
    CHECK(mt == NATIVE_EXPECT(v), "%s: utimensat(%lld): stat mtime %lld (want %lld)", dir, (long long) v,
          (long long) mt, (long long) NATIVE_EXPECT(v));

    if (v >= INT32_MIN && v <= INT32_MAX) {
        // utimes and utime with the ABI's own time_t (32-bit and signed on i386)
        // (the asm-generic ABIs, arm64 and riscv64, have neither)
#ifdef SYS_utimes
        struct { long sec, usec; } tv[2] = {{(long) v, 0}, {(long) v, 0}};
        r = syscall(SYS_utimes, path, tv);
        mt = statx_mtime(path, &at);
        CHECK(r == 0 && mt == v, "%s: utimes(%lld): statx mtime %lld", dir, (long long) v, (long long) mt);
#endif
#ifdef SYS_utime
        int64_t w = v > INT32_MIN ? v - 7 : v + 7;     // (a different value, still in range)
        struct { long actime, modtime; } ub = {(long) w, (long) w};
        r = syscall(SYS_utime, path, &ub);
        mt = statx_mtime(path, &at);
        CHECK(r == 0 && mt == w && at == w, "%s: utime(%lld): statx mtime %lld atime %lld", dir,
              (long long) w, (long long) mt, (long long) at);
#endif
#if __SIZEOF_LONG__ == 4
        struct { int32_t sec; int32_t nsec; } ts32[2] = {{(int32_t) v, 0}, {(int32_t) v, 0}};
        r = syscall(SYS_utimensat, AT_FDCWD, path, ts32, 0);
        mt = statx_mtime(path, &at);
        CHECK(r == 0 && mt == v, "%s: utimensat (32-bit time)(%lld): statx mtime %lld", dir, (long long) v,
              (long long) mt);
#endif
    }
    unlink(path);
}

int main(void) {
    static const int64_t values[] = {
        -1, -86400, -1000000000, INT32_MAX, INT32_MIN, 1ll << 32, 1ll << 33, 4102444800ll /* 2100 */,
        0, 1700000000,
        // (not beyond 2242: ext4 clamps at 2446 and APFS, under AOK's fakefs,
        // holds nanoseconds in 64 bits, so 2262 is its end)
    };
    const char *dirs[] = {"/tmp", "/dev/shm"};
    for (unsigned d = 0; d < 2; d++) {
        if (access(dirs[d], W_OK) != 0)
            continue;
        for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++)
            try_value(dirs[d], values[i]);
    }
    printf("stat_time_range: %s (%d checks, %d failures%s)\n", failures ? "FAIL" : "PASS", checks, failures,
           no_statx ? "; no statx: stat64 only" : "");
    return failures != 0;
}
