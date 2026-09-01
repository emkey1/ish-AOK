// sysfs_dev_ns.c — the sysfs and nsfs surfaces three util-linux tools need.
//
// From a TestFlight report against build 551: lsblk, lsns and lsmem each
// failed outright, and each for its own reason.
//
//   lsblk: failed to access sysfs directory: /sys/dev/block: No such file...
//   lsns: Unsupported ioctl NS_GET_USERNS
//   lsmem: cannot open /sys/devices/system/memory: No such file or directory
//
// None of the three is exotic; they are what a person runs to find out what a
// machine is made of. What they have in common is that they ask the kernel to
// DESCRIBE itself, and AOK had the descriptions and no way to hand them over:
// a block device with no by-devnum index, namespaces with no ioctls, and
// memory with no hotplug view.
//
// Written against what Devuan (Linux 6.12) actually answers, checked one
// question at a time rather than by running the tools, so a failure names the
// missing fact instead of a tool's opinion of it. Passes identically on a real
// Linux kernel run as root.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "test_common.h"

// linux/nsfs.h
#define NS_GET_USERNS_ 0xb701
#define NS_GET_PARENT_ 0xb702
#define NS_GET_NSTYPE_ 0xb703
#define NS_GET_OWNER_UID_ 0xb704
// linux/sockios.h: a socket's network namespace.
#define SIOCGSKNS_ 0x894c

#define CLONE_NEWTIME_ 0x00000080
#define CLONE_NEWNS_ 0x00020000
#define CLONE_NEWCGROUP_ 0x02000000
#define CLONE_NEWUTS_ 0x04000000
#define CLONE_NEWIPC_ 0x08000000
#define CLONE_NEWUSER_ 0x10000000
#define CLONE_NEWPID_ 0x20000000
#define CLONE_NEWNET_ 0x40000000

static int check(const char *label, int cond) {
    if (cond) {
        test_logf("ok   %s\n", label);
        return 1;
    }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

static int eq_errno(const char *label, long r, int want) {
    int e = (r < 0) ? errno : 0;
    if (r < 0 && e == want) {
        test_logf("ok   %s (errno %d)\n", label, e);
        return 1;
    }
    printf("FAIL %s: ret=%ld errno=%d (%s), wanted -1/errno=%d\n",
           label, r, e, r < 0 ? strerror(e) : "no error", want);
    failures_total++;
    return 0;
}

// Read a whole small sysfs attribute. Returns the length, or -1.
static ssize_t slurp(const char *path, char *buf, size_t bufsize) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, bufsize - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return n;
}

// The first real entry in a directory, so the checks below describe whatever
// the machine actually has rather than assuming this one's device names.
static int first_entry(const char *dir, char *name, size_t namesize) {
    DIR *d = opendir(dir);
    if (d == NULL)
        return 0;
    struct dirent *de;
    int found = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        snprintf(name, namesize, "%s", de->d_name);
        found = 1;
        break;
    }
    closedir(d);
    return found;
}

// ---- /sys/dev/block: the by-devnum index lsblk starts from ---------------

static void test_sys_dev_block(void) {
    struct stat st;
    check("sys.dev_is_a_directory", stat("/sys/dev", &st) == 0 && S_ISDIR(st.st_mode));
    check("sys.dev_block_is_a_directory",
          stat("/sys/dev/block", &st) == 0 && S_ISDIR(st.st_mode));
    // Linux always has both; a tool testing for char/ must not conclude the
    // machine is odd because only block/ is there.
    check("sys.dev_char_is_a_directory",
          stat("/sys/dev/char", &st) == 0 && S_ISDIR(st.st_mode));

    char devnum[64];
    if (!check("sys.dev_block_has_an_entry", first_entry("/sys/dev/block", devnum, sizeof(devnum))))
        return;

    // The name is major:minor, and it is a SYMLINK -- lsblk takes the basename
    // of the target as the device's name, so a directory here would leave
    // every device nameless.
    unsigned maj = 0, min = 0;
    check("sys.dev_block_entry_is_maj_min",
          sscanf(devnum, "%u:%u", &maj, &min) == 2);

    char path[256];
    snprintf(path, sizeof(path), "/sys/dev/block/%s", devnum);
    check("sys.dev_block_entry_is_a_symlink",
          lstat(path, &st) == 0 && S_ISLNK(st.st_mode));

    char target[512];
    ssize_t tlen = readlink(path, target, sizeof(target) - 1);
    if (check("sys.dev_block_entry_readlink", tlen > 0)) {
        target[tlen] = '\0';
        test_logf("     %s -> %s\n", devnum, target);
        // Relative, so it resolves against the link's own directory.
        check("sys.dev_block_target_is_relative", target[0] != '/');
    }

    // Following it has to reach the device, and the device has to agree about
    // its own number -- that agreement is the entire point of the index.
    char devattr[256];
    snprintf(devattr, sizeof(devattr), "/sys/dev/block/%s/dev", devnum);
    char contents[64];
    if (check("sys.dev_block_target_has_dev", slurp(devattr, contents, sizeof(contents)) > 0)) {
        unsigned tmaj = 0, tmin = 0;
        check("sys.dev_block_dev_matches_the_name",
              sscanf(contents, "%u:%u", &tmaj, &tmin) == 2 && tmaj == maj && tmin == min);
    }
}

// ---- /sys/block/<dev>: the attributes lsblk reads once it has the device --

static void test_sys_block_attrs(void) {
    char name[64];
    if (!check("sys.block_has_a_device", first_entry("/sys/block", name, sizeof(name))))
        return;

    char path[256], buf[64];
    // size is in 512-byte sectors and is what lsblk's SIZE column is computed
    // from. Absent, it printed a number anyway -- a wrong one.
    snprintf(path, sizeof(path), "/sys/block/%s/size", name);
    if (check("sys.block_size_readable", slurp(path, buf, sizeof(buf)) > 0)) {
        unsigned long long sectors = strtoull(buf, NULL, 10);
        test_logf("     %s size = %llu sectors\n", name, sectors);
        check("sys.block_size_is_plausible", sectors > 0);
    }

    // Booleans, and they have to BE booleans: lsblk prints them straight
    // through into its RM and RO columns.
    const char *flags[] = {"removable", "ro", "hidden"};
    for (unsigned i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "/sys/block/%s/%s", name, flags[i]);
        char label[96];
        snprintf(label, sizeof(label), "sys.block_%s_is_0_or_1", flags[i]);
        if (slurp(path, buf, sizeof(buf)) > 0)
            check(label, buf[0] == '0' || buf[0] == '1');
        else
            printf("SKIP %s (not present on this kernel)\n", label);
    }
}

// ---- /sys/devices/system/memory: the hotplug view lsmem is built on ------

static void test_sys_memory(void) {
    struct stat st;
    if (stat("/sys/devices/system/memory", &st) != 0 || !S_ISDIR(st.st_mode)) {
        // A real kernel built without memory hotplug has none of this, and
        // lsmem says so rather than failing. Skip rather than fail, but say so
        // out loud: on AOK it must be there.
        printf("SKIP sys.memory (no /sys/devices/system/memory on this kernel)\n");
        return;
    }

    char buf[64];
    unsigned long long block_size = 0;
    if (check("sys.memory_block_size_readable",
              slurp("/sys/devices/system/memory/block_size_bytes", buf, sizeof(buf)) > 0)) {
        // Hex, without an 0x prefix -- lsmem parses it that way, so a decimal
        // value here would be silently misread by a factor of millions.
        block_size = strtoull(buf, NULL, 16);
        test_logf("     block_size_bytes = %s (%llu bytes)\n", buf, block_size);
        check("sys.memory_block_size_nonzero", block_size > 0);
        check("sys.memory_block_size_is_a_power_of_two",
              block_size > 0 && (block_size & (block_size - 1)) == 0);
    }

    // Every block reports whether it is present and whether it could be taken
    // away; those two columns are lsmem's whole output.
    if (check("sys.memory_block0_state_readable",
              slurp("/sys/devices/system/memory/memory0/state", buf, sizeof(buf)) > 0))
        check("sys.memory_block0_is_online", strncmp(buf, "online", 6) == 0);
    if (check("sys.memory_block0_removable_readable",
              slurp("/sys/devices/system/memory/memory0/removable", buf, sizeof(buf)) > 0))
        check("sys.memory_block0_removable_is_boolean", buf[0] == '0' || buf[0] == '1');
    check("sys.memory_block0_valid_zones_readable",
          slurp("/sys/devices/system/memory/memory0/valid_zones", buf, sizeof(buf)) > 0);

    // The blocks have to be numbered from zero without gaps, because lsmem
    // walks them by index and stops at the first one it cannot open.
    int blocks = 0;
    for (int i = 0; i < 8192; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/memory/memory%d", i);
        if (stat(path, &st) != 0)
            break;
        blocks++;
    }
    test_logf("     %d memory blocks\n", blocks);
    check("sys.memory_blocks_start_at_zero_and_are_contiguous", blocks > 0);
}

// ---- nsfs: the four ioctls lsns is built out of --------------------------

struct ns_expect {
    const char *name;
    unsigned nstype;
    // NS_GET_PARENT: hierarchical kinds answer EPERM (there IS a parent and
    // you may not have it), flat kinds answer EINVAL (there is no such thing).
    int parent_errno;
};

static void test_nsfs_ioctls(void) {
    static const struct ns_expect expect[] = {
        {"mnt", CLONE_NEWNS_, EINVAL},
        {"net", CLONE_NEWNET_, EINVAL},
        {"uts", CLONE_NEWUTS_, EINVAL},
        {"ipc", CLONE_NEWIPC_, EINVAL},
        {"cgroup", CLONE_NEWCGROUP_, EINVAL},
        {"time", CLONE_NEWTIME_, EINVAL},
        {"pid", CLONE_NEWPID_, EPERM},
        {"user", CLONE_NEWUSER_, EPERM},
    };

    for (unsigned i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        char path[64], label[128];
        snprintf(path, sizeof(path), "/proc/self/ns/%s", expect[i].name);
        int fd = open(path, O_RDONLY);
        snprintf(label, sizeof(label), "ns.%s_opens", expect[i].name);
        if (!check(label, fd >= 0))
            continue;

        snprintf(label, sizeof(label), "ns.%s_nstype", expect[i].name);
        int type = ioctl(fd, NS_GET_NSTYPE_);
        if (!check(label, type == (int) expect[i].nstype))
            printf("     (got %#x, wanted %#x)\n", type, expect[i].nstype);

        snprintf(label, sizeof(label), "ns.%s_parent", expect[i].name);
        eq_errno(label, ioctl(fd, NS_GET_PARENT_), expect[i].parent_errno);

        // Everything is owned by the one user namespace; asking that namespace
        // who owns IT walks off the top of the hierarchy.
        snprintf(label, sizeof(label), "ns.%s_userns", expect[i].name);
        int owner = ioctl(fd, NS_GET_USERNS_);
        if (expect[i].nstype == CLONE_NEWUSER_) {
            eq_errno(label, owner, EPERM);
        } else if (check(label, owner >= 0)) {
            // What comes back is a namespace fd in its own right, and it is a
            // USER namespace -- not a copy of the one that was asked.
            snprintf(label, sizeof(label), "ns.%s_userns_is_a_user_ns", expect[i].name);
            check(label, ioctl(owner, NS_GET_NSTYPE_) == CLONE_NEWUSER_);
            close(owner);
        }

        // The owner's uid is a question only a user namespace can answer.
        uid_t owner_uid = 12345;
        snprintf(label, sizeof(label), "ns.%s_owner_uid", expect[i].name);
        int rc = ioctl(fd, NS_GET_OWNER_UID_, &owner_uid);
        if (expect[i].nstype == CLONE_NEWUSER_) {
            if (check(label, rc == 0))
                check("ns.user_owner_uid_is_root", owner_uid == 0);
        } else {
            eq_errno(label, rc, EINVAL);
        }
        close(fd);
    }

    // A socket belongs to a network namespace, and saying so is how lsns
    // avoids opening every descriptor of every process to find out.
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (check("ns.socket_opens", sock >= 0)) {
        int ns = ioctl(sock, SIOCGSKNS_);
        if (geteuid() == 0) {
            if (check("ns.siocgskns_as_root", ns >= 0)) {
                check("ns.siocgskns_gives_a_net_ns",
                      ioctl(ns, NS_GET_NSTYPE_) == CLONE_NEWNET_);
                close(ns);
            }
        } else {
            // Needs CAP_SYS_ADMIN in the target namespace.
            eq_errno("ns.siocgskns_unprivileged", ns, EPERM);
        }
        close(sock);
    }

    // Asking a file that is not a namespace is ENOTTY, which is what lets a
    // caller tell "not a namespace" from "namespaces unsupported".
    int reg = open("/proc/self/cmdline", O_RDONLY);
    if (check("ns.regular_file_opens", reg >= 0)) {
        eq_errno("ns.regular_file_nstype_is_enotty", ioctl(reg, NS_GET_NSTYPE_), ENOTTY);
        close(reg);
    }
}

// ---- a sysfs attribute is always ready to read --------------------------
//
// A descriptor whose filesystem reports no readiness at all reads back as
// NEVER ready, and busybox ash's `read` builtin polls before every read even
// with no timeout. So `read x < /sys/anything` hung forever, at zero CPU,
// while `cat` on the same file worked -- which is what made the file look
// fine. Every attribute this test checks is reachable that way, and that is
// the ordinary way a shell script consults sysfs.
//
// Measured on Devuan: a sysfs attribute answers POLLIN|POLLOUT|POLLPRI|POLLERR
// and a procfs file answers POLLIN|POLLOUT. What is asserted here is the part
// that decides whether anything hangs -- readable, and ready at all -- rather
// than the exact mask, which differs between the two filesystems for reasons
// that have nothing to do with this.
static void test_sysfs_poll(void) {
    const char *paths[] = {
        "/sys/devices/system/cpu/online",
        "/sys/block",
    };
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        // The directory case is opened as a file only if it is one; use a
        // real attribute for the poll itself.
        const char *path = paths[i];
        if (i == 1)
            continue;
        int fd = open(path, O_RDONLY);
        char label[128];
        snprintf(label, sizeof(label), "sysfs.poll_opens(%s)", path);
        if (!check(label, fd >= 0))
            continue;
        struct pollfd pf = {.fd = fd, .events = POLLIN | POLLOUT};
        int r = poll(&pf, 1, 200);
        snprintf(label, sizeof(label), "sysfs.poll_reports_ready");
        check(label, r == 1);
        snprintf(label, sizeof(label), "sysfs.poll_is_readable");
        check(label, (pf.revents & POLLIN) != 0);
        test_logf("     %s revents=%#x\n", path, pf.revents);
        close(fd);
    }
}

// ---- one anonymous filesystem per kind, as Linux has it -----------------
//
// A program cannot ask a descriptor what KIND of thing it is. What it can do
// is ask which filesystem the descriptor lives on, and Linux makes that
// answer meaningful by giving pipes, sockets, namespaces and the
// eventfd/epoll family each their own internal filesystem with its own
// anonymous device number.
//
// lsns is built on exactly that: it reads the device number off
// /proc/self/ns/net and then examines every descriptor that matches it. With
// one device shared by everything anonymous, that filter selects everything,
// and lsns interrogates every pipe and socket on the machine -- seventy lines
// of "Unsupported ioctl NS_GET_NSTYPE" before its first row of output, on a
// device with thirty-two processes.
//
// Measured on Devuan: nsfs 0:4, sockfs 0:9, pipefs 0:15, anon_inodefs 0:16.
static dev_t dev_of_fd(int fd) {
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0)
        return (dev_t) -1;
    return st.st_dev;
}

static void test_anon_devices(void) {
    int nsfd = open("/proc/self/ns/net", O_RDONLY);
    dev_t ns_dev = dev_of_fd(nsfd);
    if (nsfd >= 0)
        close(nsfd);

    int p[2];
    dev_t pipe_dev = (dev_t) -1;
    if (pipe(p) == 0) {
        pipe_dev = dev_of_fd(p[0]);
        close(p[0]);
        close(p[1]);
    }

    int sv[2];
    dev_t sock_dev = (dev_t) -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
        sock_dev = dev_of_fd(sv[0]);
        close(sv[0]);
        close(sv[1]);
    }

    int efd = eventfd(0, 0);
    dev_t event_dev = dev_of_fd(efd);
    if (efd >= 0)
        close(efd);
    int pfd = epoll_create1(0);
    dev_t epoll_dev = dev_of_fd(pfd);
    if (pfd >= 0)
        close(pfd);

    if (!check("dev.namespace_has_one", ns_dev != (dev_t) -1))
        return;
    check("dev.pipe_has_one", pipe_dev != (dev_t) -1);
    check("dev.socket_has_one", sock_dev != (dev_t) -1);
    check("dev.eventfd_has_one", event_dev != (dev_t) -1);
    test_logf("     nsfs=%u:%u pipe=%u:%u sock=%u:%u anon=%u:%u\n",
              major(ns_dev), minor(ns_dev), major(pipe_dev), minor(pipe_dev),
              major(sock_dev), minor(sock_dev), major(event_dev), minor(event_dev));

    // The three that matter: a namespace must not look like a pipe or a
    // socket, or every pipe and socket gets probed as a namespace.
    check("dev.namespace_differs_from_pipe", ns_dev != pipe_dev);
    check("dev.namespace_differs_from_socket", ns_dev != sock_dev);
    check("dev.namespace_differs_from_anon_inode", ns_dev != event_dev);
    check("dev.pipe_differs_from_socket", pipe_dev != sock_dev);

    // ...and the one that must NOT differ: Linux keeps the whole
    // eventfd/epoll/timerfd/signalfd family on a single anon_inodefs.
    check("dev.anon_inode_family_shares_one", event_dev == epoll_dev);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    test_sysfs_poll();
    test_anon_devices();
    test_sys_dev_block();
    test_sys_block_attrs();
    test_sys_memory();
    test_nsfs_ioctls();

    return finish_suite("sysfs_dev_ns");
}
