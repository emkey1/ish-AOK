#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "fs/devices.h"
#include "fs/fd.h"
#include "fs/real.h"
#include "fs/tty.h"
#include "kernel/calls.h"
#include "kernel/init.h"
#include "kernel/personality.h"
#include "kernel/signal.h"
#include "kernel/task.h"

int mount_root(const struct fs_ops *fs, const char *source) {
    char source_realpath[MAX_PATH + 1];
    if (realpath(source, source_realpath) == NULL)
        return errno_map();
    int err = do_mount(fs, source_realpath, "", "", 0);
    if (err < 0)
        return err;
    // What / reports as its source, in /proc/mounts, /proc/self/mountinfo and
    // therefore df's "Filesystem" column.
    //
    // NOT the host path this was mounted from: that is long, unusable from
    // inside the guest, and on device carries the app group UUID. It used to be
    // the root's directory name ("Devuan6-arm64"), which read well in df but
    // named something no other file in the guest had ever heard of. Nothing
    // tied a mount to a device: /proc/diskstats advertised one device, btop
    // matched mounts to diskstats entries by name, and no name in one file
    // appeared in the other, so btop's disk and io panels listed nothing at all.
    //
    // So it is the block device the root is on, spelled the way Linux spells
    // it. AOK aggregates every real read and write into one device (see
    // fs/real.h) and has always printed major 8, minor 0 for it -- which IS
    // "sda" in Linux's numbering, whatever the name beside it said. Now
    // /proc/mounts, /proc/diskstats and /sys/block agree on it.
    //
    // The host path stays in mount->source for fakefs; only the label changes.
    // Ignore failure: the mount succeeded, only its label is at stake.
    mount_set_display_source("", "/dev/" GUEST_DISK_NAME);
    return 0;
}

// Declare the root in /etc/fstab, if nothing there declares it already.
//
// A rootfs tarball has never heard of AOK's root: Alpine's ships an fstab whose
// only entries are `noauto` lines for a CD-ROM and a USB stick, and Devuan's and
// Arch's are no better. Every real Linux system's fstab names its root
// filesystem, and things read it expecting that -- btop takes its ENTIRE disk
// list from fstab by default (use_fstab), so its disk and io panels were empty
// on every AOK guest, showing nothing at all rather than showing the root.
// (Measured: with use_fstab off the same btop fills the panel in; with it on,
// which is the default nobody changes, it stays blank.)
//
// The same reasoning as the /dev repair the CLI and the app each do at boot,
// and the same conservatism: this only ever ADDS a line, and only when no entry
// for "/" exists at all. A user who has written their own root entry keeps it,
// and running twice changes nothing.
void ensure_root_fstab_entry(void) {
    static const char entry[] =
        "# Added by iSH-AOK: the root, which the rootfs image did not describe.\n"
        "/dev/" GUEST_DISK_NAME "\t/\tfake\trw\t0 0\n";

    struct fd *fd = generic_open("/etc/fstab", O_RDWR_ | O_CREAT_, 0644);
    if (IS_ERR(fd))
        return;   // no /etc, read-only root, whatever -- not worth a boot failure

    // Small file by nature; a root entry past 64 KiB is not a case to serve.
    char buf[65536];
    ssize_t n = fd->ops->read(fd, buf, sizeof(buf) - 1);
    if (n < 0)
        n = 0;
    buf[n] = '\0';

    bool have_root = false;
    for (char *line = buf; line != NULL && *line != '\0'; ) {
        char *end = strchr(line, '\n');
        if (end != NULL)
            *end = '\0';
        // <device> <mountpoint> ...; the mountpoint is the second field.
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '#' && *p != '\0') {
            while (*p != '\0' && *p != ' ' && *p != '\t')
                p++;                              // past the device
            while (*p == ' ' || *p == '\t')
                p++;
            if (p[0] == '/' && (p[1] == '\0' || p[1] == ' ' || p[1] == '\t'))
                have_root = true;
        }
        line = end != NULL ? end + 1 : NULL;
    }

    if (!have_root) {
        // A file not ending in a newline would otherwise splice onto our line.
        off_t_ at = n;
        if (n > 0 && buf[n - 1] != '\n') {
            fd->ops->lseek(fd, at, LSEEK_SET);
            fd->ops->write(fd, "\n", 1);
            at++;
        }
        fd->ops->lseek(fd, at, LSEEK_SET);
        fd->ops->write(fd, entry, sizeof(entry) - 1);
    }
    fd_close(fd);
}

static void establish_signal_handlers(void) {
    extern void sigusr1_handler(int sig);
    struct sigaction sigact;
    sigact.sa_handler = sigusr1_handler;
    sigact.sa_flags = 0;
    sigemptyset(&sigact.sa_mask);
    sigaddset(&sigact.sa_mask, SIGUSR1);
    sigaction(SIGUSR1, &sigact, NULL);

    // The backup wake poke (util/sync.c). Without a handler SIGUSR2's default
    // action is to kill the whole emulator, so this must be installed before
    // signal_wake_task can ever send one.
    extern void sigusr2_handler(int sig);
    struct sigaction usr2act;
    usr2act.sa_handler = sigusr2_handler;
    usr2act.sa_flags = 0;
    sigemptyset(&usr2act.sa_mask);
    sigaddset(&usr2act.sa_mask, SIGUSR2);
    sigaction(SIGUSR2, &usr2act, NULL);

    signal(SIGPIPE, SIG_IGN);
}

// copied from include/asm-generic/resource.h in the kernel
static struct rlimit_ init_rlimits[16] = {
    [RLIMIT_CPU_]        = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_FSIZE_]      = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_DATA_]       = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_STACK_]      = {8*1024*1024, RLIM_INFINITY_},
    [RLIMIT_CORE_]       = {0, RLIM_INFINITY_},
    [RLIMIT_RSS_]        = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_NPROC_]      = {1024, 1024},
    [RLIMIT_NOFILE_]     = {1024, 4096},
    [RLIMIT_MEMLOCK_]    = {64*1024, 64*1024},
    [RLIMIT_AS_]         = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_LOCKS_]      = {RLIM_INFINITY_, RLIM_INFINITY_},
    [RLIMIT_SIGPENDING_] = {1024, 1024},
    [RLIMIT_MSGQUEUE_]   = {819200, 819200},
    [RLIMIT_NICE_]       = {0, 0},
    [RLIMIT_RTPRIO_]     = {0, 0},
    [RLIMIT_RTTIME_]     = {RLIM_INFINITY_, RLIM_INFINITY_},
};

static int kill_task(struct task *task, dword_t sig) {
    if (!superuser() &&
            current->uid != task->uid &&
            current->uid != task->suid &&
            current->euid != task->uid &&
            current->euid != task->suid)
        return _EPERM;
    struct siginfo_ info = {
        .code = SI_USER_,
        .kill.pid = current->pid,
        .kill.uid = current->uid,
    };
    send_signal(task, sig, info);
    return 0;
}

// TODO error propagation
static struct task *construct_task(struct task *parent) {
    struct task *task = task_create_(parent);
    if (task == NULL) {
        // Handle task creation error
        return NULL;
    }

    // task_create_ only ALIASES a parent's uts_ns: with no parent it retains
    // init_uts_ns itself, but with one it copies the pointer out of the parent
    // struct and leaves the reference to copy_task, which is on the fork path
    // and never runs here. Every other refcounted field the parent copy brought
    // along is replaced with a fresh object below (mm_new, sighand_new,
    // fdtable_new, fs_info_new), so uts_ns was the only one left owning nothing
    // while do_exit still released it. Each become_new_init_child task therefore
    // decremented a reference it never took, and since every terminal session,
    // the display applet and the root upgrader all come through here, two
    // session exits were enough to drive init_uts_ns to zero and call free() on
    // a static: "pointer being freed was not allocated", SIGABRT out of
    // do_exit. Sharing the parent's UTS namespace is the right semantic (Linux
    // only gives a new one for CLONE_NEWUTS), so take the reference for it.
    if (parent != NULL)
        uts_ns_retain(task->uts_ns);

    //atomic_thread_fence(__ATOMIC_SEQ_CST);
    
    struct tgroup *group = malloc(sizeof(struct tgroup));
    if (group == NULL) {
        // Handle memory allocation failure
        // Clean up the previously allocated 'task'
        kill_task(task, SIGTERM_); // Cleanup
        return NULL;
    }
    
    *group = (struct tgroup) {};
    list_init(&group->threads);
    lock_init(&group->lock, "construct_task\0");
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    memcpy(group->limits, init_rlimits, sizeof(init_rlimits));
    group->leader = task;
    group->personality = ADDR_NO_RANDOMIZE_;
    list_add(&group->threads, &task->group_links);
    task->group = group;
    task->tgid = task->pid;
    task_setsid(task);

    task_set_mm(task, mm_new(task->abi));
    task->sighand = sighand_new();
    task->files = fdtable_new(3); // why is there a 3 here

    task->fs = fs_info_new();
    task->fs->umask = 0022;
    // we'll need to have current set to do the open call
    struct task *old_current = current;
    current = task;
    task->fs->root = generic_open("/", O_RDONLY_, 0);
    current = old_current;
    if (IS_ERR(task->fs->root))
        return ERR_PTR(task->fs->root);
    task->fs->pwd = fd_retain(task->fs->root);

    return task;
}

intptr_t become_first_process(void) {
    // now seems like a nice time
    establish_signal_handlers();

    list_init(&alive_pids_list);
    init_pending_queues(); // Initialize pending queus

    struct task *task = construct_task(NULL);
    if (IS_ERR(task))
        return PTR_ERR(task);

    // The machine boots when init does, not when the app process starts.
    //
    // run_at_boot() sets boot_time once per APP PROCESS, and iOS keeps an app
    // alive across suspensions for days -- so a guest whose init had just
    // started reported an uptime measured from the app's launch, and
    // /proc/stat's btime (derived from it) named a moment long past. Linux
    // userland reads btime as "when this system came up" and checks it:
    // wtmpdb-update-boot refused AOK's outright, with "Boot time too far in
    // the past".
    //
    // This is where pid 1 is created, which is the only event in AOK that
    // means what a boot means. run_at_boot still seeds boot_time so it is
    // never zero for anything that reads the clock before init exists.
    extern time_t boot_time;
    boot_time = time(NULL);
    //task_ref_cnt_mod(task, 1);
    current = task;
    return 0;
}

intptr_t become_new_init_child(void) {
    struct task *init = pid_get_task_ref(1);
    if (init == NULL)
        return _ESRCH;

    struct task *task = construct_task(init);
    task_ref_cnt_mod(init, -1);
    if (IS_ERR(task))
        return PTR_ERR(task);

    // these are things we definitely don't want to inherit
    task->clear_tid = 0;
    task->vfork = NULL;
    task->blocked = task->pending = task->waiting = 0;
    list_init(&task->queue);
    // TODO: think about whether it would be a good idea to inherit fs_info

    current = task;
    return 0;
}

extern int console_major;
extern int console_minor;
void set_console_device(int major, int minor) {
    console_major = major;
    console_minor = minor;
}

void get_console_device(int *major, int *minor) {
    if (major != NULL)
        *major = console_major;
    if (minor != NULL)
        *minor = console_minor;
}

int create_stdio(const char *file, int major, int minor) {
    struct fd *fd = generic_open(file, O_RDWR_, 0);
    if (!IS_ERR(fd)) {
        struct statbuf stat = {};
        int stat_err = fd->mount->fs->fstat(fd, &stat);
        if (stat_err < 0 || !S_ISCHR(stat.mode) || stat.rdev != dev_make(major, minor)) {
            fd_close(fd);
            fd = ERR_PTR(stat_err < 0 ? stat_err : _ENODEV);
        }
    }
    if (IS_ERR(fd)) {
        // fallback to adhoc files for stdio
        fd = adhoc_fd_create(NULL);
        fd->stat.rdev = dev_make(major, minor);
        fd->stat.mode = S_IFCHR | S_IRUSR;
        fd->flags = O_RDWR_;
        int err = dev_open(major, minor, DEV_CHAR, fd);
        if (err < 0)
            return err;
    }

    fd->refcount = 0;
    current->files->files[0] = fd_retain(fd);
    current->files->files[1] = fd_retain(fd);
    current->files->files[2] = fd_retain(fd);
    return 0;
}

static struct fd *open_fd_from_actual_fd(int fd_no) {
    struct fd *fd = adhoc_fd_create(&realfs_fdops);
    if (fd == NULL) {
        return NULL;
    }
    fd->real_fd = fd_no;
    fd->dir = NULL;
    int flags = realfs_getflags(fd);
    if (flags >= 0)
        fd->flags = flags;
    realfs_fstat(fd, &fd->stat);
    return fd;
}

int create_piped_stdio(void) {
    if (!(current->files->files[0] = open_fd_from_actual_fd(STDIN_FILENO))) {
        return -1;
    }
    if (!(current->files->files[1] = open_fd_from_actual_fd(STDOUT_FILENO))) {
        return -1;
    }
    if (!(current->files->files[2] = open_fd_from_actual_fd(STDERR_FILENO))) {
        return -1;
    }
    return 0;
}

static long ish_monotonic_ms_since(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000L +
           (now.tv_nsec - start->tv_nsec) / 1000000L;
}

int run_guest_command_capture(const char *command, const char *env,
                              int timeout_ms, size_t max_output,
                              struct guest_command_result *result) {
    if (result == NULL || command == NULL)
        return _EINVAL;
    memset(result, 0, sizeof(*result));
    if (max_output == 0)
        max_output = 64 * 1024;

    // Host pipe: the guest writes stdout/stderr into the write ends, we drain the
    // read end. Two write ends (one dup'd) so guest fd 1 and fd 2 each own an
    // independent host descriptor and close()ing one never closes the other.
    int pipefd[2];
    if (pipe(pipefd) < 0)
        return -errno;
    int read_fd = pipefd[0];
    int write_fd = pipefd[1];
    int write_fd2 = dup(write_fd);
    if (write_fd2 < 0) {
        int saved_errno = errno;
        close(read_fd);
        close(write_fd);
        return -saved_errno;
    }
    int null_fd = open("/dev/null", O_RDONLY); // guest stdin -> immediate EOF

    struct task *saved = current;

    intptr_t spawn_err = become_new_init_child();
    if (spawn_err < 0) {
        close(read_fd);
        close(write_fd);
        close(write_fd2);
        if (null_fd >= 0)
            close(null_fd);
        current = saved;
        return (int) spawn_err;
    }
    struct task *child = current;
    dword_t child_pid = child->pid;

    // Pack argv as do_execve expects: "/bin/sh\0-c\0<command>\0\0", argc = 3.
    // Note the trailing double-NUL: args_size() walks `count` strings and then
    // asserts the next byte is '\0' (see exec.c), so the buffer needs one extra
    // terminator after the last argument -- exactly what xX_main_Xx writes.
    static const char shell_path[] = "/bin/sh";
    size_t command_len = strlen(command);
    char *argv = malloc(sizeof(shell_path) + sizeof("-c") + command_len + 1 + 1);
    const char *envp = (env != NULL && env[0] != '\0') ? env
        : "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
          "HOME=/root\0TERM=dumb\0";
    int launch_err = argv != NULL ? 0 : _ENOMEM;
    if (argv != NULL) {
        size_t off = 0;
        memcpy(argv + off, shell_path, sizeof(shell_path)); off += sizeof(shell_path);
        memcpy(argv + off, "-c", sizeof("-c")); off += sizeof("-c");
        memcpy(argv + off, command, command_len + 1); off += command_len + 1;
        argv[off] = '\0'; // trailing terminator required by args_size()
        // Load the program image first, then wire stdio (matches the xX_main_Xx
        // bootstrap order; do_execve does not need an fd table to be present).
        launch_err = do_execve(shell_path, 3, argv, envp);
        free(argv);
    }
    if (launch_err < 0) {
        close(read_fd);
        close(write_fd);
        close(write_fd2);
        if (null_fd >= 0)
            close(null_fd);
        current = saved;
        return launch_err;
    }

    struct fd *in_fd = null_fd >= 0 ? open_fd_from_actual_fd(null_fd) : NULL;
    struct fd *out_fd = open_fd_from_actual_fd(write_fd);
    struct fd *err_fd = open_fd_from_actual_fd(write_fd2);
    child->files->files[0] = in_fd;
    child->files->files[1] = out_fd;
    child->files->files[2] = err_fd;
    // Each wrapped guest fd now owns its host descriptor and will close() it when
    // the guest exits, which is what finally delivers EOF to read_fd. Only close
    // directly here for any wrap that failed (otherwise the write end would leak
    // and read_fd would never see EOF).
    if (out_fd == NULL) close(write_fd);
    if (err_fd == NULL) close(write_fd2);
    if (in_fd == NULL && null_fd >= 0) close(null_fd);

    result->launched = 1;
    if (task_start(child) < 0) {
        // Host thread limit/memory: the child never ran. Its fdtable owns the
        // pipe write ends, so the release inside task_never_ran_destroy closes
        // them; only the read end is still ours to clean up.
        printk("ERROR: could not start host thread for command child %d\n", child_pid);
        result->launched = 0;
        task_never_ran_destroy(child);
        current = saved;
        close(read_fd);
        return _EAGAIN;
    }
    current = saved; // the child runs on its own thread now; stop impersonating it

    // Drain the pipe until EOF (guest exit closes both write ends), bounding total
    // bytes and wall-clock time. On timeout, SIGKILL the child by pid and keep
    // draining briefly so any final buffered output is still captured.
    size_t cap = 4096;
    char *buf = malloc(cap);
    size_t len = 0;
    int killed = 0;
    int got_eof = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (buf != NULL) {
        int wait_ms = -1;
        if (timeout_ms > 0) {
            long remaining = timeout_ms - ish_monotonic_ms_since(&start);
            if (remaining <= 0) {
                if (!killed) {
                    struct task *ct = pid_get_task_ref(child_pid);
                    if (ct != NULL) {
                        send_signal(ct, SIGKILL_, SIGINFO_NIL);
                        task_ref_cnt_mod(ct, -1);
                    }
                    killed = 1;
                    result->timed_out = 1;
                }
                wait_ms = 200; // grace period to collect final output, then EOF
            } else {
                wait_ms = (int) remaining;
            }
        }
        struct pollfd pfd = {.fd = read_fd, .events = POLLIN};
        int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0) {
            if (killed)
                break; // already SIGKILLed and gave it a grace period
            continue;  // timeout tick; top of loop re-evaluates the deadline
        }
        if (!(pfd.revents & (POLLIN | POLLHUP | POLLERR)))
            continue;
        if (cap - len < 2) { // keep at least one spare byte for the trailing NUL
            size_t newcap = cap < max_output ? cap * 2 : cap;
            if (newcap > max_output + 1)
                newcap = max_output + 1;
            if (newcap <= cap) {
                result->truncated = 1;
                break;
            }
            char *grown = realloc(buf, newcap);
            if (grown == NULL)
                break;
            buf = grown;
            cap = newcap;
        }
        ssize_t n = read(read_fd, buf + len, cap - len - 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0) {
            got_eof = 1;
            break; // EOF: every write end is closed, the guest is done writing
        }
        len += (size_t) n;
        if (len >= max_output) {
            result->truncated = 1;
            break;
        }
    }
    close(read_fd);
    if (buf != NULL) {
        buf[len] = '\0'; // room reserved by the cap-len<2 growth guard above
        result->output = buf;
        result->output_len = len;
    }

    // If we stopped reading for any reason other than clean EOF (truncation,
    // read error, timeout), the child may still be alive and could otherwise
    // wedge the reap below forever (e.g. a command that ignores SIGPIPE), so make
    // sure it dies. On clean EOF we leave it alone to preserve its real exit code.
    if (!got_eof && !killed) {
        struct task *ct = pid_get_task_ref(child_pid);
        if (ct != NULL) {
            send_signal(ct, SIGKILL_, SIGINFO_NIL);
            task_ref_cnt_mod(ct, -1);
        }
    }

    // Reap for the exit status, bounded so it can never hang the caller's thread:
    // poll with WNOHANG, escalate to SIGKILL if the child lingers, and give up
    // after a short grace. The captured output does not depend on this -- if the
    // guest's real init wins the reap race we just lose the exit code, not output.
    struct task *init = pid_get_task_ref(1);
    if (init != NULL) {
        current = init;
        int status = 0;
        int reaped = 0;
        int escalated = 0;
        struct timespec reap_start;
        clock_gettime(CLOCK_MONOTONIC, &reap_start);
        for (;;) {
            reaped = wait_child_status(child_pid, &status, true /*nonblock*/);
            if (reaped != 0)
                break; // reaped > 0: collected; < 0: ECHILD/error (already gone)
            long waited = ish_monotonic_ms_since(&reap_start);
            if (!escalated && waited > 200) {
                struct task *ct = pid_get_task_ref(child_pid);
                if (ct != NULL) {
                    send_signal(ct, SIGKILL_, SIGINFO_NIL);
                    task_ref_cnt_mod(ct, -1);
                }
                escalated = 1;
            }
            if (waited > 3000)
                break; // give up; output is still valid, exit code stays unknown
            nanosleep(&(struct timespec){.tv_nsec = 5 * 1000 * 1000}, NULL);
        }
        current = saved;
        task_ref_cnt_mod(init, -1);
        if (reaped > 0) {
            if ((status & 0x7f) == 0) {
                result->exited = 1;
                result->exit_code = (status >> 8) & 0xff;
            } else {
                result->term_signal = status & 0x7f;
            }
        }
    }
    return 0;
}
