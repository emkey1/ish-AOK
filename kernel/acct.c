#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>

#include "kernel/acct.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/resource.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/tty.h"
#include "util/sync.h"

// AHZ is the tick the v3 record counts in, and it is 100 REGARDLESS of the
// guest's USER_HZ: kernel/acct.c hardcodes `#define AHZ 100` in its v3 branch.
// Getting this wrong is not a rounding error, it is a factor -- a `sleep 2`
// would be recorded as 2 rather than 200.
#define ACCT_AHZ 100

#define ACCT_COMM_ 16
#define ACCT_VERSION_V3_ 3

#define AFORK_ 0x01   // forked but never exec'd
#define ASU_   0x02   // used superuser privileges
#define ACORE_ 0x08   // dumped core
#define AXSIG_ 0x10   // killed by a signal

struct acct_v3_ {
    uint8_t  ac_flag;
    uint8_t  ac_version;
    uint16_t ac_tty;
    uint32_t ac_exitcode;
    uint32_t ac_uid;
    uint32_t ac_gid;
    uint32_t ac_pid;
    uint32_t ac_ppid;
    uint32_t ac_btime;
    uint32_t ac_etime;   // an IEEE float, byte-for-byte; see acct_encode_float
    uint16_t ac_utime;
    uint16_t ac_stime;
    uint16_t ac_mem;
    uint16_t ac_io;
    uint16_t ac_rw;
    uint16_t ac_minflt;
    uint16_t ac_majflt;
    uint16_t ac_swaps;
    char     ac_comm[ACCT_COMM_];
};
static_assert(sizeof(struct acct_v3_) == 64, "acct_v3 is 64 bytes on the wire");

// The file, and a flag that can be read without it.
//
// The flag exists so the exit path costs one relaxed load on a system that has
// never called acct(2), which is almost all of them. Taking acct_lock on every
// process exit would put a process-wide lock in the exit path for a feature
// nobody has turned on.
static lock_t acct_lock = LOCK_INITIALIZER;
static struct fd *acct_file = NULL;
static atomic_bool acct_on = false;

bool acct_is_on(void) {
    return atomic_load_explicit(&acct_on, memory_order_relaxed);
}

// comp_t: 3-bit exponent, 13-bit mantissa, base 8. Transcribed from
// kernel/acct.c's encode_comp_t, including the rounding step -- a value that
// rounds up out of the mantissa has to carry into the exponent.
static uint16_t acct_encode_comp_t(uint64_t value) {
    static const int mantsize = 13, expsize = 3;
    static const uint64_t maxfract = (1u << 13) - 1;
    static const int maxexp = (1 << 3) - 1;
    int exp = 0, rnd = 0;

    while (value > maxfract) {
        rnd = (int) (value & (1 << (expsize - 1)));
        value >>= expsize;
        exp++;
    }
    if (rnd && (++value > maxfract)) {
        value >>= expsize;
        exp++;
    }
    if (exp > maxexp)
        return (uint16_t) maxfract;   // saturate, as Linux does
    return (uint16_t) ((value & maxfract) | ((uint64_t) exp << mantsize));
}

// ac_etime is an IEEE single holding a count of AHZ ticks. Linux builds the
// bit pattern by hand because it will not do floating point in the kernel;
// AOK is ordinary userspace and a cast is the same number with none of the
// transcription risk -- a hand-rolled version of this was wrong by seventeen
// orders of magnitude on its first run.
static uint32_t acct_encode_float(uint64_t value) {
    float f = (float) value;
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

static uint64_t acct_timeval_to_ahz(const struct timeval_ *tv) {
    return (uint64_t) tv->sec * ACCT_AHZ +
           (uint64_t) tv->usec / (1000000 / ACCT_AHZ);
}

static dword_t acct_common(guest_addr_t path_addr) {
    if (!current_capable(CAP_SYS_PACCT_))
        return _EPERM;

    if (path_addr == 0) {
        STRACE("acct(NULL)");
        lock(&acct_lock, 0);
        struct fd *old = acct_file;
        acct_file = NULL;
        atomic_store_explicit(&acct_on, false, memory_order_release);
        unlock(&acct_lock);
        // Closed outside the lock: fd_close does filesystem work, and holding
        // a lock across that is the rule this file already has to respect on
        // the write side.
        if (old != NULL)
            fd_close(old);
        return 0;
    }

    char path[MAX_PATH];
    int path_err = user_read_path(path_addr, path, sizeof(path));
    if (path_err)
        return path_err;
    STRACE("acct(\"%s\")", path);

    struct fd *fd = generic_open(path, O_WRONLY_ | O_APPEND_, 0);
    if (IS_ERR(fd))
        return (dword_t) PTR_ERR(fd);

    // Linux refuses anything that is not a regular file with EACCES: records
    // are appended blindly for the life of the system, and a fifo would block
    // an exiting process on a reader that may never come.
    struct statbuf stat = {};
    int err = fd->mount->fs->fstat(fd, &stat);
    if (err < 0) {
        fd_close(fd);
        return (dword_t) err;
    }
    if (!S_ISREG(stat.mode)) {
        fd_close(fd);
        return _EACCES;
    }

    lock(&acct_lock, 0);
    struct fd *old = acct_file;
    acct_file = fd;
    atomic_store_explicit(&acct_on, true, memory_order_release);
    unlock(&acct_lock);
    if (old != NULL)
        fd_close(old);
    return 0;
}

bool acct_collect(struct task *leader, const struct rusage_ *group_rusage,
                  dword_t status, struct acct_record *out) {
    if (!acct_is_on() || leader == NULL || group_rusage == NULL || out == NULL)
        return false;

    struct acct_v3_ rec = {};
    rec.ac_version = ACCT_VERSION_V3_;

    // Elapsed time in AHZ ticks. start_time_ticks is UPTIME in USER_HZ (100 Hz,
    // kernel/task.c's guest_uptime_ticks), which is already the unit this field
    // wants -- subtracting it from a wall-clock reading instead, as the first
    // version did, recorded 3.75e17 for a process that lived a few hundredths
    // of a second.
    uint64_t now_ticks = guest_uptime_ticks();
    uint64_t start_ticks = leader->start_time_ticks;
    uint64_t elapsed = now_ticks > start_ticks ? now_ticks - start_ticks : 0;
    rec.ac_etime = acct_encode_float(elapsed);
    // Wall clock through guest_clock_now, not timespec_now: this is a
    // guest-visible absolute time, and that is the rule for every one of them.
    struct timespec now = guest_clock_now(CLOCK_REALTIME_, CLOCK_REALTIME);
    rec.ac_btime = (uint32_t) (now.tv_sec - (long) (elapsed / ACCT_AHZ));

    rec.ac_exitcode = status;
    rec.ac_pid = leader->pid;
    // 0 rather than 1 when the parent is gone: a reparented process has no
    // honest ppid to report by the time its record is written.
    rec.ac_ppid = leader->parent != NULL ? (uint32_t) leader->parent->pid : 0;
    rec.ac_uid = leader->uid;
    rec.ac_gid = leader->gid;

    rec.ac_utime = acct_encode_comp_t(acct_timeval_to_ahz(&group_rusage->utime));
    rec.ac_stime = acct_encode_comp_t(acct_timeval_to_ahz(&group_rusage->stime));
    // Linux reports vsize/1024 at exit. AOK reports PEAK RSS, which rusage has
    // already measured -- deliberately, because the virtual size would mean
    // walking the address space on every process exit, and this field is
    // advisory. Same units (KB), different (arguably more useful) quantity.
    rec.ac_mem = acct_encode_comp_t(group_rusage->maxrss);
    rec.ac_minflt = acct_encode_comp_t(group_rusage->minflt);
    rec.ac_majflt = acct_encode_comp_t(group_rusage->majflt);
    // Zero, and so are Linux's: ac_io/ac_rw have not been filled in since the
    // BSD-era block accounting was removed, and ac_swaps never was.
    rec.ac_io = acct_encode_comp_t(0);
    rec.ac_rw = acct_encode_comp_t(0);
    rec.ac_swaps = acct_encode_comp_t(0);

    if (!leader->did_exec)
        rec.ac_flag |= AFORK_;
    if ((status & 0x7f) != 0)
        rec.ac_flag |= AXSIG_;
    // ASU and ACORE are left clear. They mean "actually exercised a privilege"
    // (Linux's PF_SUPERPRIV) and "dumped core", neither of which AOK tracks --
    // and euid 0 is NOT the same question, as a root `sleep` on Linux records
    // ac_flag 0.

    // No locks for either of these. The caller reaches here only once the
    // group is dead, so there is no surviving thread of this process to change
    // its comm or hand its terminal on; and do_exit already holds the dying
    // task's general_lock, so asking for it again would deadlock outright.
    struct tty *tty = leader->group->tty;
    // old_encode_dev: the 16-bit dev_t these records have always carried.
    rec.ac_tty = tty != NULL ? (uint16_t) ((tty->driver->major << 8) | tty->num) : 0;
    strncpy(rec.ac_comm, leader->comm, sizeof(rec.ac_comm));

    memcpy(out->bytes, &rec, sizeof(rec));
    return true;
}

dword_t sys_acct(addr_t path_addr) {
    return acct_common(path_addr);
}

dword_t sys_acct_guest(guest_addr_t path_addr) {
    return acct_common(path_addr);
}

void acct_write(const struct acct_record *rec) {
    lock(&acct_lock, 0);
    struct fd *fd = acct_file;
    if (fd != NULL)
        fd_retain(fd);
    unlock(&acct_lock);
    if (fd == NULL)
        return;   // turned off between the collect and here

    // A short or failed write is dropped rather than retried: this runs inside
    // a process's exit and has nothing to report an error to. A full disk
    // silently stops the accounting file growing, which is what Linux does too.
    fd->ops->write(fd, rec->bytes, sizeof(rec->bytes));
    fd_close(fd);
}
