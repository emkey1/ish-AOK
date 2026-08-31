#include <sys/stat.h>
#include <arpa/inet.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#include <inttypes.h>
#include "kernel/calls.h"
#include "kernel/random.h"
#include "kernel/task.h"
#include "kernel/hostinfo.h"
#include "fs/proc.h"
#include "platform/platform.h"
#include <sys/utsname.h>

void get_current_hostname(char *hostname, size_t size);

#import <ifaddrs.h>
#import <netinet/in.h>
#import <sys/socket.h>
#import <unistd.h>
#if defined(__APPLE__)
#import <net/if_var.h>
#endif

extern const char *proc_ish_version;

#pragma mark - /proc/sys

static bool sys_show_abi(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}


static bool sys_show_dev(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool sys_show_fscache(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool sys_show_sunrpc(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool sys_show_user(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

// iSH doesn't actually enforce any of these (no real memory-pressure
// management, no routing/forwarding, no socket buffer accounting to speak
// of), so they're read-write scalars backed by plain storage rather than
// hooked up to real behavior. That's enough for software that only inspects
// or requires a minimum value (e.g. Elasticsearch/OpenSearch's bootstrap
// check on vm.max_map_count) rather than depending on the kernel enforcing it.
struct sys_scalar {
    const char *name;
    _Atomic long value;
    // The range Linux's proc_dointvec_minmax enforces for this knob. A write
    // outside it is EINVAL and leaves the stored value alone. Left as {0,0}
    // the knob takes any non-negative integer, which is what most of them do.
    long min;
    long max;
};

// Parse a sysctl write the way Linux does, into *out. Returns 0, or _EINVAL.
//
// There was no validation at all: strtol's return was stored whatever it had
// parsed, so "notanumber" became 0 and "-5" was kept verbatim -- and the write
// reported the full byte count either way. A program that sets a knob and
// reads it back to confirm got confirmation of a value the kernel had invented,
// which is worse than the write failing.
//
// Linux accepts leading whitespace and one optional sign, requires at least one
// digit, and allows trailing whitespace or a newline and nothing else.
static int proc_sys_scalar_parse(struct proc_data *data, long min, long max, long *out) {
    char tmp[64];
    size_t n = data->size < sizeof(tmp) - 1 ? data->size : sizeof(tmp) - 1;
    memcpy(tmp, data->data, n);
    tmp[n] = '\0';

    const char *p = tmp;
    while (*p == ' ' || *p == '\t')
        p++;
    const char *digits_start = p;
    if (*p == '-' || *p == '+')
        p++;
    if (*p < '0' || *p > '9')
        return _EINVAL;   // no digits at all: "notanumber", "", "-"

    errno = 0;
    char *end = NULL;
    long value = strtol(digits_start, &end, 10);
    if (errno == ERANGE)
        return _EINVAL;
    // Anything after the number that is not whitespace makes the whole write
    // invalid -- "12abc" is not 12.
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')
        end++;
    if (*end != '\0')
        return _EINVAL;

    if (max == 0 && min == 0) {
        // The default: non-negative. Every knob in these tables is a count, a
        // size or a boolean, and none of them is meaningful below zero.
        if (value < 0)
            return _EINVAL;
    } else if (value < min || value > max) {
        return _EINVAL;
    }
    *out = value;
    return 0;
}

// Shared body for the scalar tables below: validate, and only then store.
static int proc_sys_scalar_update(struct sys_scalar *scalar, struct proc_data *data) {
    long value;
    int err = proc_sys_scalar_parse(data, scalar->min, scalar->max, &value);
    if (err < 0)
        return err;
    atomic_store_explicit(&scalar->value, value, memory_order_relaxed);
    return 0;
}

static struct sys_scalar proc_sys_vm_scalars[] = {
    {"dirty_background_ratio", 10},
    {"dirty_ratio", 20},
    {"max_map_count", 262144},
    {"mmap_min_addr", 65536},
    {"overcommit_memory", 0},
    {"swappiness", 60},
};
#define PROC_SYS_VM_SCALARS_LEN (sizeof(proc_sys_vm_scalars) / sizeof(proc_sys_vm_scalars[0]))

static struct proc_dir_entry proc_sys_vm_entry;

static void proc_sys_vm_getname(struct proc_entry *entry, char *buf) {
    snprintf(buf, 256, "%s", proc_sys_vm_scalars[entry->fd].name);
}
static int proc_sys_vm_show(struct proc_entry *entry, struct proc_data *buf) {
    proc_printf(buf, "%ld\n", atomic_load_explicit(&proc_sys_vm_scalars[entry->fd].value, memory_order_relaxed));
    return 0;
}
static int proc_sys_vm_update(struct proc_entry *entry, struct proc_data *data) {
    return proc_sys_scalar_update(&proc_sys_vm_scalars[entry->fd], data);
}
static bool sys_show_vm(struct proc_entry *UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) {
    if (*index >= PROC_SYS_VM_SCALARS_LEN)
        return false;
    *next_entry = (struct proc_entry) {&proc_sys_vm_entry, .fd = (sdword_t) *index};
    (*index)++;
    return true;
}
static struct proc_dir_entry proc_sys_vm_entry = {NULL, S_IFREG | 0644,
    .getname = proc_sys_vm_getname, .show = proc_sys_vm_show, .update = proc_sys_vm_update};

// /proc/sys/fs/binfmt_misc: an EMPTY directory, which is exactly what Linux
// shows until the binfmt_misc filesystem is mounted on it.
//
// It used to present `register` and `status` unconditionally, with nothing
// behind them. `status` read "enabled" always; a `register` write returned the
// full byte count and was discarded, so no file for the format appeared and
// execve never consulted it; and writing 0 to `status` reported success and
// left it reading "enabled".
//
// That is the worst of the three possible answers. update-binfmts and
// systemd-binfmt both check `status`, register their formats, and believe the
// success they are handed -- so a guest configured to run, say, foreign
// binaries through an interpreter looked configured and then silently ran
// nothing. An empty directory is the truthful state and one those tools
// already handle: it is what every kernel without CONFIG_BINFMT_MISC, and
// every system that has not mounted it yet, looks like.
static bool proc_binfmt_misc_readdir(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index),
        struct proc_entry *UNUSED(next_entry)) {
    return false;
}

// Set once written, computed from memory otherwise: a value nothing enforces,
// but one `sysctl -p` can set without aborting the rest of the file.
static _Atomic long fs_file_max_override = 0;

static int sys_update_fs_file_max(struct proc_entry *UNUSED(entry), struct proc_data *data) {
    if (!superuser())
        return _EPERM;
    long value;
    int err = proc_sys_scalar_parse(data, 1, INT64_MAX, &value);
    if (err < 0)
        return err;
    atomic_store_explicit(&fs_file_max_override, value, memory_order_relaxed);
    return 0;
}

static int sys_show_fs_file_max(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    long override = atomic_load_explicit(&fs_file_max_override, memory_order_relaxed);
    if (override != 0) {
        proc_printf(buf, "%ld\n", override);
        return 0;
    }
    // Real kernels size this roughly proportional to installed memory; iSH
    // enforces no such ceiling, so this is a plausible value for software
    // that only inspects it rather than one derived from a real limit.
    struct mem_usage usage = get_mem_usage();
    uint64_t file_max = usage.total / 10240;
    if (file_max < 8192)
        file_max = 8192;
    proc_printf(buf, "%"PRIu64"\n", file_max);
    return 0;
}

// The ceiling RLIMIT_NOFILE may be raised to. Stored and reported; the fd table
// grows on demand rather than being preallocated, so there is no separate
// structure to resize when it changes.
static _Atomic long fs_nr_open = 1048576;

long fs_nr_open_value(void) {
    return atomic_load_explicit(&fs_nr_open, memory_order_relaxed);
}

static int sys_show_fs_nr_open(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%ld\n", fs_nr_open_value());
    return 0;
}

static int sys_update_fs_nr_open(struct proc_entry *UNUSED(entry), struct proc_data *data) {
    if (!superuser())
        return _EPERM;
    long value;
    int err = proc_sys_scalar_parse(data, 1, INT32_MAX, &value);
    if (err < 0)
        return err;
    atomic_store_explicit(&fs_nr_open, value, memory_order_relaxed);
    return 0;
}

static struct proc_dir_entry proc_sys_fs_entries[] = {
    {"binfmt_misc", S_IFDIR, .readdir = proc_binfmt_misc_readdir},
    {"file-max", S_IFREG | 0644, .show = sys_show_fs_file_max, .update = sys_update_fs_file_max},
    {"nr_open", S_IFREG | 0644, .show = sys_show_fs_nr_open, .update = sys_update_fs_nr_open},
};

#define PROC_SYS_FS_LEN sizeof(proc_sys_fs_entries) / sizeof(proc_sys_fs_entries[0])

static bool proc_sys_fs_readdir(struct proc_entry *UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) {
    if (*index < PROC_SYS_FS_LEN) {
        *next_entry = (struct proc_entry) {&proc_sys_fs_entries[*index], *index, NULL, NULL, 0, 0, NULL};
        (*index)++;
        return true;
    }
    return false;
}

static struct sys_scalar proc_sys_net_core_scalars[] = {
    {"rmem_max", 212992},
    {"somaxconn", 128},
    {"wmem_max", 212992},
};
#define PROC_SYS_NET_CORE_SCALARS_LEN (sizeof(proc_sys_net_core_scalars) / sizeof(proc_sys_net_core_scalars[0]))

static struct proc_dir_entry proc_sys_net_core_entry;

static void proc_sys_net_core_getname(struct proc_entry *entry, char *buf) {
    snprintf(buf, 256, "%s", proc_sys_net_core_scalars[entry->fd].name);
}
static int proc_sys_net_core_show(struct proc_entry *entry, struct proc_data *buf) {
    proc_printf(buf, "%ld\n", atomic_load_explicit(&proc_sys_net_core_scalars[entry->fd].value, memory_order_relaxed));
    return 0;
}
static int proc_sys_net_core_update(struct proc_entry *entry, struct proc_data *data) {
    return proc_sys_scalar_update(&proc_sys_net_core_scalars[entry->fd], data);
}
static bool sys_show_net_core(struct proc_entry *UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) {
    if (*index >= PROC_SYS_NET_CORE_SCALARS_LEN)
        return false;
    *next_entry = (struct proc_entry) {&proc_sys_net_core_entry, .fd = (sdword_t) *index};
    (*index)++;
    return true;
}
static struct proc_dir_entry proc_sys_net_core_entry = {NULL, S_IFREG | 0644,
    .getname = proc_sys_net_core_getname, .show = proc_sys_net_core_show, .update = proc_sys_net_core_update};

static struct sys_scalar proc_sys_net_ipv4_scalars[] = {
    {"ip_default_ttl", 64},
    {"ip_forward", 0},
    {"tcp_syncookies", 1},
};
#define PROC_SYS_NET_IPV4_SCALARS_LEN (sizeof(proc_sys_net_ipv4_scalars) / sizeof(proc_sys_net_ipv4_scalars[0]))

static struct proc_dir_entry proc_sys_net_ipv4_entry;

static void proc_sys_net_ipv4_getname(struct proc_entry *entry, char *buf) {
    snprintf(buf, 256, "%s", proc_sys_net_ipv4_scalars[entry->fd].name);
}
static int proc_sys_net_ipv4_show(struct proc_entry *entry, struct proc_data *buf) {
    proc_printf(buf, "%ld\n", atomic_load_explicit(&proc_sys_net_ipv4_scalars[entry->fd].value, memory_order_relaxed));
    return 0;
}
static int proc_sys_net_ipv4_update(struct proc_entry *entry, struct proc_data *data) {
    return proc_sys_scalar_update(&proc_sys_net_ipv4_scalars[entry->fd], data);
}
static bool sys_show_net_ipv4(struct proc_entry *UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) {
    if (*index >= PROC_SYS_NET_IPV4_SCALARS_LEN)
        return false;
    *next_entry = (struct proc_entry) {&proc_sys_net_ipv4_entry, .fd = (sdword_t) *index};
    (*index)++;
    return true;
}
static struct proc_dir_entry proc_sys_net_ipv4_entry = {NULL, S_IFREG | 0644,
    .getname = proc_sys_net_ipv4_getname, .show = proc_sys_net_ipv4_show, .update = proc_sys_net_ipv4_update};

static bool sys_show_net_ipv6(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool sys_show_net_netfilter(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static bool sys_show_net_unix(struct proc_entry *UNUSED(entry), unsigned long *UNUSED(index), struct proc_entry *UNUSED(next_entry)) {
    return 0;
}

static int sys_show_net_debug_exception_trace(struct proc_entry * UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%d\n", 0);
    
    return 0;
}

struct proc_dir_entry proc_sys_debug[] = {
    {"exception-trace", .show = sys_show_net_debug_exception_trace},
};

#define PROC_SYS_DEBUG_LEN sizeof(proc_sys_debug)/sizeof(proc_sys_debug[0])

static bool proc_sys_debug_readdir(struct proc_entry *UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) {
    if (*index < PROC_SYS_DEBUG_LEN) {
        *next_entry = (struct proc_entry) {&proc_sys_debug[*index], *index, NULL, NULL, 0, 0, NULL};
        (*index)++;
        return true;
    }
    
    return false;
}

static int sys_show_net_unix_hostname(struct proc_entry * UNUSED(entry), struct proc_data *buf) {
    struct uname uts;
    char hostname[sizeof(uts.hostname)];
    get_current_hostname(hostname, sizeof(hostname));
    proc_printf(buf, "%s\n", hostname);
    return 0;
}

// Writing /proc/sys/kernel/hostname sets the hostname, exactly as
// sethostname(2) does -- which is how `hostname foo` works on a system whose
// hostname(1) writes the file rather than making the syscall. The file was
// mode 0444 with no update handler at all, so those simply failed.
static int sys_update_kernel_hostname(struct proc_entry *UNUSED(entry), struct proc_data *data) {
    if (!superuser())
        return _EPERM;
    struct uname uts;
    size_t len = data->size;
    // A trailing newline is what `echo foo > hostname` writes; it is not part
    // of the name.
    while (len > 0 && (data->data[len - 1] == '\n' || data->data[len - 1] == '\r'))
        len--;
    if (len >= sizeof(uts.hostname))
        return _EINVAL;
    char new_hostname[sizeof(uts.hostname)];
    memcpy(new_hostname, data->data, len);
    new_hostname[len] = '\0';
    struct uts_namespace *ns = uts_ns_current();
    lock(&ns->lock, 0);
    memcpy(ns->hostname, new_hostname, len + 1);
    unlock(&ns->lock);
    return 0;
}

static int sys_show_net_version(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%s\n", proc_ish_version);
    return 0;
}

static int sys_show_kernel_osrelease(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct uname uts;
    do_uname(&uts);
    proc_printf(buf, "%s\n", uts.release);
    return 0;
}

static int sys_show_kernel_cap_last_cap(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    // Keep this aligned with the advertised 4.20 kernel release.
    proc_printf(buf, "%d\n", 37);
    return 0;
}

static int sys_show_kernel_random_poolsize(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%d\n", RANDOM_POOL_BITS);
    return 0;
}

static int sys_show_kernel_random_entropy_avail(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    // Randomness is host-backed, so the pool always reads as full.
    proc_printf(buf, "%d\n", RANDOM_POOL_BITS);
    return 0;
}

// RFC 4122 v4 (random) UUID, formatted as the canonical 8-4-4-4-12 hex string
// real Linux reports from these two files. Missing entirely was a real gap:
// bash >= 5.1's $RANDOM seeding and various distro profile/rc scripts (seen
// live on Arch Linux ARM's default bash) read /proc/sys/kernel/random/{uuid,
// boot_id} unconditionally and print "No such file or directory" to stderr
// on every new shell when it's absent.
static void format_random_uuid(struct proc_data *buf, const unsigned char *b) {
    proc_printf(buf, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
            b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static int sys_show_kernel_random_uuid(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    // A fresh random UUID on every read, matching real Linux.
    unsigned char b[16];
    get_random((char *) b, sizeof(b));
    b[6] = (b[6] & 0x0f) | 0x40; // version 4
    b[8] = (b[8] & 0x3f) | 0x80; // RFC 4122 variant
    format_random_uuid(buf, b);
    return 0;
}

// A single UUID generated once per iSH launch and reused for every read,
// matching real Linux's "constant for this boot, changes across reboots"
// contract (systemd and friends use it to detect a reboot happened).
static bool boot_id_generated = false;
static unsigned char boot_id[16];

static int sys_show_kernel_random_boot_id(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    if (!boot_id_generated) {
        get_random((char *) boot_id, sizeof(boot_id));
        boot_id[6] = (boot_id[6] & 0x0f) | 0x40;
        boot_id[8] = (boot_id[8] & 0x3f) | 0x80;
        boot_id_generated = true;
    }
    format_random_uuid(buf, boot_id);
    return 0;
}

struct proc_dir_entry proc_sys_kernel_random[] = {
    {"boot_id", .show = sys_show_kernel_random_boot_id},
    {"entropy_avail", .show = sys_show_kernel_random_entropy_avail},
    {"poolsize", .show = sys_show_kernel_random_poolsize},
    {"uuid", .show = sys_show_kernel_random_uuid},
};

#define PROC_SYS_KERNEL_RANDOM_LEN sizeof(proc_sys_kernel_random)/sizeof(proc_sys_kernel_random[0])

static bool proc_sys_kernel_random_readdir(struct proc_entry *UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) {
    if (*index < PROC_SYS_KERNEL_RANDOM_LEN) {
        *next_entry = (struct proc_entry) {&proc_sys_kernel_random[*index], *index, NULL, NULL, 0, 0, NULL};
        (*index)++;
        return true;
    }

    return false;
}

// These four advertised mode 0644 and then refused every write with EPERM,
// even for root, because they had no .update at all. `sysctl -w
// kernel.pid_max=...` failed, and worse, `sysctl -p` aborts at the first
// failing key -- so one unwritable knob in /etc/sysctl.conf silently dropped
// every setting after it. A file that says it is writable has to either take
// the write or say why.
static int sys_show_kernel_pid_max(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%u\n", task_pid_max());
    return 0;
}

static int sys_update_kernel_pid_max(struct proc_entry *UNUSED(entry), struct proc_data *data) {
    if (!superuser())
        return _EPERM;
    long value;
    // The table is sized at compile time, so MAX_PID is a hard ceiling; Linux's
    // own floor is 301. Out of range is EINVAL, which is what Linux answers too
    // -- it is not silently clamped.
    int err = proc_sys_scalar_parse(data, 301, MAX_PID, &value);
    if (err < 0)
        return err;
    return task_set_pid_max((dword_t) value);
}

// AOK has no thread-count cap distinct from the pid space, so this reports and
// accepts a value without a separate limit behind it. Accepting the write is
// still the right answer: the alternative is failing `sysctl -p` over a knob
// whose only common use is being raised, and a raised ceiling AOK does not
// enforce refuses nothing that would otherwise have worked.
static _Atomic long kernel_threads_max = MAX_PID;

static int sys_show_kernel_threads_max(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "%ld\n", atomic_load_explicit(&kernel_threads_max, memory_order_relaxed));
    return 0;
}

static int sys_update_kernel_threads_max(struct proc_entry *UNUSED(entry), struct proc_data *data) {
    if (!superuser())
        return _EPERM;
    long value;
    int err = proc_sys_scalar_parse(data, 1, INT32_MAX, &value);
    if (err < 0)
        return err;
    atomic_store_explicit(&kernel_threads_max, value, memory_order_relaxed);
    return 0;
}

static int sys_show_kernel_ngroups_max(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    // The real ceiling setgroups() enforces (kernel/task.h), not a number
    // chosen to look like Linux's. glibc reads this file for
    // sysconf(_SC_NGROUPS_MAX); musl answers from its own constant and never
    // asks, which is why the guest may still report 32.
    proc_printf(buf, "%d\n", MAX_GROUPS);
    return 0;
}

struct proc_dir_entry proc_sys_kernel[] = {
    {"cap_last_cap", .show = sys_show_kernel_cap_last_cap},
    {"hostname", S_IFREG | 0644, .show = sys_show_net_unix_hostname, .update = sys_update_kernel_hostname},
    {"ngroups_max", .show = sys_show_kernel_ngroups_max},
    {"osrelease", .show = sys_show_kernel_osrelease},
    {"pid_max", S_IFREG | 0644, .show = sys_show_kernel_pid_max, .update = sys_update_kernel_pid_max},
    {"random", S_IFDIR, .readdir = proc_sys_kernel_random_readdir},
    {"threads-max", S_IFREG | 0644, .show = sys_show_kernel_threads_max, .update = sys_update_kernel_threads_max},
    {"version", .show = sys_show_net_version},
};

#define PROC_SYS_KERNEL_LEN sizeof(proc_sys_kernel)/sizeof(proc_sys_kernel[0])

static bool proc_sys_kernel_readdir(struct proc_entry *UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) {
    if (*index < PROC_SYS_KERNEL_LEN) {
        *next_entry = (struct proc_entry) {&proc_sys_kernel[*index], *index, NULL, NULL, 0, 0, NULL};
        (*index)++;
        return true;
    }
    
    return false;
}

struct proc_dir_entry proc_sys_net[] = {
    {"core", S_IFDIR, .readdir = sys_show_net_core},
    {"ipv4", S_IFDIR, .readdir = sys_show_net_ipv4},
    {"ipv6", S_IFDIR, .readdir = sys_show_net_ipv6},
    {"netfilter", S_IFDIR, .readdir = sys_show_net_netfilter},
    {"unix", S_IFDIR, .readdir = sys_show_net_unix},
};

#define PROC_SYS_NET_LEN sizeof(proc_sys_net)/sizeof(proc_sys_net[0])

static bool proc_sys_net_readdir(struct proc_entry *UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) {
    if (*index < PROC_SYS_NET_LEN) {
        *next_entry = (struct proc_entry) {&proc_sys_net[*index], *index, NULL, NULL, 0, 0, NULL};
        (*index)++;
        return true;
    }
    
    return false;
}

struct proc_dir_entry proc_net = {NULL, S_IFDIR, .readdir = proc_sys_net_readdir};

struct proc_children proc_sys_children = PROC_CHILDREN({
    {"abi", S_IFDIR, .readdir = sys_show_abi},
    {"debug", S_IFDIR, .readdir = proc_sys_debug_readdir},
    {"dev", S_IFDIR, .readdir = sys_show_dev},
    {"fs", S_IFDIR, .readdir = proc_sys_fs_readdir},
    {"fscache", S_IFDIR, .readdir = sys_show_fscache},
    {"kernel", S_IFDIR, .readdir = &proc_sys_kernel_readdir},
    {"net", S_IFDIR, .readdir = &proc_sys_net_readdir},
    {"sunrpc", S_IFDIR, .readdir = sys_show_sunrpc},
    {"user", S_IFDIR, .readdir = sys_show_user},
    {"vm", S_IFDIR, .readdir = sys_show_vm},
   //{"dev", .show = proc_show_dev},
});

void proc_sys_init(struct proc_dir_entry *root_entry) {
    struct proc_dir_entry *debug_dir;
    struct proc_dir_entry *fs_dir;
    struct proc_dir_entry *kernel_dir;
    struct proc_dir_entry *net_dir;
    struct proc_dir_entry *vm_dir;

    if (root_entry == NULL)
        return;

    proc_set_children_parent(&proc_sys_children, root_entry);

    debug_dir = proc_children_find(&proc_sys_children, "debug");
    if (debug_dir != NULL)
        proc_set_entries_parent(proc_sys_debug, PROC_SYS_DEBUG_LEN, debug_dir);

    fs_dir = proc_children_find(&proc_sys_children, "fs");
    if (fs_dir != NULL)
        proc_set_entries_parent(proc_sys_fs_entries, PROC_SYS_FS_LEN, fs_dir);

    kernel_dir = proc_children_find(&proc_sys_children, "kernel");
    if (kernel_dir != NULL) {
        proc_set_entries_parent(proc_sys_kernel, PROC_SYS_KERNEL_LEN, kernel_dir);
        proc_set_entries_parent(proc_sys_kernel_random, PROC_SYS_KERNEL_RANDOM_LEN,
                proc_find_entry(proc_sys_kernel, PROC_SYS_KERNEL_LEN, "random"));
    }

    vm_dir = proc_children_find(&proc_sys_children, "vm");
    if (vm_dir != NULL)
        proc_sys_vm_entry.parent = vm_dir;

    net_dir = proc_children_find(&proc_sys_children, "net");
    if (net_dir != NULL) {
        proc_set_entries_parent(proc_sys_net, PROC_SYS_NET_LEN, net_dir);
        proc_net.parent = net_dir;
        proc_sys_net_core_entry.parent = proc_find_entry(proc_sys_net, PROC_SYS_NET_LEN, "core");
        proc_sys_net_ipv4_entry.parent = proc_find_entry(proc_sys_net, PROC_SYS_NET_LEN, "ipv4");
    }
}
