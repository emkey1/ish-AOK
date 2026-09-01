#include <string.h>
#include <stdio.h>
#include "debug.h"
#include "app/DiagnosticsBridge.h"
#include "jit/jit.h"
#include "kernel/calls.h"
#include "emu/interrupt.h"
#include "emu/memory.h"
#include "emu/tlb.h"
#include "emu/vec.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "fs/devices.h"
#include "fs/tty.h"
#include "util/sync.h"

extern bool isGlibC;

static guest_addr_t current_fault_ip(const struct cpu_state *cpu);

dword_t syscall_stub(void) {
    STRACE("syscall_stub()");
    return _ENOSYS;
}
dword_t syscall_stub_silent(void) {
    STRACE("syscall_stub_silent()");
    return _ENOSYS;
}
dword_t syscall_success_stub(void) {
    STRACE("syscall_stub_success()");
    return 0;
}
dword_t syscall_eopnotsupp_stub(void) {
    STRACE("syscall_stub_eopnotsupp()");
    return _EOPNOTSUPP;
}

static bool amd64_tty2_shell_syscall_trace_enabled(void) {
    return false;
}

static struct tty *amd64_session_tty(void) {
    if (current == NULL || current->abi != GUEST_ABI_AMD64 || current->group == NULL)
        return NULL;
    lock(&current->group->lock, 0);
    struct tty *tty = current->group->tty;
    unlock(&current->group->lock);
    if (tty == NULL)
        return NULL;
    if (tty->type != TTY_CONSOLE_MAJOR && tty->type != TTY_PSEUDO_SLAVE_MAJOR)
        return NULL;
    return tty;
}

static bool __attribute__((unused)) amd64_console_tty_is_num(struct tty *tty, int tty_num) {
    return tty != NULL && tty->type == TTY_CONSOLE_MAJOR && tty->num == tty_num;
}

static bool __attribute__((unused)) amd64_session_tty_is_interactive(struct tty *tty) {
    return tty != NULL &&
        (tty->type == TTY_CONSOLE_MAJOR || tty->type == TTY_PSEUDO_SLAVE_MAJOR);
}

static bool __attribute__((unused)) amd64_interactive_session_comm(const char *comm) {
    return strcmp(comm, "login") == 0 ||
        strcmp(comm, "sh") == 0 ||
        strcmp(comm, "ash") == 0 ||
        strcmp(comm, "dash") == 0 ||
        strcmp(comm, "bash") == 0 ||
        strcmp(comm, "getty") == 0 ||
        strcmp(comm, "agetty") == 0;
}

static void amd64_trace_copy_user_path(qword_t guest_path, char *buf, size_t size) {
    if (size == 0)
        return;
    buf[0] = '\0';
    if (guest_path == 0)
        return;
    if (user_read_string((guest_addr_t) guest_path, buf, size) != 0)
        strncpy(buf, "<unreadable>", size);
    buf[size - 1] = '\0';
}

static bool dpkg_overflow_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_DPKG_SYSCALL") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    if (current == NULL)
        return false;
    return strncmp(current->comm, "dpkg", 4) == 0 ||
        strcmp(current->comm, "apt") == 0 ||
        strcmp(current->comm, "apt-get") == 0;
}

static void dpkg_overflow_syscall_trace(qword_t syscall_num, const qword_t raw_args[6], qword_t result) {
    if (!dpkg_overflow_trace_enabled() || (sqword_t) result != _EOVERFLOW)
        return;

    char path[128] = "";
    bool have_path = false;
    bool have_fd = false;
    qword_t fd = 0;

    switch (current->abi) {
    case GUEST_ABI_I386:
        switch (syscall_num) {
        case 33:  // access
        case 106: // stat
        case 107: // lstat
        case 195: // stat64
        case 196: // lstat64
            amd64_trace_copy_user_path(raw_args[0], path, sizeof(path));
            have_path = true;
            break;
        case 108: // fstat
        case 197: // fstat64
            fd = raw_args[0];
            have_fd = true;
            break;
        case 295: // openat
        case 300: // fstatat64
        case 307: // faccessat
        case 383: // statx
            amd64_trace_copy_user_path(raw_args[1], path, sizeof(path));
            have_path = true;
            fd = raw_args[0];
            have_fd = true;
            break;
        default:
            break;
        }
        break;
    case GUEST_ABI_AMD64:
        switch (syscall_num) {
        case 4:   // stat
        case 6:   // lstat
        case 21:  // access
            amd64_trace_copy_user_path(raw_args[0], path, sizeof(path));
            have_path = true;
            break;
        case 5:   // fstat
            fd = raw_args[0];
            have_fd = true;
            break;
        case 257: // openat
        case 262: // newfstatat
        case 269: // faccessat
        case 332: // statx
            amd64_trace_copy_user_path(raw_args[1], path, sizeof(path));
            have_path = true;
            fd = raw_args[0];
            have_fd = true;
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }

    if (have_path && have_fd) {
        fprintf(stderr,
                "ish-dpkgsys:eoverflow pid=%d tgid=%d abi=%d comm=%s nr=%llu fd=%#llx path=%s a2=%#llx a3=%#llx\n",
                current->pid, current->tgid, current->abi, current->comm,
                (unsigned long long) syscall_num, (unsigned long long) fd,
                path, (unsigned long long) raw_args[2], (unsigned long long) raw_args[3]);
        return;
    }
    if (have_path) {
        fprintf(stderr,
                "ish-dpkgsys:eoverflow pid=%d tgid=%d abi=%d comm=%s nr=%llu path=%s a1=%#llx a2=%#llx a3=%#llx\n",
                current->pid, current->tgid, current->abi, current->comm,
                (unsigned long long) syscall_num, path,
                (unsigned long long) raw_args[1], (unsigned long long) raw_args[2],
                (unsigned long long) raw_args[3]);
        return;
    }
    if (have_fd) {
        fprintf(stderr,
                "ish-dpkgsys:eoverflow pid=%d tgid=%d abi=%d comm=%s nr=%llu fd=%#llx a1=%#llx a2=%#llx a3=%#llx\n",
                current->pid, current->tgid, current->abi, current->comm,
                (unsigned long long) syscall_num, (unsigned long long) fd,
                (unsigned long long) raw_args[1], (unsigned long long) raw_args[2],
                (unsigned long long) raw_args[3]);
        return;
    }

    fprintf(stderr,
            "ish-dpkgsys:eoverflow pid=%d tgid=%d abi=%d comm=%s nr=%llu a0=%#llx a1=%#llx a2=%#llx a3=%#llx a4=%#llx a5=%#llx\n",
            current->pid, current->tgid, current->abi, current->comm,
            (unsigned long long) syscall_num,
            (unsigned long long) raw_args[0], (unsigned long long) raw_args[1],
            (unsigned long long) raw_args[2], (unsigned long long) raw_args[3],
            (unsigned long long) raw_args[4], (unsigned long long) raw_args[5]);
}

enum { AMD64_TRACE_LINEAGE_MAX = 32 };
static pid_t_ amd64_traced_exec_pid __attribute__((unused));
static pid_t_ amd64_traced_exec_tgid __attribute__((unused));
static unsigned amd64_traced_exec_trace_count __attribute__((unused));
static pid_t_ amd64_traced_tgid_lineage[AMD64_TRACE_LINEAGE_MAX];
static unsigned amd64_tracked_proc_trace_count;
static unsigned amd64_trace_child_count;
static unsigned amd64_tty_proc_trace_count;
static unsigned amd64_enoent_path_trace_count;
static unsigned amd64_errno_trace_count;
static unsigned amd64_other_errno_trace_count;
static unsigned amd64_signal_trace_count;
static unsigned amd64_write_trace_count;

static void __attribute__((unused)) amd64_trace_clear_lineage(void) {
    memset(amd64_traced_tgid_lineage, 0, sizeof(amd64_traced_tgid_lineage));
}

bool amd64_trace_is_lineage_tgid(pid_t_ tgid) {
    (void) tgid;
    return false;
}

static void amd64_trace_add_lineage_tgid(pid_t_ tgid) {
    if (tgid == 0 || amd64_trace_is_lineage_tgid(tgid))
        return;
    for (unsigned i = 0; i < AMD64_TRACE_LINEAGE_MAX; i++) {
        if (amd64_traced_tgid_lineage[i] == 0) {
            amd64_traced_tgid_lineage[i] = tgid;
            return;
        }
    }
}

void amd64_trace_track_exec(pid_t_ pid, pid_t_ tgid, const char *file) {
    (void) pid;
    (void) tgid;
    (void) file;
}

static bool amd64_tracked_exec_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_AMD64_WRITES") != NULL ? 1 : 0;
    return enabled != 0;
}

static void amd64_tty2_shell_syscall_trace_enter(qword_t syscall_num, const qword_t raw_args[6]) {
    enum { AMD64_TTY2_SHELL_SYSCALL_TRACE_BUDGET = 256 };
    static unsigned amd64_tty2_shell_syscall_trace_count;
    if (!amd64_tty2_shell_syscall_trace_enabled())
        return;
    if (amd64_tty2_shell_syscall_trace_count >= AMD64_TTY2_SHELL_SYSCALL_TRACE_BUDGET)
        return;
    amd64_tty2_shell_syscall_trace_count++;
    printk("tracked tty2 syscall: pid=%d comm=%s nr=%llu a0=%#llx a1=%#llx a2=%#llx a3=%#llx\n",
           current->pid, current->comm, (unsigned long long) syscall_num,
           (unsigned long long) raw_args[0], (unsigned long long) raw_args[1],
           (unsigned long long) raw_args[2], (unsigned long long) raw_args[3]);
}

static void amd64_tty2_shell_syscall_trace_exit(qword_t syscall_num, qword_t result) {
    if (!amd64_tty2_shell_syscall_trace_enabled())
        return;
    printk("tracked tty2 syscall ret: pid=%d comm=%s nr=%llu result=%#llx\n",
           current->pid, current->comm, (unsigned long long) syscall_num,
           (unsigned long long) result);
}

static void amd64_decode_wait_status(int status, char *buf, size_t size) {
    if (size == 0)
        return;
    if ((status & 0xff) == 0x7f) {
        snprintf(buf, size, "stopped sig=%d status=%#x", (status >> 8) & 0xff, status);
        return;
    }
    if ((status & 0x7f) == 0) {
        snprintf(buf, size, "exited code=%d status=%#x", (status >> 8) & 0xff, status);
        return;
    }
    snprintf(buf, size, "signaled sig=%d core=%d status=%#x",
             status & 0x7f, (status & 0x80) != 0, status);
}

static void amd64_tracked_proc_trace_enter(qword_t syscall_num, const qword_t raw_args[6]) {
    enum { AMD64_TRACKED_PROC_TRACE_BUDGET = 128 };
    if (!amd64_tracked_exec_trace_enabled())
        return;
    if (amd64_tracked_proc_trace_count >= AMD64_TRACKED_PROC_TRACE_BUDGET)
        return;
    switch (syscall_num) {
    case 56: // clone
    case 57: // fork
    case 58: // vfork
    case 435: // clone3
        amd64_tracked_proc_trace_count++;
        printk("tracked proc enter: pid=%d tgid=%d abi=%d comm=%s nr=%llu a0=%#llx a1=%#llx a2=%#llx a3=%#llx a4=%#llx\n",
               current->pid, current->tgid, current->abi, current->comm, (unsigned long long) syscall_num,
               (unsigned long long) raw_args[0], (unsigned long long) raw_args[1],
               (unsigned long long) raw_args[2], (unsigned long long) raw_args[3],
               (unsigned long long) raw_args[4]);
        break;
    case 59: // execve
    case 322: { // execveat
        char path[128];
        qword_t path_arg = syscall_num == 322 ? raw_args[1] : raw_args[0];
        amd64_trace_copy_user_path(path_arg, path, sizeof(path));
        amd64_tracked_proc_trace_count++;
        printk("tracked proc enter: pid=%d tgid=%d abi=%d comm=%s nr=%llu file=%s\n",
               current->pid, current->tgid, current->abi, current->comm,
               (unsigned long long) syscall_num, path);
        break;
    }
    case 61: // wait4
    case 247: // waitid
        amd64_tracked_proc_trace_count++;
        printk("tracked proc enter: pid=%d tgid=%d abi=%d comm=%s nr=%llu a0=%#llx a1=%#llx a2=%#llx a3=%#llx\n",
               current->pid, current->tgid, current->abi, current->comm, (unsigned long long) syscall_num,
               (unsigned long long) raw_args[0], (unsigned long long) raw_args[1],
               (unsigned long long) raw_args[2], (unsigned long long) raw_args[3]);
        break;
    default:
        break;
    }
}

static void amd64_tracked_proc_trace_exit(qword_t syscall_num, const qword_t raw_args[6], qword_t result) {
    if (!amd64_tracked_exec_trace_enabled())
        return;
    switch (syscall_num) {
    case 56: // clone
    case 57: // fork
    case 58: // vfork
    case 435: // clone3
    case 59: // execve
    case 322: // execveat
        printk("tracked proc ret: pid=%d tgid=%d abi=%d comm=%s nr=%llu result=%#llx\n",
               current->pid, current->tgid, current->abi, current->comm, (unsigned long long) syscall_num,
               (unsigned long long) result);
        break;
    case 61: { // wait4
        if ((sqword_t) result > 0 && raw_args[1] != 0) {
            int status;
            if (!user_get((addr_t) raw_args[1], status)) {
                char decoded[64];
                amd64_decode_wait_status(status, decoded, sizeof(decoded));
                printk("tracked wait4: pid=%d tgid=%d abi=%d comm=%s child=%#llx %s\n",
                       current->pid, current->tgid, current->abi, current->comm,
                       (unsigned long long) result, decoded);
                return;
            }
        }
        printk("tracked proc ret: pid=%d tgid=%d abi=%d comm=%s nr=%llu result=%#llx\n",
               current->pid, current->tgid, current->abi, current->comm, (unsigned long long) syscall_num,
               (unsigned long long) result);
        break;
    }
    case 247: // waitid
        printk("tracked proc ret: pid=%d tgid=%d abi=%d comm=%s nr=%llu result=%#llx\n",
               current->pid, current->tgid, current->abi, current->comm, (unsigned long long) syscall_num,
               (unsigned long long) result);
        break;
    default:
        break;
    }
}

static void amd64_trace_track_child(qword_t syscall_num, qword_t result) {
    enum { AMD64_TRACE_CHILD_BUDGET = 64 };
    if (current == NULL)
        return;
    if (!amd64_trace_is_lineage_tgid(current->tgid))
        return;
    if (result == 0 || (sqword_t) result < 0)
        return;
    switch (syscall_num) {
    case 56: // clone
    case 57: // fork
    case 58: // vfork
    case 435: // clone3
        amd64_trace_add_lineage_tgid((pid_t_) result);
        if (amd64_trace_child_count < AMD64_TRACE_CHILD_BUDGET) {
            amd64_trace_child_count++;
            printk("tracked child: parent=%d tgid=%d abi=%d child=%d nr=%llu\n",
                   current->pid, current->tgid, current->abi, (pid_t_) result,
                   (unsigned long long) syscall_num);
        }
        break;
    default:
        break;
    }
}

static bool amd64_tty_proc_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_AMD64_TTY") != NULL ? 1 : 0;
    return enabled != 0;
}

static void amd64_tty_process_trace(qword_t syscall_num, const qword_t raw_args[6], qword_t result) {
    enum { AMD64_TTY_PROC_TRACE_BUDGET = 128 };
    if (!amd64_tty_proc_trace_enabled())
        return;
    struct tty *tty = amd64_session_tty();
    if (tty == NULL)
        return;
    if (amd64_tty_proc_trace_count >= AMD64_TTY_PROC_TRACE_BUDGET)
        return;

    switch (syscall_num) {
    case 56: // clone
    case 57: // fork
    case 58: // vfork
    case 435: // clone3
        amd64_tty_proc_trace_count++;
        printk("tracked tty proc: tty=%d pid=%d tgid=%d nr=%llu result=%#llx\n",
               tty->num, current->pid, current->tgid, (unsigned long long) syscall_num,
               (unsigned long long) result);
        break;
    case 61: // wait4
    case 247: // waitid
        amd64_tty_proc_trace_count++;
        if (syscall_num == 61 && (sqword_t) result > 0 && raw_args[1] != 0) {
            dword_t status = 0;
            char decoded[64];
            if (user_get((guest_addr_t) raw_args[1], status) == 0) {
                amd64_decode_wait_status((int) status, decoded, sizeof(decoded));
                printk("tracked tty wait: tty=%d pid=%d tgid=%d nr=%llu a0=%#llx result=%#llx child_status=%s\n",
                       tty->num, current->pid, current->tgid, (unsigned long long) syscall_num,
                       (unsigned long long) raw_args[0], (unsigned long long) result, decoded);
                break;
            }
        }
        printk("tracked tty wait: tty=%d pid=%d tgid=%d nr=%llu a0=%#llx result=%#llx\n",
               tty->num, current->pid, current->tgid, (unsigned long long) syscall_num,
               (unsigned long long) raw_args[0], (unsigned long long) result);
        break;
    case 59: // execve
    case 322: { // execveat
        char path[128];
        qword_t path_arg = syscall_num == 322 ? raw_args[1] : raw_args[0];
        amd64_trace_copy_user_path(path_arg, path, sizeof(path));
        amd64_tty_proc_trace_count++;
        printk("tracked tty exec syscall: tty=%d pid=%d tgid=%d nr=%llu file=%s result=%#llx\n",
               tty->num, current->pid, current->tgid, (unsigned long long) syscall_num,
               path, (unsigned long long) result);
        break;
    }
    default:
        break;
    }
}

static void amd64_tracked_enoent_path_trace(qword_t syscall_num, const qword_t raw_args[6], qword_t result) {
    enum { AMD64_ENOENT_PATH_TRACE_BUDGET = 64 };
    char path[128];
    qword_t path_arg = 0;

    if (!amd64_tracked_exec_trace_enabled())
        return;
    if ((sqword_t) result != _ENOENT)
        return;
    if (amd64_enoent_path_trace_count >= AMD64_ENOENT_PATH_TRACE_BUDGET)
        return;

    switch (syscall_num) {
    case 2:   // open
    case 4:   // stat
    case 6:   // lstat
    case 21:  // access
    case 59:  // execve
        path_arg = raw_args[0];
        break;
    case 257: // openat
    case 262: // newfstatat
    case 269: // faccessat
    case 322: // execveat
    case 437: // openat2
    case 439: // faccessat2
        path_arg = raw_args[1];
        break;
    default:
        return;
    }

    amd64_trace_copy_user_path(path_arg, path, sizeof(path));
    amd64_enoent_path_trace_count++;
    printk("tracked enoent path: pid=%d tgid=%d nr=%llu path=%s a0=%#llx result=%#llx\n",
           current->pid, current->tgid, (unsigned long long) syscall_num, path,
           (unsigned long long) raw_args[0], (unsigned long long) result);
}

static void amd64_tracked_einval_trace(qword_t syscall_num, const qword_t raw_args[6], qword_t result) {
    enum { AMD64_EINVAL_TRACE_BUDGET = 64 };
    if (!amd64_tracked_exec_trace_enabled())
        return;
    if ((sqword_t) result != _EINVAL)
        return;
    if (syscall_num == 85) // i386 readlink: EINVAL on non-symlink probes is expected
        return;
    if (amd64_errno_trace_count >= AMD64_EINVAL_TRACE_BUDGET)
        return;
    amd64_errno_trace_count++;

    char path[128] = "";
    bool have_path = false;
    switch (syscall_num) {
    case 2:   // open
    case 4:   // stat
    case 6:   // lstat
    case 21:  // access
    case 59:  // execve
    case 85:  // creat
        amd64_trace_copy_user_path(raw_args[0], path, sizeof(path));
        have_path = true;
        break;
    case 257: // openat
    case 262: // newfstatat
    case 269: // faccessat
    case 322: // execveat
    case 437: // openat2
    case 439: // faccessat2
        amd64_trace_copy_user_path(raw_args[1], path, sizeof(path));
        have_path = true;
        break;
    default:
        break;
    }

    if (have_path) {
        printk("tracked einval: pid=%d tgid=%d abi=%d comm=%s nr=%llu path=%s a0=%#llx a1=%#llx a2=%#llx a3=%#llx\n",
               current->pid, current->tgid, current->abi, current->comm, (unsigned long long) syscall_num,
               path,
               (unsigned long long) raw_args[0], (unsigned long long) raw_args[1],
               (unsigned long long) raw_args[2], (unsigned long long) raw_args[3]);
        return;
    }

    printk("tracked einval: pid=%d tgid=%d abi=%d comm=%s nr=%llu a0=%#llx a1=%#llx a2=%#llx a3=%#llx a4=%#llx a5=%#llx\n",
           current->pid, current->tgid, current->abi, current->comm, (unsigned long long) syscall_num,
           (unsigned long long) raw_args[0], (unsigned long long) raw_args[1],
           (unsigned long long) raw_args[2], (unsigned long long) raw_args[3],
           (unsigned long long) raw_args[4], (unsigned long long) raw_args[5]);
}

static void amd64_tracked_errno_trace(qword_t syscall_num, const qword_t raw_args[6], qword_t result) {
    enum { AMD64_OTHER_ERRNO_TRACE_BUDGET = 64 };
    if (!amd64_tracked_exec_trace_enabled())
        return;
    if ((sqword_t) result >= 0)
        return;
    if ((syscall_num == 39 && (sqword_t) result == _EEXIST) ||
            (syscall_num == 54 && (sqword_t) result == _ENOTTY) ||
            (syscall_num == 140 && (sqword_t) result == _ESPIPE) ||
            (syscall_num == 240 && (sqword_t) result == _ETIMEDOUT))
        return;
    if ((sqword_t) result == _ENOENT || (sqword_t) result == _ENOSYS || (sqword_t) result == _EINVAL)
        return;
    if (amd64_other_errno_trace_count >= AMD64_OTHER_ERRNO_TRACE_BUDGET)
        return;
    amd64_other_errno_trace_count++;

    char path[128] = "";
    bool have_path = false;
    switch (syscall_num) {
    case 2:   // open
    case 4:   // stat
    case 5:   // open/i386
    case 6:   // lstat
    case 11:  // execve/i386
    case 21:  // access
    case 33:  // access/i386
    case 59:  // execve
    case 85:  // creat
        amd64_trace_copy_user_path(raw_args[0], path, sizeof(path));
        have_path = true;
        break;
    case 257: // openat
    case 262: // newfstatat
    case 269: // faccessat
    case 295: // openat/i386
    case 307: // faccessat/i386
    case 322: // execveat
    case 437: // openat2
    case 439: // faccessat2
        amd64_trace_copy_user_path(raw_args[1], path, sizeof(path));
        have_path = true;
        break;
    default:
        break;
    }

    if (have_path) {
        printk("tracked errno: pid=%d tgid=%d abi=%d comm=%s nr=%llu err=%lld path=%s a0=%#llx a1=%#llx a2=%#llx a3=%#llx\n",
               current->pid, current->tgid, current->abi, current->comm, (unsigned long long) syscall_num,
               (long long) -(sqword_t) result, path,
               (unsigned long long) raw_args[0], (unsigned long long) raw_args[1],
               (unsigned long long) raw_args[2], (unsigned long long) raw_args[3]);
        return;
    }

    printk("tracked errno: pid=%d tgid=%d abi=%d comm=%s nr=%llu err=%lld a0=%#llx a1=%#llx a2=%#llx a3=%#llx a4=%#llx a5=%#llx\n",
           current->pid, current->tgid, current->abi, current->comm, (unsigned long long) syscall_num,
           (long long) -(sqword_t) result,
           (unsigned long long) raw_args[0], (unsigned long long) raw_args[1],
           (unsigned long long) raw_args[2], (unsigned long long) raw_args[3],
           (unsigned long long) raw_args[4], (unsigned long long) raw_args[5]);
}

static void amd64_tracked_sigabrt_trace(qword_t syscall_num, const qword_t raw_args[6], qword_t result) {
    enum { TRACKED_SIGABRT_TRACE_BUDGET = 16 };
    if (!amd64_tracked_exec_trace_enabled())
        return;
    if ((sqword_t) result != 0)
        return;
    if (amd64_signal_trace_count >= TRACKED_SIGABRT_TRACE_BUDGET)
        return;

    pid_t_ target = 0;
    pid_t_ target_tgid = 0;
    dword_t sig = 0;
    switch (syscall_num) {
    case 37:  // kill (i386)
    case 62:  // kill (amd64)
    case 200: // tkill (amd64)
    case 238: // tkill (i386)
        target = (pid_t_) raw_args[0];
        sig = (dword_t) raw_args[1];
        break;
    case 234: // tgkill (amd64)
    case 270: // tgkill (i386)
        target_tgid = (pid_t_) raw_args[0];
        target = (pid_t_) raw_args[1];
        sig = (dword_t) raw_args[2];
        break;
    case 297: // rt_tgsigqueueinfo (amd64)
        target_tgid = (pid_t_) raw_args[0];
        target = (pid_t_) raw_args[1];
        sig = (dword_t) raw_args[2];
        break;
    default:
        return;
    }
    if (sig != SIGABRT_)
        return;
    amd64_signal_trace_count++;
    printk("tracked sigabrt: sender=%d sender_tgid=%d abi=%d comm=%s nr=%llu target=%d target_tgid=%d\n",
           current->pid, current->tgid, current->abi, current->comm,
           (unsigned long long) syscall_num, target, target_tgid);
}

static void amd64_trace_escape_text(const char *src, size_t src_len, char *dst, size_t dst_size) {
    size_t si = 0, di = 0;
    if (dst_size == 0)
        return;
    while (si < src_len && di + 1 < dst_size) {
        unsigned char ch = (unsigned char) src[si++];
        if (ch == '\n') {
            if (di + 2 >= dst_size) break;
            dst[di++] = '\\';
            dst[di++] = 'n';
            continue;
        }
        if (ch == '\r') {
            if (di + 2 >= dst_size) break;
            dst[di++] = '\\';
            dst[di++] = 'r';
            continue;
        }
        if (ch == '\t') {
            if (di + 2 >= dst_size) break;
            dst[di++] = '\\';
            dst[di++] = 't';
            continue;
        }
        if (ch >= 32 && ch <= 126) {
            dst[di++] = (char) ch;
            continue;
        }
        if (di + 4 >= dst_size)
            break;
        snprintf(&dst[di], dst_size - di, "\\x%02x", ch);
        di += 4;
    }
    dst[di] = '\0';
}

static bool amd64_copy_writev_data(qword_t iov_addr, qword_t iov_count, qword_t result,
        char *raw, size_t raw_size, size_t *copied_out) {
    struct guest_iovec_ *iovec;
    size_t copied = 0;
    size_t want;

    if (raw_size == 0 || copied_out == NULL)
        return false;
    *copied_out = 0;
    if ((sqword_t) result <= 0)
        return false;

    want = (size_t) result;
    if (want > raw_size - 1)
        want = raw_size - 1;

    iovec = user_read_iovecs_abi(current, current->abi, (guest_addr_t) iov_addr, (dword_t) iov_count);
    if (IS_ERR(iovec))
        return false;

    for (dword_t i = 0; i < (dword_t) iov_count && copied < want; i++) {
        size_t chunk = iovec[i].len;
        if (chunk > want - copied)
            chunk = want - copied;
        if (chunk == 0)
            continue;
        if (user_read(iovec[i].base, raw + copied, chunk) != 0) {
            free(iovec);
            return false;
        }
        copied += chunk;
    }

    free(iovec);
    *copied_out = copied;
    return true;
}

static void amd64_tracked_write_trace(qword_t syscall_num, const qword_t raw_args[6], qword_t result) {
    enum { AMD64_WRITE_TRACE_BUDGET = 64, AMD64_WRITE_COPY_MAX = 1024, AMD64_WRITE_ESCAPED_MAX = 3072 };
    char raw[AMD64_WRITE_COPY_MAX];
    char escaped[AMD64_WRITE_ESCAPED_MAX];
    qword_t fd, buf_addr = 0, size = 0, iov_count = 0;
    size_t to_copy;

    if (!amd64_tracked_exec_trace_enabled())
        return;
    if (amd64_write_trace_count >= AMD64_WRITE_TRACE_BUDGET)
        return;
    if ((sqword_t) result <= 0)
        return;

    switch (syscall_num) {
    case 1: // write
        fd = raw_args[0];
        buf_addr = raw_args[1];
        size = raw_args[2];
        break;
    case 20: // writev
        fd = raw_args[0];
        buf_addr = raw_args[1];
        iov_count = raw_args[2];
        size = result;
        break;
    default:
        return;
    }

    if (fd != 1 && fd != 2)
        return;

    to_copy = (size_t) result;
    if (to_copy > (size_t) size)
        to_copy = (size_t) size;
    if (to_copy > AMD64_WRITE_COPY_MAX - 1)
        to_copy = AMD64_WRITE_COPY_MAX - 1;
    amd64_write_trace_count++;
    printk("tracked write meta: pid=%d tgid=%d fd=%llu size=%llu result=%llu buf=%#llx\n",
           current->pid, current->tgid, (unsigned long long) fd,
           (unsigned long long) size, (unsigned long long) result,
           (unsigned long long) buf_addr);
    if (to_copy == 0)
        return;
    switch (syscall_num) {
    case 1:
        if (user_read((guest_addr_t) buf_addr, raw, to_copy) != 0) {
            printk("tracked write unreadable: pid=%d tgid=%d fd=%llu buf=%#llx count=%zu\n",
                   current->pid, current->tgid, (unsigned long long) fd,
                   (unsigned long long) buf_addr, to_copy);
            return;
        }
        break;
    case 20:
        if (!amd64_copy_writev_data(buf_addr, iov_count, result, raw, sizeof(raw), &to_copy)) {
            printk("tracked writev unreadable: pid=%d tgid=%d fd=%llu iov=%#llx iovcnt=%llu count=%zu\n",
                   current->pid, current->tgid, (unsigned long long) fd,
                   (unsigned long long) buf_addr, (unsigned long long) iov_count, to_copy);
            return;
        }
        break;
    }
    raw[to_copy] = '\0';
    if (current != NULL && current->abi == GUEST_ABI_AMD64 &&
            strcmp(current->comm, "as") == 0 &&
            (strstr(raw, "Internal error") != NULL ||
             strstr(raw, "build_modrm_byte") != NULL)) {
        dump_amd64_as_trace_task(current);
        dump_amd64_as_state_task(current);
        dump_amd64_as_stack_task(current);
    }
    amd64_trace_escape_text(raw, to_copy, escaped, sizeof(escaped));
    printk("tracked write: pid=%d tgid=%d fd=%llu count=%llu text=\"%s\"\n",
           current->pid, current->tgid, (unsigned long long) fd,
           (unsigned long long) result, escaped);
}

static void amd64_enomem_syscall_trace(qword_t syscall_num, const qword_t raw_args[6], qword_t result) {
    enum { AMD64_ENOMEM_TRACE_BUDGET = 32 };
    static unsigned amd64_enomem_trace_count;
    if (!amd64_tracked_exec_trace_enabled())
        return;
    if (current == NULL || current->abi != GUEST_ABI_AMD64)
        return;
    if ((sqword_t) result != _ENOMEM)
        return;
    if (amd64_enomem_trace_count >= AMD64_ENOMEM_TRACE_BUDGET)
        return;
    amd64_enomem_trace_count++;
    printk("amd64 enomem syscall: pid=%d comm=%s nr=%llu a0=%#llx a1=%#llx a2=%#llx a3=%#llx a4=%#llx a5=%#llx\n",
           current->pid, current->comm, (unsigned long long) syscall_num,
           (unsigned long long) raw_args[0], (unsigned long long) raw_args[1],
           (unsigned long long) raw_args[2], (unsigned long long) raw_args[3],
           (unsigned long long) raw_args[4], (unsigned long long) raw_args[5]);
}

// Positioned I/O whose 64-bit offset arrives as a (low, high) dword pair.
// That is the real i386 kernel ABI (compat_sys_x86_pread and friends take
// poslo/poshi in two consecutive arg registers, because x86-32 has no
// even-register-pair alignment rule), and it is also the shape
// marshal_syscall_args_legacy hands the amd64 table after splitting the
// single 64-bit offset register. Wiring sys_pread/sys_pwrite into a table
// directly instead does not work: their 4th parameter is a 64-bit off_t_,
// but the legacy dispatcher only ever passes six dwords, so the high half
// is dropped -- an i386 guest's pwrite at 5GiB silently landed at 1GiB and
// clobbered whatever was there, with a success return and the right byte
// count. Not "_amd64" any more precisely because i386 needs it too.
static dword_t sys_pread_off_pair(fd_t f, addr_t buf_addr, dword_t size,
        dword_t off_low, dword_t off_high, dword_t UNUSED(unused)) {
    off_t_ off = ((off_t_) off_high << 32) | off_low;
    return sys_pread(f, buf_addr, size, off);
}

static dword_t sys_pwrite_off_pair(fd_t f, addr_t buf_addr, dword_t size,
        dword_t off_low, dword_t off_high, dword_t UNUSED(unused)) {
    off_t_ off = ((off_t_) off_high << 32) | off_low;
    return sys_pwrite(f, buf_addr, size, off);
}

// Legacy 32-bit-pointer fallback for the amd64 table slots;
// handle_amd64_native_memory_syscall intercepts 295/296/327/328 first with the
// 64-bit-safe sys_{preadv,pwritev}{,2}_guest, so these never actually run --
// they only need to exist so the table entries aren't NULL (the NULL check in
// handle_syscall_interrupt runs before the native intercept).
static dword_t sys_preadv_amd64(fd_t f, addr_t iovec_addr, dword_t iovec_count,
        dword_t off_low, dword_t off_high, dword_t UNUSED(unused)) {
    off_t_ off = ((off_t_) off_high << 32) | off_low;
    return sys_preadv_guest(f, iovec_addr, iovec_count, off);
}

static dword_t sys_pwritev_amd64(fd_t f, addr_t iovec_addr, dword_t iovec_count,
        dword_t off_low, dword_t off_high, dword_t UNUSED(unused)) {
    off_t_ off = ((off_t_) off_high << 32) | off_low;
    return sys_pwritev_guest(f, iovec_addr, iovec_count, off);
}

static dword_t sys_preadv2_amd64(fd_t f, addr_t iovec_addr, dword_t iovec_count,
        dword_t off_low, dword_t off_high, dword_t flags) {
    off_t_ off = ((off_t_) off_high << 32) | off_low;
    return sys_preadv2_guest(f, iovec_addr, iovec_count, off, flags);
}

static dword_t sys_pwritev2_amd64(fd_t f, addr_t iovec_addr, dword_t iovec_count,
        dword_t off_low, dword_t off_high, dword_t flags) {
    off_t_ off = ((off_t_) off_high << 32) | off_low;
    return sys_pwritev2_guest(f, iovec_addr, iovec_count, off, flags);
}

// i386 preadv/pwritev/preadv2/pwritev2 (333/334/378/379). Same (poslo, poshi)
// offset pair as pread/pwrite above, but the vector itself has to be read with
// the 32-bit iovec layout, so these cannot share the amd64 wrappers.
static dword_t sys_preadv_i386(fd_t f, addr_t iovec_addr, dword_t iovec_count,
        dword_t off_low, dword_t off_high, dword_t UNUSED(unused)) {
    off_t_ off = ((off_t_) off_high << 32) | off_low;
    return sys_preadv_i386_guest(f, iovec_addr, iovec_count, off);
}

static dword_t sys_pwritev_i386(fd_t f, addr_t iovec_addr, dword_t iovec_count,
        dword_t off_low, dword_t off_high, dword_t UNUSED(unused)) {
    off_t_ off = ((off_t_) off_high << 32) | off_low;
    return sys_pwritev_i386_guest(f, iovec_addr, iovec_count, off);
}

static dword_t sys_preadv2_i386(fd_t f, addr_t iovec_addr, dword_t iovec_count,
        dword_t off_low, dword_t off_high, dword_t flags) {
    off_t_ off = ((off_t_) off_high << 32) | off_low;
    return sys_preadv2_i386_guest(f, iovec_addr, iovec_count, off, flags);
}

static dword_t sys_pwritev2_i386(fd_t f, addr_t iovec_addr, dword_t iovec_count,
        dword_t off_low, dword_t off_high, dword_t flags) {
    off_t_ off = ((off_t_) off_high << 32) | off_low;
    return sys_pwritev2_i386_guest(f, iovec_addr, iovec_count, off, flags);
}

static dword_t sys_truncate_amd64(addr_t path_addr, dword_t size_low, dword_t size_high,
        dword_t UNUSED(unused3), dword_t UNUSED(unused4), dword_t UNUSED(unused5)) {
    return sys_truncate64(path_addr, size_low, size_high);
}

static dword_t sys_ftruncate_amd64(fd_t f, dword_t size_low, dword_t size_high,
        dword_t UNUSED(unused3), dword_t UNUSED(unused4), dword_t UNUSED(unused5)) {
    return sys_ftruncate64(f, size_low, size_high);
}

static dword_t sys_fallocate_amd64(fd_t f, dword_t mode, dword_t offset_low, dword_t offset_high,
        dword_t len_low, dword_t len_high) {
    return sys_fallocate(f, mode, offset_low, offset_high, len_low, len_high);
}

// The syscall tables intentionally cast handlers with varied signatures to the
// uniform syscall_t; the dispatcher always invokes them with the correct
// arguments. clang reports __GNUC__ as 4, so the is_gcc(8) guard alone left the
// suppression disabled under clang (the actual compiler), producing hundreds of
// -Wcast-function-type-mismatch warnings.
#if is_gcc(8) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
static syscall_t i386_syscall_table[] = {
    [1]   = (syscall_t) sys_exit,
    [2]   = (syscall_t) sys_fork,
    [3]   = (syscall_t) sys_read,
    [4]   = (syscall_t) sys_write,
    [5]   = (syscall_t) sys_open,
    [6]   = (syscall_t) sys_close,
    [7]   = (syscall_t) sys_waitpid,
    [8]   = (syscall_t) sys_creat, // creat
    [9]   = (syscall_t) sys_link,
    [10]  = (syscall_t) sys_unlink,
    [11]  = (syscall_t) sys_execve,
    [12]  = (syscall_t) sys_chdir,
    [13]  = (syscall_t) sys_time,
    [14]  = (syscall_t) sys_mknod,
    [15]  = (syscall_t) sys_chmod,
    [19]  = (syscall_t) sys_lseek,
    [20]  = (syscall_t) sys_getpid,
    [21]  = (syscall_t) sys_mount,
    [23]  = (syscall_t) sys_setuid,
    [24]  = (syscall_t) sys_getuid,
    [25]  = (syscall_t) sys_stime,
    [26]  = (syscall_t) sys_ptrace,
    [27]  = (syscall_t) sys_alarm,
    [29]  = (syscall_t) sys_pause,
    [30]  = (syscall_t) sys_utime,
    [33]  = (syscall_t) sys_access,
    [36]  = (syscall_t) syscall_success_stub, // sync
    [37]  = (syscall_t) sys_kill,
    [38]  = (syscall_t) sys_rename,
    [39]  = (syscall_t) sys_mkdir,
    [40]  = (syscall_t) sys_rmdir,
    [41]  = (syscall_t) sys_dup,
    [42]  = (syscall_t) sys_pipe,
    [43]  = (syscall_t) sys_times,
    [45]  = (syscall_t) sys_brk,
    [46]  = (syscall_t) sys_setgid,
    [47]  = (syscall_t) sys_getgid,
    [49]  = (syscall_t) sys_geteuid,
    [50]  = (syscall_t) sys_getegid,
    [52]  = (syscall_t) sys_umount2,
    [54]  = (syscall_t) sys_ioctl,
    [55]  = (syscall_t) sys_fcntl32,
    [57]  = (syscall_t) sys_setpgid,
    [60]  = (syscall_t) sys_umask,
    [61]  = (syscall_t) sys_chroot,
    [63]  = (syscall_t) sys_dup2,
    [64]  = (syscall_t) sys_getppid,
    [65]  = (syscall_t) sys_getpgrp,
    [66]  = (syscall_t) sys_setsid,
    [67]  = (syscall_t) syscall_stub, // sigaction
    [74]  = (syscall_t) sys_sethostname,
    [75]  = (syscall_t) sys_setrlimit32,
    [76]  = (syscall_t) sys_old_getrlimit32,
    [77]  = (syscall_t) sys_getrusage,
    [78]  = (syscall_t) sys_gettimeofday,
    [79]  = (syscall_t) sys_settimeofday,
    [80]  = (syscall_t) sys_getgroups,
    [81]  = (syscall_t) sys_setgroups,
    [83]  = (syscall_t) sys_symlink,
    [85]  = (syscall_t) sys_readlink,
    [88]  = (syscall_t) sys_reboot,
    [90]  = (syscall_t) sys_mmap,
    [91]  = (syscall_t) sys_munmap,
    [92]  = (syscall_t) sys_truncate,
    [93]  = (syscall_t) sys_ftruncate,
    [94]  = (syscall_t) sys_fchmod,
    [96]  = (syscall_t) sys_getpriority,
    [97]  = (syscall_t) sys_setpriority,
    [99]  = (syscall_t) sys_statfs,
    [100] = (syscall_t) sys_fstatfs,
    [102] = (syscall_t) sys_socketcall,
    [103] = (syscall_t) sys_syslog,
    [104] = (syscall_t) sys_setitimer,
    [105] = (syscall_t) sys_getitimer,
    [106] = (syscall_t) sys_stat,
    [107] = (syscall_t) sys_lstat,
    [108] = (syscall_t) sys_fstat,
    [111] = (syscall_t) syscall_success_stub, // vhangup (tty-cleanup no-op)
    [114] = (syscall_t) sys_wait4,
    [116] = (syscall_t) sys_sysinfo,
    [117] = (syscall_t) sys_ipc,
    [118] = (syscall_t) sys_fsync,
    [119] = (syscall_t) sys_sigreturn,
    [120] = (syscall_t) sys_clone,
    [121] = (syscall_t) sys_setdomainname,
    [122] = (syscall_t) sys_uname,
    [124] = (syscall_t) sys_adjtimex, // adjtimex
    [125] = (syscall_t) sys_mprotect,
    [126] = (syscall_t) sys_sigprocmask,
    [132] = (syscall_t) sys_getpgid,
    [133] = (syscall_t) sys_fchdir,
    [136] = (syscall_t) sys_personality,
    [140] = (syscall_t) sys__llseek,
    [141] = (syscall_t) sys_getdents,
    [142] = (syscall_t) sys_select,
    [143] = (syscall_t) sys_flock,
    [144] = (syscall_t) sys_msync,
    [145] = (syscall_t) sys_readv,
    [146] = (syscall_t) sys_writev,
    [147] = (syscall_t) sys_getsid,
    [148] = (syscall_t) sys_fsync, // fdatasync
    [150] = (syscall_t) sys_mlock,
    [151] = (syscall_t) sys_munlock,
    [152] = (syscall_t) sys_mlockall,
    [153] = (syscall_t) sys_munlockall,
    [154] = (syscall_t) sys_sched_setparam,
    [155] = (syscall_t) sys_sched_getparam,
    [156] = (syscall_t) sys_sched_setscheduler,
    [157] = (syscall_t) sys_sched_getscheduler,
    [158] = (syscall_t) sys_sched_yield,
    [159] = (syscall_t) sys_sched_get_priority_max,
    [160] = (syscall_t) sys_sched_get_priority_min,
    [161] = (syscall_t) sys_sched_rr_get_interval,
    [162] = (syscall_t) sys_nanosleep,
    [163] = (syscall_t) sys_mremap,
    [168] = (syscall_t) sys_poll,
    [172] = (syscall_t) sys_prctl,
    [173] = (syscall_t) sys_rt_sigreturn,
    [174] = (syscall_t) sys_rt_sigaction,
    [175] = (syscall_t) sys_rt_sigprocmask,
    [176] = (syscall_t) sys_rt_sigpending,
    [177] = (syscall_t) sys_rt_sigtimedwait,
    [178] = (syscall_t) sys_rt_sigqueueinfo,
    [179] = (syscall_t) sys_rt_sigsuspend,
    // NOT sys_pread/sys_pwrite: their 64-bit off_t_ cannot survive the
    // six-dword legacy dispatcher. See sys_pread_off_pair.
    [180] = (syscall_t) sys_pread_off_pair,
    [181] = (syscall_t) sys_pwrite_off_pair,
    [183] = (syscall_t) sys_getcwd,
    [184] = (syscall_t) sys_capget,
    [185] = (syscall_t) sys_capset,
    [186] = (syscall_t) sys_sigaltstack,
    [187] = (syscall_t) sys_sendfile,
    [190] = (syscall_t) sys_vfork,
    [191] = (syscall_t) sys_getrlimit32,
    [192] = (syscall_t) sys_mmap2,
    [193] = (syscall_t) sys_truncate64,
    [194] = (syscall_t) sys_ftruncate64,
    [195] = (syscall_t) sys_stat64,
    [196] = (syscall_t) sys_lstat64,
    [197] = (syscall_t) sys_fstat64,
    [198] = (syscall_t) sys_lchown,
    [199] = (syscall_t) sys_getuid32,
    [200] = (syscall_t) sys_getgid32,
    [201] = (syscall_t) sys_geteuid32,
    [202] = (syscall_t) sys_getegid32,
    [203] = (syscall_t) sys_setreuid,
    [204] = (syscall_t) sys_setregid,
    [205] = (syscall_t) sys_getgroups,
    [206] = (syscall_t) sys_setgroups,
    [207] = (syscall_t) sys_fchown32,
    [208] = (syscall_t) sys_setresuid,
    [209] = (syscall_t) sys_getresuid,
    [210] = (syscall_t) sys_setresgid,
    [211] = (syscall_t) sys_getresgid,
    [212] = (syscall_t) sys_chown32,
    [213] = (syscall_t) sys_setuid,
    [214] = (syscall_t) sys_setgid,
    [215] = (syscall_t) sys_setfsuid,
    [216] = (syscall_t) sys_setfsgid,
    [218] = (syscall_t) sys_mincore,
    [219] = (syscall_t) sys_madvise,
    [220] = (syscall_t) sys_getdents64,
    [221] = (syscall_t) sys_fcntl,
    [224] = (syscall_t) sys_gettid,
    [225] = (syscall_t) syscall_success_stub, // readahead
    [226 ... 237] = (syscall_t) sys_xattr_stub,
    [238] = (syscall_t) sys_tkill,
    [239] = (syscall_t) sys_sendfile64,
    [240] = (syscall_t) sys_futex,
    [241] = (syscall_t) sys_sched_setaffinity,
    [242] = (syscall_t) sys_sched_getaffinity,
    [243] = (syscall_t) sys_set_thread_area,
    // The io_* family. 246-249 were absent rather than stubbed, and absent is
    // worse: a missing entry is a "missing syscall" SIGSYS kill instead of an
    // errno, so a 32-bit guest touching AIO died without even a log line.
    [245] = (syscall_t) sys_io_setup,
    [246] = (syscall_t) sys_io_destroy,
    [247] = (syscall_t) sys_io_getevents,
    [248] = (syscall_t) sys_io_submit,
    [249] = (syscall_t) sys_io_cancel,
    [252] = (syscall_t) sys_exit_group,
    [254] = (syscall_t) sys_epoll_create0,
    [255] = (syscall_t) sys_epoll_ctl,
    [256] = (syscall_t) sys_epoll_wait,
    [258] = (syscall_t) sys_set_tid_address,
    [259] = (syscall_t) sys_timer_create,
    [260] = (syscall_t) sys_timer_settime,
    [261] = (syscall_t) sys_timer_gettime,
    [262] = (syscall_t) sys_timer_getoverrun,
    [263] = (syscall_t) sys_timer_delete,
    [264] = (syscall_t) sys_clock_settime,
    [265] = (syscall_t) sys_clock_gettime,
    [266] = (syscall_t) sys_clock_getres,
    [267] = (syscall_t) sys_clock_nanosleep,
    [268] = (syscall_t) sys_statfs64,
    [269] = (syscall_t) sys_fstatfs64,
    [270] = (syscall_t) sys_tgkill,
    [271] = (syscall_t) sys_utimes,
    [272] = (syscall_t) syscall_success_stub,
    [274] = (syscall_t) sys_mbind,
    [275] = (syscall_t) sys_get_mempolicy,
    [276] = (syscall_t) sys_set_mempolicy,
    [284] = (syscall_t) sys_waitid,
    [288] = (syscall_t) sys_keyctl,
    [289] = (syscall_t) sys_ioprio_set,
    [290] = (syscall_t) sys_ioprio_get,
    [291] = (syscall_t) sys_inotify_init,
    [292] = (syscall_t) sys_inotify_add_watch,
    [293] = (syscall_t) sys_inotify_rm_watch,
    [295] = (syscall_t) sys_openat,
    [296] = (syscall_t) sys_mkdirat,
    [297] = (syscall_t) sys_mknodat,
    [298] = (syscall_t) sys_fchownat,
    [300] = (syscall_t) sys_fstatat64,
    [301] = (syscall_t) sys_unlinkat,
    [302] = (syscall_t) sys_renameat,
    [303] = (syscall_t) sys_linkat,
    [304] = (syscall_t) sys_symlinkat,
    [305] = (syscall_t) sys_readlinkat,
    [306] = (syscall_t) sys_fchmodat,
    [307] = (syscall_t) sys_faccessat,
    [308] = (syscall_t) sys_pselect,
    [309] = (syscall_t) sys_ppoll,
    [310] = (syscall_t) sys_unshare,
    [311] = (syscall_t) sys_set_robust_list,
    [312] = (syscall_t) sys_get_robust_list,
    [313] = (syscall_t) sys_splice,
    [315] = (syscall_t) sys_tee,
    [316] = (syscall_t) sys_vmsplice,
    [314] = (syscall_t) syscall_success_stub, // sync_file_range
    [318] = (syscall_t) syscall_success_stub, // getcpu
    [319] = (syscall_t) sys_epoll_pwait,
    [320] = (syscall_t) sys_utimensat,
    [321] = (syscall_t) sys_signalfd,
    [322] = (syscall_t) sys_timerfd_create,
    [323] = (syscall_t) sys_eventfd,
    [324] = (syscall_t) sys_fallocate,
    [325] = (syscall_t) sys_timerfd_settime,
    [326] = (syscall_t) sys_timerfd_gettime,
    [327] = (syscall_t) sys_signalfd4,
    [328] = (syscall_t) sys_eventfd2,
    [329] = (syscall_t) sys_epoll_create,
    [330] = (syscall_t) sys_dup3,
    [331] = (syscall_t) sys_pipe2,
    [332] = (syscall_t) sys_inotify_init1,
    [333] = (syscall_t) sys_preadv_i386,
    [334] = (syscall_t) sys_pwritev_i386,
    [336] = (syscall_t) syscall_stub, // perf_event_open
    [337] = (syscall_t) sys_recvmmsg,
    [340] = (syscall_t) sys_prlimit64,
    [341] = (syscall_t) syscall_eopnotsupp_stub, // name_to_handle_at
    [342] = (syscall_t) syscall_eopnotsupp_stub, // open_by_handle_at
    [343] = (syscall_t) sys_clock_adjtime, // clock_adjtime (EPERM: iSH can't slew the iOS clock)
    [344] = (syscall_t) sys_syncfs,
    [345] = (syscall_t) sys_sendmmsg,
    [347] = (syscall_t) sys_process_vm_readv,
    [349] = (syscall_t) sys_kcmp,
    [352] = (syscall_t) syscall_stub, // sched_getattr
    [353] = (syscall_t) sys_renameat2,
    [354] = (syscall_t) syscall_eopnotsupp_stub, // seccomp
    [355] = (syscall_t) sys_getrandom,
    [356] = (syscall_t) sys_memfd_create,
    [358] = (syscall_t) sys_execveat,
    [359] = (syscall_t) sys_socket,
    [360] = (syscall_t) sys_socketpair,
    [361] = (syscall_t) sys_bind,
    [362] = (syscall_t) sys_connect,
    [363] = (syscall_t) sys_listen,
    [364] = (syscall_t) sys_accept4,
    [365] = (syscall_t) sys_getsockopt,
    [366] = (syscall_t) sys_setsockopt,
    [367] = (syscall_t) sys_getsockname,
    [368] = (syscall_t) sys_getpeername,
    [369] = (syscall_t) sys_sendto,
    [370] = (syscall_t) sys_sendmsg,
    [371] = (syscall_t) sys_recvfrom,
    [372] = (syscall_t) sys_recvmsg,
    [373] = (syscall_t) sys_shutdown,
    [375] = (syscall_t) sys_membarrier, // membarrier
    [377] = (syscall_t) sys_copy_file_range,
    [378] = (syscall_t) sys_preadv2_i386,
    [379] = (syscall_t) sys_pwritev2_i386,
    [383] = (syscall_t) sys_statx,
    [384] = (syscall_t) sys_arch_prctl,
    [386] = (syscall_t) sys_rseq,
    // Direct SysV IPC syscalls, added to i386 in Linux 4.19 alongside the
    // older sys_ipc(2) multiplexer at [117]. 394 is semctl, not semtimedop:
    // semtimedop is 392 (the time32 flavour; the timeout is ignored either
    // way). Having semtimedop at 394 meant every modern-musl semctl(2) --
    // musl calls 394 directly when __NR_semctl is defined -- was interpreted
    // as a semop vector, returning EFAULT for a perfectly valid call.
    [392] = (syscall_t) sys_semtimedop,
    [393] = (syscall_t) sys_semget,
    [394] = (syscall_t) sys_semctl,
    [395] = (syscall_t) sys_shmget,
    [396] = (syscall_t) sys_shmctl,
    [397] = (syscall_t) sys_shmat,
    [398] = (syscall_t) sys_shmdt,
    [399] = (syscall_t) sys_msgget,
    [400] = (syscall_t) sys_msgsnd,
    [401] = (syscall_t) sys_msgrcv,
    [402] = (syscall_t) sys_msgctl,
    [403] = (syscall_t) sys_clock_gettime64, // clock_gettime64
    [404] = (syscall_t) sys_clock_settime64, // clock_settime64
    [405] = (syscall_t) sys_clock_adjtime64, // clock_adjtime64 (read-state -> chronyd monitor-only)
    [406] = (syscall_t) sys_clock_getres_time64, // clock_getres_time64
    [407] = (syscall_t) sys_clock_nanosleep_time64, // clock_nanosleep_time64
    [408] = (syscall_t) sys_timer_gettime64, // timer_gettime64
    [409] = (syscall_t) sys_timer_settime64, // timer_settime64
    [410] = (syscall_t) sys_timerfd_gettime64, // timerfd_gettime64
    [411] = (syscall_t) sys_timerfd_settime64, // timerfd_settime64
    [412] = (syscall_t) sys_utimensat64, // utimensat_time64
    [413] = (syscall_t) sys_pselect_time64, // pselect6_time64
    [414] = (syscall_t) sys_ppoll_time64,
    [417] = (syscall_t) sys_recvmmsg_time64, // recvmmsg_time64
    [421] = (syscall_t) sys_rt_sigtimedwait_time64, // rt_sigtimedwait_time64
    [422] = (syscall_t) sys_futex_time64, // futex_time64
    [424] = (syscall_t) sys_pidfd_send_signal,
    [425] = (syscall_t) syscall_stub_silent, // io_uring_setup (mirror amd64: ENOSYS so liburing/cmake fall back to epoll)
    [426] = (syscall_t) syscall_stub_silent, // io_uring_enter
    [427] = (syscall_t) syscall_stub_silent, // io_uring_register
    [428] = (syscall_t) syscall_stub_silent, // open_tree (new mount API; util-linux falls back to mount(2) on ENOSYS)
    [429] = (syscall_t) sys_move_mount,
    [430] = (syscall_t) sys_fsopen,
    [431] = (syscall_t) sys_fsconfig,
    [432] = (syscall_t) sys_fsmount,
    [433] = (syscall_t) syscall_stub_silent, // fspick
    [434] = (syscall_t) sys_pidfd_open,
    [435] = (syscall_t) sys_clone3, // clone3
    [436] = (syscall_t) sys_close_range,
    [437] = (syscall_t) sys_openat2,
    [438] = (syscall_t) syscall_stub_silent, // pidfd_getfd
    [439] = (syscall_t) sys_faccessat, // faccessat2
    [440] = (syscall_t) syscall_stub_silent, // process_madvise
    [441] = (syscall_t) sys_epoll_pwait2, // epoll_pwait2
    // 442-448, 450, 451, 453: same post-5.x-syscall gap as the arm64 table
    // (identical syscall numbers on i386 -- Linux assigns these consistently
    // across arches). i386 doesn't need an arg-count classifier entry to go
    // with these: marshal_syscall_args_legacy only enforces the 32-bit
    // dword-fit check for the two 64-bit-register ABIs (amd64/arm64); i386
    // registers are already word-sized, so a stub here can't SIGSYS the way
    // it did on arm64 before that table also got explicit arg counts.
    [442] = (syscall_t) syscall_stub_silent, // mount_setattr
    [443] = (syscall_t) syscall_stub_silent, // quotactl_fd
    [444] = (syscall_t) syscall_stub_silent, // landlock_create_ruleset
    [445] = (syscall_t) syscall_stub_silent, // landlock_add_rule
    [446] = (syscall_t) syscall_stub_silent, // landlock_restrict_self
    [447] = (syscall_t) syscall_stub_silent, // memfd_secret
    [448] = (syscall_t) syscall_stub_silent, // process_mrelease
    [449] = (syscall_t) syscall_stub_silent, // futex_waitv
    [450] = (syscall_t) syscall_stub_silent, // set_mempolicy_home_node
    [451] = (syscall_t) syscall_stub_silent, // cachestat
    [452] = (syscall_t) sys_fchmodat2,
    [453] = (syscall_t) syscall_stub_silent, // map_shadow_stack
    // 454-462: see arm64_syscall_table's matching comment (futex2 family,
    // mount API introspection, LSM self-attr API, memory sealing).
    [454] = (syscall_t) syscall_stub_silent, // futex_wake
    [455] = (syscall_t) syscall_stub_silent, // futex_wait
    [456] = (syscall_t) syscall_stub_silent, // futex_requeue
    [457] = (syscall_t) syscall_stub_silent, // statmount
    [458] = (syscall_t) syscall_stub_silent, // listmount
    [459] = (syscall_t) syscall_stub_silent, // lsm_get_self_attr
    [460] = (syscall_t) syscall_stub_silent, // lsm_set_self_attr
    [461] = (syscall_t) syscall_stub_silent, // lsm_list_modules
    [462] = (syscall_t) syscall_stub_silent, // mseal
    // 463-469: xattrat family (6.13), open_tree_attr (6.15),
    // file_getattr/file_setattr (6.17). Same silent-ENOSYS treatment as
    // 442-462 -- any pre-6.13 kernel (uname advertises 5.20) returns ENOSYS
    // for all of these, and callers fall back to the classic xattr path
    // (which reports ENOTSUP like a no-xattr filesystem). Observed: Arch
    // aarch64 useradd -m probing getxattrat/464 during the /etc/skel copy
    // got SIGSYS-killed by the out-of-table "missing syscall" path.
    [463] = (syscall_t) syscall_stub_silent, // setxattrat
    [464] = (syscall_t) syscall_stub_silent, // getxattrat
    [465] = (syscall_t) syscall_stub_silent, // listxattrat
    [466] = (syscall_t) syscall_stub_silent, // removexattrat
    [467] = (syscall_t) syscall_stub_silent, // open_tree_attr
    [468] = (syscall_t) syscall_stub_silent, // file_getattr
    [469] = (syscall_t) syscall_stub_silent, // file_setattr
};
/*
SYS_MSGRCV                       = 401
SYS_MSGCTL                       = 402
SYS_CLOCK_GETTIME64              = 403
SYS_CLOCK_SETTIME64              = 404
SYS_CLOCK_ADJTIME64              = 405
SYS_CLOCK_GETRES_TIME64          = 406
SYS_CLOCK_NANOSLEEP_TIME64       = 407
SYS_TIMER_GETTIME64              = 408
SYS_TIMER_SETTIME64              = 409
SYS_TIMERFD_GETTIME64            = 410
SYS_TIMERFD_SETTIME64            = 411
SYS_UTIMENSAT_TIME64             = 412
SYS_PSELECT6_TIME64              = 413
SYS_PPOLL_TIME64                 = 414
SYS_IO_PGETEVENTS_TIME64         = 416
SYS_RECVMMSG_TIME64              = 417
SYS_MQ_TIMEDSEND_TIME64          = 418
SYS_MQ_TIMEDRECEIVE_TIME64       = 419
SYS_SEMTIMEDOP_TIME64            = 420
SYS_RT_SIGTIMEDWAIT_TIME64       = 421
SYS_FUTEX_TIME64                 = 422
SYS_SCHED_RR_GET_INTERVAL_TIME64 = 423
SYS_PIDFD_SEND_SIGNAL            = 424
SYS_IO_URING_SETUP               = 425
SYS_IO_URING_ENTER               = 426
SYS_IO_URING_REGISTER            = 427
SYS_OPEN_TREE                    = 428
SYS_MOVE_MOUNT                   = 429
SYS_FSOPEN                       = 430
SYS_FSCONFIG                     = 431
SYS_FSMOUNT                      = 432
SYS_FSPICK                       = 433
SYS_PIDFD_OPEN                   = 434
SYS_CLONE3                       = 435
SYS_CLOSE_RANGE                  = 436
SYS_OPENAT2                      = 437
SYS_PIDFD_GETFD                  = 438
SYS_FACCESSAT2                   = 439
SYS_PROCESS_MADVISE              = 440
SYS_EPOLL_PWAIT2                 = 441
 */

static inline qword_t i386_syscall_number(const struct cpu_state *cpu);
static inline void i386_syscall_args(const struct cpu_state *cpu, qword_t args[6]);
static inline void i386_syscall_result(struct cpu_state *cpu, dword_t result);
static inline qword_t amd64_syscall_number(const struct cpu_state *cpu);
static inline void amd64_syscall_args(const struct cpu_state *cpu, qword_t args[6]);
static inline void amd64_syscall_result(struct cpu_state *cpu, dword_t result);
static inline qword_t arm64_syscall_number(const struct cpu_state *cpu);
static inline void arm64_syscall_args(const struct cpu_state *cpu, qword_t args[6]);
static inline void arm64_syscall_result(struct cpu_state *cpu, dword_t result);
static inline qword_t riscv64_syscall_number(const struct cpu_state *cpu);
static inline void riscv64_syscall_args(const struct cpu_state *cpu, qword_t args[6]);
static inline void riscv64_syscall_result(struct cpu_state *cpu, dword_t result);
void handle_syscall_interrupt(struct cpu_state *cpu);

struct syscall_abi_dispatch {
    enum guest_abi abi;
    const char *name;
    const syscall_t *table;
    size_t num_syscalls;
    qword_t (*syscall_number)(const struct cpu_state *cpu);
    void (*syscall_args)(const struct cpu_state *cpu, qword_t args[6]);
    void (*syscall_result)(struct cpu_state *cpu, dword_t result);
};

static syscall_t amd64_syscall_table[470] = {
    // This table covers amd64 syscalls that either reuse an existing i386
    // implementation safely or have a small amd64 shim for 64-bit arg packing.
    [0] = (syscall_t) sys_read,
    [1] = (syscall_t) sys_write,
    [2] = (syscall_t) sys_open,
    [3] = (syscall_t) sys_close,
    [4] = (syscall_t) sys_stat_amd64,
    [5] = (syscall_t) sys_fstat_amd64,
    [6] = (syscall_t) sys_lstat_amd64,
    [7] = (syscall_t) sys_poll,
    [8] = (syscall_t) sys_lseek_amd64,
    [9] = (syscall_t) sys_mmap_amd64,
    [10] = (syscall_t) sys_mprotect,
    [11] = (syscall_t) sys_munmap,
    [12] = (syscall_t) sys_brk,
    [13] = (syscall_t) sys_rt_sigaction,
    [14] = (syscall_t) sys_rt_sigprocmask,
    [15] = (syscall_t) sys_rt_sigreturn,
    [17] = (syscall_t) sys_pread_off_pair,
    [18] = (syscall_t) sys_pwrite_off_pair,
    [16] = (syscall_t) sys_ioctl,
    [19] = (syscall_t) sys_readv_amd64,
    [20] = (syscall_t) sys_writev_amd64,
    [21] = (syscall_t) sys_access,
    [22] = (syscall_t) sys_pipe,
    [23] = (syscall_t) sys_select_amd64,
    [24] = (syscall_t) sys_sched_yield,
    [25] = (syscall_t) sys_mremap,
    [26] = (syscall_t) sys_msync,
    [27] = (syscall_t) sys_mincore,
    [28] = (syscall_t) sys_madvise,
    [29] = (syscall_t) sys_shmget,
    [30] = (syscall_t) sys_shmat,
    [31] = (syscall_t) sys_shmctl,
    [32] = (syscall_t) sys_dup,
    [33] = (syscall_t) sys_dup2,
    [34] = (syscall_t) sys_pause,
    [35] = (syscall_t) sys_nanosleep_amd64,
    [36] = (syscall_t) sys_getitimer_amd64,
    [37] = (syscall_t) sys_alarm,
    [38] = (syscall_t) sys_setitimer_amd64,
    [39] = (syscall_t) sys_getpid,
    [40] = (syscall_t) sys_sendfile,
    [41] = (syscall_t) sys_socket,
    [42] = (syscall_t) sys_connect,
    [43] = (syscall_t) sys_accept,
    [44] = (syscall_t) sys_sendto,
    [45] = (syscall_t) sys_recvfrom,
    [46] = (syscall_t) sys_sendmsg_amd64,
    [47] = (syscall_t) sys_recvmsg_amd64,
    [48] = (syscall_t) sys_shutdown,
    [49] = (syscall_t) sys_bind,
    [50] = (syscall_t) sys_listen,
    [51] = (syscall_t) sys_getsockname,
    [52] = (syscall_t) sys_getpeername,
    [53] = (syscall_t) sys_socketpair,
    [54] = (syscall_t) sys_setsockopt_amd64,
    [55] = (syscall_t) sys_getsockopt_amd64,
    [56] = (syscall_t) sys_clone,
    [57] = (syscall_t) sys_fork,
    [58] = (syscall_t) sys_vfork,
    [59] = (syscall_t) sys_execve,
    [60] = (syscall_t) sys_exit,
    [61] = (syscall_t) sys_wait4,
    [62] = (syscall_t) sys_kill,
    [63] = (syscall_t) sys_uname,
    [67] = (syscall_t) sys_shmdt,
    // SysV semaphores + message queues: dispatch natively (full-width) in
    // handle_amd64_native_memory_syscall; entries pass the NULL check.
    [64] = (syscall_t) sys_semget,
    [65] = (syscall_t) sys_semtimedop, // semop
    [66] = (syscall_t) sys_semctl,
    [220] = (syscall_t) sys_semtimedop,
    [68] = (syscall_t) sys_msgget,
    [69] = (syscall_t) sys_msgsnd,
    [70] = (syscall_t) sys_msgrcv,
    [71] = (syscall_t) sys_msgctl,
    [72] = (syscall_t) sys_fcntl_amd64,
    [73] = (syscall_t) sys_flock,
    [74] = (syscall_t) sys_fsync,
    [75] = (syscall_t) sys_fsync, // fdatasync
    [76] = (syscall_t) sys_truncate_amd64,
    [77] = (syscall_t) sys_ftruncate_amd64,
    [78] = (syscall_t) sys_getdents_amd64,
    [79] = (syscall_t) sys_getcwd,
    [80] = (syscall_t) sys_chdir,
    [81] = (syscall_t) sys_fchdir,
    [82] = (syscall_t) sys_rename,
    [83] = (syscall_t) sys_mkdir,
    [84] = (syscall_t) sys_rmdir,
    [85] = (syscall_t) sys_creat,
    [86] = (syscall_t) sys_link,
    [87] = (syscall_t) sys_unlink,
    [88] = (syscall_t) sys_symlink,
    [89] = (syscall_t) sys_readlink,
    [90] = (syscall_t) sys_chmod,
    [91] = (syscall_t) sys_fchmod,
    [92] = (syscall_t) sys_chown_amd64,
    [93] = (syscall_t) sys_fchown_amd64,
    [94] = (syscall_t) sys_lchown_amd64,
    [95] = (syscall_t) sys_umask,
    [96] = (syscall_t) sys_gettimeofday_amd64,
    [97] = (syscall_t) sys_getrlimit64,
    [98] = (syscall_t) sys_getrusage,
    [99] = (syscall_t) sys_sysinfo,
    [100] = (syscall_t) sys_times,
    [101] = (syscall_t) sys_ptrace,
    [102] = (syscall_t) sys_getuid,
    [103] = (syscall_t) sys_syslog,
    [104] = (syscall_t) sys_getgid,
    [105] = (syscall_t) sys_setuid,
    [106] = (syscall_t) sys_setgid,
    [107] = (syscall_t) sys_geteuid,
    [108] = (syscall_t) sys_getegid,
    [109] = (syscall_t) sys_setpgid,
    [110] = (syscall_t) sys_getppid,
    [111] = (syscall_t) sys_getpgrp,
    [112] = (syscall_t) sys_setsid,
    [113] = (syscall_t) sys_setreuid,
    [114] = (syscall_t) sys_setregid,
    [115] = (syscall_t) sys_getgroups,
    [116] = (syscall_t) sys_setgroups,
    [117] = (syscall_t) sys_setresuid,
    [118] = (syscall_t) sys_getresuid,
    [119] = (syscall_t) sys_setresgid,
    [120] = (syscall_t) sys_getresgid,
    [121] = (syscall_t) sys_getpgid,
    [122] = (syscall_t) sys_setfsuid,
    [123] = (syscall_t) sys_setfsgid,
    [124] = (syscall_t) sys_getsid,
    [125] = (syscall_t) sys_capget,
    [126] = (syscall_t) sys_capset,
    [127] = (syscall_t) sys_rt_sigpending,
    [128] = (syscall_t) sys_rt_sigtimedwait,
    [129] = (syscall_t) sys_rt_sigqueueinfo,
    [130] = (syscall_t) sys_rt_sigsuspend,
    [131] = (syscall_t) sys_sigaltstack,
    [132] = (syscall_t) sys_utime_amd64,
    [133] = (syscall_t) sys_mknod,
    [135] = (syscall_t) sys_personality,
    [137] = (syscall_t) sys_statfs_amd64,
    [138] = (syscall_t) sys_fstatfs_amd64,
    [140] = (syscall_t) sys_getpriority,
    [141] = (syscall_t) sys_setpriority,
    [142] = (syscall_t) sys_sched_setparam,
    [143] = (syscall_t) sys_sched_getparam,
    [144] = (syscall_t) sys_sched_setscheduler,
    [145] = (syscall_t) sys_sched_getscheduler,
    [146] = (syscall_t) sys_sched_get_priority_max,
    [147] = (syscall_t) sys_sched_get_priority_min,
    [148] = (syscall_t) sys_sched_rr_get_interval,
    [149] = (syscall_t) sys_mlock,
    [150] = (syscall_t) sys_munlock,
    [151] = (syscall_t) sys_mlockall,
    [152] = (syscall_t) sys_munlockall,
    [153] = (syscall_t) syscall_success_stub, // vhangup (tty-cleanup no-op)
    [157] = (syscall_t) sys_prctl,
    [158] = (syscall_t) sys_arch_prctl,
    [159] = (syscall_t) sys_adjtimex, // adjtimex (pointer arg goes through the native handler)
    [160] = (syscall_t) sys_setrlimit64,
    [161] = (syscall_t) sys_chroot,
    [162] = (syscall_t) syscall_success_stub, // sync
    [164] = (syscall_t) sys_settimeofday,
    [165] = (syscall_t) sys_mount,
    [166] = (syscall_t) sys_umount2,
    [169] = (syscall_t) sys_reboot,
    [170] = (syscall_t) sys_sethostname,
    [171] = (syscall_t) sys_setdomainname,
    [186] = (syscall_t) sys_gettid,
    [187] = (syscall_t) syscall_success_stub, // readahead
    [188 ... 199] = (syscall_t) sys_xattr_stub,
    [200] = (syscall_t) sys_tkill,
    [201] = (syscall_t) sys_time_amd64,
    [202] = (syscall_t) sys_futex,
    [203] = (syscall_t) sys_sched_setaffinity,
    [204] = (syscall_t) sys_sched_getaffinity,
    // The io_* family, which had NO amd64 entries at all -- so an amd64 guest
    // touching AIO got a "missing syscall" SIGSYS kill rather than an errno,
    // and none of the log line that made this diagnosable on arm64.
    [206] = (syscall_t) sys_io_setup,
    [207] = (syscall_t) sys_io_destroy,
    [208] = (syscall_t) sys_io_getevents,
    [209] = (syscall_t) sys_io_submit,
    [210] = (syscall_t) sys_io_cancel,
    [213] = (syscall_t) sys_epoll_create0,
    [217] = (syscall_t) sys_getdents64,
    [218] = (syscall_t) sys_set_tid_address,
    [221] = (syscall_t) syscall_success_stub, // fadvise64 (advisory; ignored)
    [222] = (syscall_t) sys_timer_create_amd64,
    // amd64 struct itimerspec uses 64-bit fields == the time64 layout; use the
    // 64-bit settime/gettime so the value isn't read/written as 32-bit.
    [223] = (syscall_t) sys_timer_settime64,
    [224] = (syscall_t) sys_timer_gettime64,
    [225] = (syscall_t) sys_timer_getoverrun,
    [226] = (syscall_t) sys_timer_delete,
    [227] = (syscall_t) sys_clock_settime,
    [228] = (syscall_t) sys_clock_gettime_amd64,
    [229] = (syscall_t) sys_clock_getres_amd64,
    [230] = (syscall_t) sys_clock_nanosleep_amd64,
    [231] = (syscall_t) sys_exit_group,
    [232] = (syscall_t) sys_epoll_wait,
    [233] = (syscall_t) sys_epoll_ctl,
    [234] = (syscall_t) sys_tgkill,
    [235] = (syscall_t) sys_utimes_amd64,
    [237] = (syscall_t) sys_mbind,
    [238] = (syscall_t) sys_set_mempolicy,
    [239] = (syscall_t) sys_get_mempolicy,
    [247] = (syscall_t) sys_waitid,
    [250] = (syscall_t) sys_keyctl,
    [251] = (syscall_t) sys_ioprio_set,
    [252] = (syscall_t) sys_ioprio_get,
    [253] = (syscall_t) sys_inotify_init,
    [254] = (syscall_t) sys_inotify_add_watch,
    [255] = (syscall_t) sys_inotify_rm_watch,
    [257] = (syscall_t) sys_openat,
    [258] = (syscall_t) sys_mkdirat,
    [259] = (syscall_t) sys_mknodat,
    [260] = (syscall_t) sys_fchownat,
    [261] = (syscall_t) sys_futimesat_amd64,
    [262] = (syscall_t) sys_newfstatat_amd64,
    [263] = (syscall_t) sys_unlinkat,
    [264] = (syscall_t) sys_renameat,
    [265] = (syscall_t) sys_linkat,
    [266] = (syscall_t) sys_symlinkat,
    [267] = (syscall_t) sys_readlinkat,
    [268] = (syscall_t) sys_fchmodat,
    [269] = (syscall_t) sys_faccessat,
    [270] = (syscall_t) sys_pselect_amd64,
    [271] = (syscall_t) sys_ppoll_amd64,
    [272] = (syscall_t) sys_unshare,
    [273] = (syscall_t) sys_set_robust_list_amd64,
    [274] = (syscall_t) sys_get_robust_list_amd64,
    [275] = (syscall_t) sys_splice,
    [276] = (syscall_t) sys_tee,
    [278] = (syscall_t) sys_vmsplice,
    [277] = (syscall_t) syscall_success_stub, // sync_file_range
    [317] = (syscall_t) syscall_eopnotsupp_stub, // seccomp
    [280] = (syscall_t) sys_utimensat_amd64,
    [281] = (syscall_t) sys_epoll_pwait,
    [282] = (syscall_t) sys_signalfd,
    [283] = (syscall_t) sys_timerfd_create,
    [284] = (syscall_t) sys_eventfd,
    [285] = (syscall_t) sys_fallocate_amd64,
    // amd64 struct itimerspec is the 64-bit layout; route to the 64-bit timerfd
    // settime/gettime so the timer value isn't marshalled as 32-bit.
    [286] = (syscall_t) sys_timerfd_settime64,
    [287] = (syscall_t) sys_timerfd_gettime64,
    [288] = (syscall_t) sys_accept4,
    [289] = (syscall_t) sys_signalfd4,
    [290] = (syscall_t) sys_eventfd2,
    [291] = (syscall_t) sys_epoll_create,
    [292] = (syscall_t) sys_dup3,
    [293] = (syscall_t) sys_pipe2,
    [294] = (syscall_t) sys_inotify_init1,
    [295] = (syscall_t) sys_preadv_amd64,
    [296] = (syscall_t) sys_pwritev_amd64,
    [297] = (syscall_t) sys_rt_tgsigqueueinfo,
    [299] = (syscall_t) sys_recvmmsg_amd64,
    [302] = (syscall_t) sys_prlimit64,
    [303] = (syscall_t) syscall_eopnotsupp_stub, // name_to_handle_at
    [304] = (syscall_t) syscall_eopnotsupp_stub, // open_by_handle_at
    [305] = (syscall_t) sys_clock_adjtime_amd64, // clock_adjtime (EPERM; full-width read-state TODO)
    [306] = (syscall_t) sys_syncfs,
    [307] = (syscall_t) sys_sendmmsg_amd64,
    [309] = (syscall_t) syscall_success_stub, // getcpu
    [310] = (syscall_t) sys_process_vm_readv,
    [312] = (syscall_t) sys_kcmp,
    [316] = (syscall_t) sys_renameat2,
    [318] = (syscall_t) sys_getrandom,
    [319] = (syscall_t) sys_memfd_create,
    [322] = (syscall_t) sys_execveat,
    [324] = (syscall_t) sys_membarrier,
    [326] = (syscall_t) sys_copy_file_range,
    [327] = (syscall_t) sys_preadv2_amd64,
    [328] = (syscall_t) sys_pwritev2_amd64,
    [332] = (syscall_t) sys_statx_amd64,
    [334] = (syscall_t) sys_rseq,
    [403] = (syscall_t) sys_clock_gettime64,
    [404] = (syscall_t) sys_clock_settime64,
    [406] = (syscall_t) sys_clock_getres_time64,
    [407] = (syscall_t) sys_clock_nanosleep_time64,
    [408] = (syscall_t) sys_timer_gettime64,
    [409] = (syscall_t) sys_timer_settime64,
    [410] = (syscall_t) sys_timerfd_gettime64,
    [411] = (syscall_t) sys_timerfd_settime64,
    [412] = (syscall_t) sys_utimensat64,
    [413] = (syscall_t) sys_pselect_time64,
    [414] = (syscall_t) sys_ppoll_time64,
    [417] = (syscall_t) sys_recvmmsg_time64,
    [421] = (syscall_t) sys_rt_sigtimedwait_time64,
    [422] = (syscall_t) sys_futex_time64,
    [424] = (syscall_t) sys_pidfd_send_signal,
    [425] = (syscall_t) syscall_stub_silent, // io_uring_setup
    [426] = (syscall_t) syscall_stub_silent, // io_uring_enter
    [427] = (syscall_t) syscall_stub_silent, // io_uring_register
    [428] = (syscall_t) syscall_stub_silent, // open_tree (new mount API; util-linux falls back to mount(2) on ENOSYS)
    [429] = (syscall_t) sys_move_mount,
    [430] = (syscall_t) sys_fsopen,
    [431] = (syscall_t) sys_fsconfig,
    [432] = (syscall_t) sys_fsmount,
    [433] = (syscall_t) syscall_stub_silent, // fspick
    [434] = (syscall_t) sys_pidfd_open,
    [435] = (syscall_t) sys_clone3,
    [436] = (syscall_t) sys_close_range,
    [437] = (syscall_t) sys_openat2,
    [438] = (syscall_t) syscall_stub_silent, // pidfd_getfd
    [439] = (syscall_t) sys_faccessat,
    [440] = (syscall_t) syscall_stub_silent, // process_madvise
    [441] = (syscall_t) sys_epoll_pwait2,
    // 442-448, 450, 451, 453: same post-5.x-syscall gap as the arm64 table
    // (identical syscall numbers on amd64). Unlike i386, these DO need
    // matching 0-arg entries in amd64_syscall_legacy_arg_count below --
    // amd64 shares arm64's 32-bit dword-fit marshalling hazard for 64-bit
    // guest registers, so a stub without an arg-count override would trade
    // the noisy-but-safe "missing syscall" ENOSYS for a real SIGSYS on
    // garbage upper argument registers (exactly what happened on arm64).
    [442] = (syscall_t) syscall_stub_silent, // mount_setattr
    [443] = (syscall_t) syscall_stub_silent, // quotactl_fd
    [444] = (syscall_t) syscall_stub_silent, // landlock_create_ruleset
    [445] = (syscall_t) syscall_stub_silent, // landlock_add_rule
    [446] = (syscall_t) syscall_stub_silent, // landlock_restrict_self
    [447] = (syscall_t) syscall_stub_silent, // memfd_secret
    [448] = (syscall_t) syscall_stub_silent, // process_mrelease
    [449] = (syscall_t) syscall_stub_silent, // futex_waitv
    [450] = (syscall_t) syscall_stub_silent, // set_mempolicy_home_node
    [451] = (syscall_t) syscall_stub_silent, // cachestat
    [452] = (syscall_t) sys_fchmodat2,
    [453] = (syscall_t) syscall_stub_silent, // map_shadow_stack
    // 454-462: see arm64_syscall_table's matching comment (futex2 family,
    // mount API introspection, LSM self-attr API, memory sealing).
    [454] = (syscall_t) syscall_stub_silent, // futex_wake
    [455] = (syscall_t) syscall_stub_silent, // futex_wait
    [456] = (syscall_t) syscall_stub_silent, // futex_requeue
    [457] = (syscall_t) syscall_stub_silent, // statmount
    [458] = (syscall_t) syscall_stub_silent, // listmount
    [459] = (syscall_t) syscall_stub_silent, // lsm_get_self_attr
    [460] = (syscall_t) syscall_stub_silent, // lsm_set_self_attr
    [461] = (syscall_t) syscall_stub_silent, // lsm_list_modules
    [462] = (syscall_t) syscall_stub_silent, // mseal
    // 463-469: see the i386 table's matching comment (xattrat family,
    // open_tree_attr, file_getattr/file_setattr). 0-arg classifier entries
    // below as usual for this ABI.
    [463] = (syscall_t) syscall_stub_silent, // setxattrat
    [464] = (syscall_t) syscall_stub_silent, // getxattrat
    [465] = (syscall_t) syscall_stub_silent, // listxattrat
    [466] = (syscall_t) syscall_stub_silent, // removexattrat
    [467] = (syscall_t) syscall_stub_silent, // open_tree_attr
    [468] = (syscall_t) syscall_stub_silent, // file_getattr
    [469] = (syscall_t) syscall_stub_silent, // file_setattr
};

// AArch64 Linux syscall table. Numbering (asm-generic based — no
// socketcall()/ipc() multiplexers, 64-bit stat structures only) and the
// real-vs-stub curation are adapted from OpenMinis/ish-arm64's
// kernel/arch/arm64/calls.c (GPLv3, see /CREDITS-aarch64.md); the syscall
// *numbers* themselves are Linux ABI facts, not their expression, but the
// choice of which ~120 of ~450 syscalls are worth real implementations
// reflects real engineering effort (curated against what busybox/musl/
// glibc/Node/Python/git actually call) that's fair to credit even where
// not literally copyrightable.
//
// Function names are iSH-AOK's own, not OpenMinis' (their arm64-only build
// has different function names throughout kernel/). Reused directly from
// the existing i386/amd64_syscall_table entries above wherever a name
// matches by syscall semantics — mostly the plain names (sys_read,
// sys_openat, etc.) and the `_amd64`-suffixed 64-bit-stat/time variants,
// which are named for amd64 but implement the Linux 64-bit ABI shape
// generically, not anything x86-specific; there was nothing to adapt
// there, just direct reuse (see kernel/abi.h's arm64 vm_layout fix for why
// this reuse is safe: the arm64 stack/heap are now kept low like amd64's,
// so these functions' addr_t/dword_t-narrow argument marshalling doesn't
// truncate real guest pointers).
static dword_t sys_riscv_hwprobe(void) {
    return _ENOSYS; // probe fails cleanly; callers use fallback paths
}
static dword_t sys_riscv_flush_icache(void) {
    return 0; // translated blocks are invalidated on guest code writes
}

static syscall_t arm64_syscall_table[470] = {
    // I/O
    [2] = (syscall_t) sys_io_submit,
    [5 ... 16] = (syscall_t) sys_xattr_stub,
    [17]  = (syscall_t) sys_getcwd,
    [19]  = (syscall_t) sys_eventfd2,
    [20]  = (syscall_t) sys_epoll_create,
    [21]  = (syscall_t) sys_epoll_ctl,
    [22]  = (syscall_t) sys_epoll_pwait,
    [23]  = (syscall_t) sys_dup,
    [24]  = (syscall_t) sys_dup3,
    [25]  = (syscall_t) sys_fcntl,
    [26]  = (syscall_t) sys_inotify_init1,
    [27]  = (syscall_t) sys_inotify_add_watch,
    [28]  = (syscall_t) sys_inotify_rm_watch,
    [29]  = (syscall_t) sys_ioctl,
    [30]  = (syscall_t) sys_ioprio_set,
    [31]  = (syscall_t) sys_ioprio_get,
    [32]  = (syscall_t) sys_flock,
    [33]  = (syscall_t) sys_mknodat,
    [34]  = (syscall_t) sys_mkdirat,
    [35]  = (syscall_t) sys_unlinkat,
    [36]  = (syscall_t) sys_symlinkat,
    [37]  = (syscall_t) sys_linkat,
    [38]  = (syscall_t) sys_renameat,
    [39]  = (syscall_t) sys_umount2,
    [40]  = (syscall_t) sys_mount,
    [43]  = (syscall_t) sys_statfs_amd64,
    [44]  = (syscall_t) sys_fstatfs_amd64,
    [45]  = (syscall_t) sys_truncate_amd64,
    [46]  = (syscall_t) sys_ftruncate_amd64,
    [47]  = (syscall_t) sys_fallocate_amd64,
    [48]  = (syscall_t) sys_faccessat,
    [49]  = (syscall_t) sys_chdir,
    [50]  = (syscall_t) sys_fchdir,
    [51]  = (syscall_t) sys_chroot,
    [52]  = (syscall_t) sys_fchmod,
    [53]  = (syscall_t) sys_fchmodat,
    [54]  = (syscall_t) sys_fchownat,
    [55]  = (syscall_t) sys_fchown_amd64,
    [56]  = (syscall_t) sys_openat,
    [57]  = (syscall_t) sys_close,
    [58]  = (syscall_t) syscall_success_stub, // vhangup (tty-cleanup no-op, matches i386/amd64 tables)
    [59]  = (syscall_t) sys_pipe2,
    [61]  = (syscall_t) sys_getdents_amd64,
    [62]  = (syscall_t) sys_lseek_amd64,
    [63]  = (syscall_t) sys_read,
    [64]  = (syscall_t) sys_write,
    [65]  = (syscall_t) sys_readv_amd64,
    [66]  = (syscall_t) sys_writev_amd64,
    [67]  = (syscall_t) sys_pread_off_pair,
    [68]  = (syscall_t) sys_pwrite_off_pair,
    [71]  = (syscall_t) sys_sendfile64,
    [72]  = (syscall_t) sys_pselect_amd64,
    [73]  = (syscall_t) sys_ppoll_amd64,
    [75]  = (syscall_t) sys_vmsplice,
    [76]  = (syscall_t) sys_splice,
    [77]  = (syscall_t) sys_tee,
    [78]  = (syscall_t) sys_readlinkat,
    [79]  = (syscall_t) sys_newfstatat_amd64,
    [80]  = (syscall_t) sys_fstat_amd64,
    [81]  = (syscall_t) syscall_success_stub, // sync
    [82]  = (syscall_t) sys_fsync,
    [83]  = (syscall_t) sys_fsync, // fdatasync (matches i386/amd64 tables' treatment)
    [85]  = (syscall_t) sys_timerfd_create,
    [86]  = (syscall_t) sys_timerfd_settime,
    [87]  = (syscall_t) sys_timerfd_gettime,
    [88]  = (syscall_t) sys_utimensat,
    [90]  = (syscall_t) sys_capget,
    [91]  = (syscall_t) sys_capset,
    [92]  = (syscall_t) sys_personality,
    [93]  = (syscall_t) sys_exit,
    [94]  = (syscall_t) sys_exit_group,
    [95]  = (syscall_t) sys_waitid,
    [96]  = (syscall_t) sys_set_tid_address,
    [97]  = (syscall_t) sys_unshare,
    [98]  = (syscall_t) sys_futex,
    [99]  = (syscall_t) sys_set_robust_list_amd64,
    [100] = (syscall_t) sys_get_robust_list_amd64,
    [101] = (syscall_t) sys_nanosleep_amd64,
    [102] = (syscall_t) sys_getitimer_amd64, // dispatched natively (full-width); entry needed to pass the NULL check
    [103] = (syscall_t) sys_setitimer_amd64,
    [107] = (syscall_t) sys_timer_create_amd64,
    [108] = (syscall_t) sys_timer_gettime,
    [109] = (syscall_t) sys_timer_getoverrun,
    [110] = (syscall_t) sys_timer_settime,
    [111] = (syscall_t) sys_timer_delete,
    [112] = (syscall_t) sys_clock_settime,
    [113] = (syscall_t) sys_clock_gettime_amd64,
    [114] = (syscall_t) sys_clock_getres_amd64,
    [115] = (syscall_t) sys_clock_nanosleep_amd64,
    [116] = (syscall_t) sys_syslog,
    [117] = (syscall_t) sys_ptrace,
    [119] = (syscall_t) sys_sched_setscheduler,
    [120] = (syscall_t) sys_sched_getscheduler,
    [121] = (syscall_t) sys_sched_getparam,
    [122] = (syscall_t) sys_sched_setaffinity,
    [123] = (syscall_t) sys_sched_getaffinity,
    [124] = (syscall_t) sys_sched_yield,
    [125] = (syscall_t) sys_sched_get_priority_max,
    [126] = (syscall_t) sys_sched_get_priority_min,
    [129] = (syscall_t) sys_kill,
    [130] = (syscall_t) sys_tkill,
    [131] = (syscall_t) sys_tgkill,
    [132] = (syscall_t) sys_sigaltstack,
    [133] = (syscall_t) sys_rt_sigsuspend,
    [134] = (syscall_t) sys_rt_sigaction,
    [135] = (syscall_t) sys_rt_sigprocmask,
    [136] = (syscall_t) sys_rt_sigpending,
    [137] = (syscall_t) sys_rt_sigtimedwait,
    [138] = (syscall_t) sys_rt_sigqueueinfo,
    [139] = (syscall_t) sys_rt_sigreturn,
    [140] = (syscall_t) sys_setpriority,
    [141] = (syscall_t) sys_getpriority,
    [142] = (syscall_t) sys_reboot,
    [143] = (syscall_t) sys_setregid,
    [144] = (syscall_t) sys_setgid,
    [145] = (syscall_t) sys_setreuid,
    [146] = (syscall_t) sys_setuid,
    [147] = (syscall_t) sys_setresuid,
    [148] = (syscall_t) sys_getresuid,
    [149] = (syscall_t) sys_setresgid,
    [150] = (syscall_t) sys_getresgid,
    [151] = (syscall_t) sys_setfsuid,
    [152] = (syscall_t) sys_setfsgid,
    [153] = (syscall_t) sys_times,
    [154] = (syscall_t) sys_setpgid,
    [155] = (syscall_t) sys_getpgid,
    [156] = (syscall_t) sys_getsid,
    [157] = (syscall_t) sys_setsid,
    [158] = (syscall_t) sys_getgroups,
    [159] = (syscall_t) sys_setgroups,
    [160] = (syscall_t) sys_uname,
    [161] = (syscall_t) sys_sethostname,
    [163] = (syscall_t) sys_getrlimit64,
    [164] = (syscall_t) sys_setrlimit64,
    [165] = (syscall_t) sys_getrusage,
    [166] = (syscall_t) sys_umask,
    [167] = (syscall_t) sys_prctl,
    [169] = (syscall_t) sys_gettimeofday_amd64,
    [170] = (syscall_t) sys_settimeofday,
    [172] = (syscall_t) sys_getpid,
    [173] = (syscall_t) sys_getppid,
    [174] = (syscall_t) sys_getuid32,
    [175] = (syscall_t) sys_geteuid32,
    [176] = (syscall_t) sys_getgid32,
    [177] = (syscall_t) sys_getegid32,
    [178] = (syscall_t) sys_gettid,
    [179] = (syscall_t) sys_sysinfo,
    // Socket syscalls: aarch64 has no socketcall() multiplexer, unlike i386
    [223] = (syscall_t) syscall_success_stub, // fadvise64 (advisory; ignored)
    // SysV shm (aarch64 direct syscalls; i386 only has ipc(2)). All four
    // dispatch natively (full-width); entries needed to pass the NULL check.
    [194] = (syscall_t) sys_shmget,
    [195] = (syscall_t) sys_shmctl,
    [196] = (syscall_t) sys_shmat,
    [197] = (syscall_t) sys_shmdt,
    // OpenMinis-parity sweep: every aarch64 syscall their table covers
    // (plus fchmodat2), so guests get a real implementation or a clean
    // errno instead of a "missing syscall" SIGSYS kill. All of these
    // dispatch natively (full-width); the entries exist to pass the
    // NULL check. syscall_stub entries keep the "stub" log visible.
    [0] = (syscall_t) sys_io_setup,
    [1] = (syscall_t) sys_io_destroy,
    [3] = (syscall_t) sys_io_cancel,
    [4] = (syscall_t) sys_io_getevents,
    [18] = (syscall_t) syscall_stub, // lookup_dcookie
    [41] = (syscall_t) syscall_stub, // pivot_root
    [42] = (syscall_t) syscall_stub, // nfsservctl
    [60] = (syscall_t) syscall_stub, // quotactl
    [69] = (syscall_t) sys_preadv_guest, // preadv
    [70] = (syscall_t) sys_pwritev_guest, // pwritev
    [74] = (syscall_t) syscall_stub_silent, // signalfd4
    [75] = (syscall_t) syscall_stub, // vmsplice
    [77] = (syscall_t) syscall_stub, // tee
    [84] = (syscall_t) syscall_stub_silent, // sync_file_range
    [89] = (syscall_t) syscall_stub, // acct
    [104] = (syscall_t) syscall_stub, // kexec_load
    [105] = (syscall_t) syscall_stub, // init_module
    [106] = (syscall_t) syscall_stub, // delete_module
    [118] = (syscall_t) sys_sched_setparam, // sched_setparam
    [127] = (syscall_t) sys_sched_rr_get_interval, // sched_rr_get_interval
    [128] = (syscall_t) syscall_stub, // restart_syscall
    [162] = (syscall_t) sys_setdomainname, // setdomainname
    [168] = (syscall_t) syscall_stub_silent, // getcpu
    [171] = (syscall_t) sys_adjtimex, // adjtimex
    [180] = (syscall_t) syscall_stub, // mq_open
    [181] = (syscall_t) syscall_stub, // mq_unlink
    [182] = (syscall_t) syscall_stub, // mq_timedsend
    [183] = (syscall_t) syscall_stub, // mq_timedreceive
    [184] = (syscall_t) syscall_stub, // mq_notify
    [185] = (syscall_t) syscall_stub, // mq_getsetattr
    // SysV message queues (fakeroot's faked<->libfakeroot IPC, so every
    // makepkg/dpkg-buildpackage package() stage). Dispatch natively
    // (full-width) like shm above; entries pass the NULL check.
    [186] = (syscall_t) sys_msgget,
    [187] = (syscall_t) sys_msgctl,
    [188] = (syscall_t) sys_msgrcv,
    [189] = (syscall_t) sys_msgsnd,
    // SysV semaphores (fakeroot again); native dispatch like msg above.
    [190] = (syscall_t) sys_semget,
    [191] = (syscall_t) sys_semctl,
    [192] = (syscall_t) sys_semtimedop,
    [193] = (syscall_t) sys_semtimedop, // semop = semtimedop without timeout
    [213] = (syscall_t) syscall_stub_silent, // readahead
    // add_key: PAM's pam_keyinit (and other keyutils-linked libs) call this
    // during ordinary process startup for session-keyring setup; every
    // caller treats ENOSYS as "keyrings unsupported" and continues, so this
    // is one of the benign-fallback stubs from the comment above, not a
    // functionality gap -- seen spamming "ERROR: arm64 stub syscall 217" on
    // every single spawned process (mount/udevadm/journalctl/userdbctl) during
    // a systemd boot.
    [217] = (syscall_t) syscall_stub_silent, // add_key
    [218] = (syscall_t) syscall_stub, // request_key
    [219] = (syscall_t) sys_keyctl, // keyctl -- matches i386 (288)/amd64 (250)
    [224] = (syscall_t) syscall_stub, // swapon
    [225] = (syscall_t) syscall_stub, // swapoff
    [234] = (syscall_t) syscall_stub, // remap_file_pages
    [236] = (syscall_t) syscall_stub_silent, // get_mempolicy
    [237] = (syscall_t) syscall_stub_silent, // set_mempolicy
    [238] = (syscall_t) syscall_stub, // migrate_pages
    [239] = (syscall_t) syscall_stub, // move_pages
    [240] = (syscall_t) syscall_stub_silent, // rt_tgsigqueueinfo
    [241] = (syscall_t) syscall_stub, // perf_event_open
    [262] = (syscall_t) syscall_stub, // fanotify_init
    [263] = (syscall_t) syscall_stub, // fanotify_mark
    [264] = (syscall_t) syscall_stub_silent, // name_to_handle_at
    [265] = (syscall_t) syscall_stub_silent, // open_by_handle_at
    [266] = (syscall_t) syscall_stub_silent, // clock_adjtime
    [267] = (syscall_t) sys_syncfs,
    [268] = (syscall_t) syscall_stub, // setns
    [270] = (syscall_t) syscall_stub_silent, // process_vm_readv
    [271] = (syscall_t) syscall_stub, // process_vm_writev
    [272] = (syscall_t) sys_kcmp,
    [273] = (syscall_t) syscall_stub, // finit_module
    [274] = (syscall_t) syscall_stub, // sched_setattr
    [275] = (syscall_t) syscall_stub, // sched_getattr
    [277] = (syscall_t) syscall_stub_silent, // seccomp
    // bpf: systemd probes this at unit-start time for cgroup BPF device/IP
    // accounting programs and explicitly falls back (logging its own
    // "BPF firewalling not supported by kernel" diagnostic) on ENOSYS --
    // another benign-fallback stub, not a functionality gap. Seen as PID 1
    // spamming "ERROR: arm64 stub syscall 280" 8x in a row during boot.
    [280] = (syscall_t) syscall_stub_silent, // bpf
    [282] = (syscall_t) syscall_stub, // userfaultfd
    [284] = (syscall_t) syscall_stub, // mlock2
    [286] = (syscall_t) sys_preadv2_guest, // preadv2
    [287] = (syscall_t) sys_pwritev2_guest, // pwritev2
    [288] = (syscall_t) syscall_stub, // pkey_mprotect
    [289] = (syscall_t) syscall_stub, // pkey_alloc
    [290] = (syscall_t) syscall_stub, // pkey_free
    // rseq (293) and kexec_file_load (294) are covered by the [292 ... 423]
    // silent-stub range below; not listed individually (it would re-init them).
    // pidfd/io_uring/process_madvise: handle_asm_generic_native_syscall already
    // returns a clean ENOSYS for every one of these (its "clean ENOSYS" case
    // group shadows the table before the entry is ever called), so the table
    // slot here governs *logging only*. Use the SILENT stub to match the
    // amd64/i386 tables: these are deliberate ENOSYS returns that real
    // software falls back from (pidfd → classic wait4/kill, io_uring → epoll,
    // process_madvise → best-effort no-op), so a benign call must not spew
    // "ERROR: arm64 stub syscall N" on every invocation. go build's os/exec
    // subprocess-wait hit 434 (pidfd_open) and logged an ERROR per child even
    // though the build/run completes correctly via the wait4 fallback.
    [424] = (syscall_t) sys_pidfd_send_signal,
    [425] = (syscall_t) syscall_stub_silent, // io_uring_setup
    [426] = (syscall_t) syscall_stub_silent, // io_uring_enter
    [427] = (syscall_t) syscall_stub_silent, // io_uring_register
    [428] = (syscall_t) syscall_stub_silent, // open_tree (new mount API; util-linux falls back to mount(2) on ENOSYS)
    [429] = (syscall_t) sys_move_mount,
    [430] = (syscall_t) sys_fsopen,
    [431] = (syscall_t) sys_fsconfig,
    [432] = (syscall_t) sys_fsmount,
    [433] = (syscall_t) syscall_stub_silent, // fspick
    [434] = (syscall_t) sys_pidfd_open,
    [438] = (syscall_t) syscall_stub_silent, // pidfd_getfd
    [440] = (syscall_t) syscall_stub_silent, // process_madvise
    [452] = (syscall_t) syscall_stub_silent, // fchmodat2
    [198] = (syscall_t) sys_socket,
    [199] = (syscall_t) sys_socketpair,
    [200] = (syscall_t) sys_bind,
    [201] = (syscall_t) sys_listen,
    [202] = (syscall_t) sys_accept,
    [203] = (syscall_t) sys_connect,
    [204] = (syscall_t) sys_getsockname,
    [205] = (syscall_t) sys_getpeername,
    [206] = (syscall_t) sys_sendto,
    [207] = (syscall_t) sys_recvfrom,
    [208] = (syscall_t) sys_setsockopt_amd64,
    [209] = (syscall_t) sys_getsockopt_amd64,
    [210] = (syscall_t) sys_shutdown,
    [211] = (syscall_t) sys_sendmsg_amd64,
    [212] = (syscall_t) sys_recvmsg_amd64,
    [214] = (syscall_t) sys_brk,
    [215] = (syscall_t) sys_munmap,
    [216] = (syscall_t) sys_mremap,
    [220] = (syscall_t) sys_clone,
    [221] = (syscall_t) sys_execve,
    [222] = (syscall_t) sys_mmap_amd64,
    [226] = (syscall_t) sys_mprotect,
    [227] = (syscall_t) sys_msync,
    [228] = (syscall_t) sys_mlock,
    [229] = (syscall_t) sys_munlock, // dispatched natively (full-width); entry needed to pass the NULL check
    [230] = (syscall_t) sys_mlockall,
    [231] = (syscall_t) sys_munlockall,
    [232] = (syscall_t) sys_mincore,
    [233] = (syscall_t) sys_madvise,
    [235] = (syscall_t) sys_mbind,
    [242] = (syscall_t) sys_accept4,
    [243] = (syscall_t) sys_recvmmsg_amd64,
    // riscv-arch-specific range (244-259; unallocated on aarch64, so these
    // entries are safe in the shared asm-generic table — only a riscv64
    // task can ever reach them). Real functions, not NULL: a NULL entry
    // takes the missing-syscall path before the native handler runs.
    [258] = (syscall_t) sys_riscv_hwprobe,      // ENOSYS: glibc/apk fall back
    [259] = (syscall_t) sys_riscv_flush_icache, // JIT invalidates on guest writes: no-op
    [260] = (syscall_t) sys_wait4,
    [261] = (syscall_t) sys_prlimit64,
    [269] = (syscall_t) sys_sendmmsg_amd64,
    [276] = (syscall_t) sys_renameat2,
    [278] = (syscall_t) sys_getrandom,
    [279] = (syscall_t) syscall_stub_silent, // memfd_create — matches i386/amd64 tables' treatment (see sys_memfd_create's own callers elsewhere for why it's silenced)
    [281] = (syscall_t) sys_execveat,
    [283] = (syscall_t) syscall_stub_silent, // membarrier
    [285] = (syscall_t) sys_copy_file_range,
    [291] = (syscall_t) sys_statx_amd64,
    // Linux 5.x+ syscalls — mostly stubbed, letting glibc/musl fall back to
    // older ABI paths, matching this file's existing amd64 table policy.
    [292 ... 423] = (syscall_t) syscall_stub_silent,
    // 424-434 (io_uring_*, pidfd_*, new-mount-API) are set individually above;
    // no [425 ... 427] range here -- it would redundantly re-init those slots.
    [435] = (syscall_t) sys_clone3,
    [436] = (syscall_t) sys_close_range,
    [437] = (syscall_t) sys_openat2,
    [439] = (syscall_t) sys_faccessat, // faccessat2 reuses sys_faccessat, matches i386/amd64 tables
    [441] = (syscall_t) sys_epoll_pwait2,
    [449] = (syscall_t) syscall_stub_silent, // futex_waitv (native ENOSYS; glibc/musl fall back to plain FUTEX_WAIT)
    // 442-448, 450, 451, 453: real modern syscalls (mount_setattr, quotactl_fd,
    // landlock_*, memfd_secret, process_mrelease, set_mempolicy_home_node,
    // cachestat, map_shadow_stack) with no table entry at all -- falling
    // through to the generic "missing syscall" path, which is a clean ENOSYS
    // but logs a loud ERROR per call (stress-ng's landlock/pidfd/mem stressors
    // hit these routinely). Give them explicit silent-stub slots so callers
    // that already handle ENOSYS (all of the above are optional/best-effort
    // in real userspace) get it quietly, matching the rest of this table's
    // policy for post-5.x syscalls.
    [442] = (syscall_t) syscall_stub_silent, // mount_setattr
    [443] = (syscall_t) syscall_stub_silent, // quotactl_fd
    [444] = (syscall_t) syscall_stub_silent, // landlock_create_ruleset
    [445] = (syscall_t) syscall_stub_silent, // landlock_add_rule
    [446] = (syscall_t) syscall_stub_silent, // landlock_restrict_self
    [447] = (syscall_t) syscall_stub_silent, // memfd_secret
    [448] = (syscall_t) syscall_stub_silent, // process_mrelease
    [450] = (syscall_t) syscall_stub_silent, // set_mempolicy_home_node
    [451] = (syscall_t) syscall_stub_silent, // cachestat
    [453] = (syscall_t) syscall_stub_silent, // map_shadow_stack
    // 454-462: next batch of post-6.x syscalls (futex2 family, mount API
    // introspection, LSM self-attr API, memory sealing). Same treatment as
    // 442-453 above -- silent ENOSYS stubs with 0-arg classifier entries
    // below, not the "way out of table bounds" SIGSYS path (observed:
    // stress-ng's landlock/security probes hit mseal/462 this way).
    [454] = (syscall_t) syscall_stub_silent, // futex_wake
    [455] = (syscall_t) syscall_stub_silent, // futex_wait
    [456] = (syscall_t) syscall_stub_silent, // futex_requeue
    [457] = (syscall_t) syscall_stub_silent, // statmount
    [458] = (syscall_t) syscall_stub_silent, // listmount
    [459] = (syscall_t) syscall_stub_silent, // lsm_get_self_attr
    [460] = (syscall_t) syscall_stub_silent, // lsm_set_self_attr
    [461] = (syscall_t) syscall_stub_silent, // lsm_list_modules
    [462] = (syscall_t) syscall_stub_silent, // mseal
    // 463-469: see the i386 table's matching comment (xattrat family,
    // open_tree_attr, file_getattr/file_setattr). This is the table that
    // took the observed hit: Arch aarch64 useradd -m probing getxattrat/464
    // fell off the old [463] bound into the SIGSYS "missing syscall" path.
    // 0-arg classifier entries below as usual for this ABI (riscv64 shares
    // both this table and the classifier).
    [463] = (syscall_t) syscall_stub_silent, // setxattrat
    [464] = (syscall_t) syscall_stub_silent, // getxattrat
    [465] = (syscall_t) syscall_stub_silent, // listxattrat
    [466] = (syscall_t) syscall_stub_silent, // removexattrat
    [467] = (syscall_t) syscall_stub_silent, // open_tree_attr
    [468] = (syscall_t) syscall_stub_silent, // file_getattr
    [469] = (syscall_t) syscall_stub_silent, // file_setattr
};

static const struct syscall_abi_dispatch i386_syscall_dispatch = {
    .abi = GUEST_ABI_I386,
    .name = "i386",
    .table = i386_syscall_table,
    .num_syscalls = sizeof(i386_syscall_table) / sizeof(i386_syscall_table[0]),
    .syscall_number = i386_syscall_number,
    .syscall_args = i386_syscall_args,
    .syscall_result = i386_syscall_result,
};

static const struct syscall_abi_dispatch amd64_syscall_dispatch = {
    .abi = GUEST_ABI_AMD64,
    .name = "amd64",
    .table = amd64_syscall_table,
    .num_syscalls = sizeof(amd64_syscall_table) / sizeof(amd64_syscall_table[0]),
    .syscall_number = amd64_syscall_number,
    .syscall_args = amd64_syscall_args,
    .syscall_result = amd64_syscall_result,
};

static const struct syscall_abi_dispatch arm64_syscall_dispatch = {
    .abi = GUEST_ABI_ARM64,
    .name = "arm64",
    .table = arm64_syscall_table,
    .num_syscalls = sizeof(arm64_syscall_table) / sizeof(arm64_syscall_table[0]),
    .syscall_number = arm64_syscall_number,
    .syscall_args = arm64_syscall_args,
    .syscall_result = arm64_syscall_result,
};

// riscv64 shares arm64's syscall table: both are asm-generic ABIs with
// identical numbering, and the entries reuse the same sys_* implementations
// either way. Only the register plumbing (a7/a0-a5/a0 vs x8/x0-x5/x0) and
// the riscv-arch-specific numbers (258 riscv_hwprobe / 259
// riscv_flush_icache, intercepted in handle_asm_generic_native_syscall)
// differ. If the tables ever need to diverge for real, split them then.
static const struct syscall_abi_dispatch riscv64_syscall_dispatch = {
    .abi = GUEST_ABI_RISCV64,
    .name = "riscv64",
    .table = arm64_syscall_table,
    .num_syscalls = sizeof(arm64_syscall_table) / sizeof(arm64_syscall_table[0]),
    .syscall_number = riscv64_syscall_number,
    .syscall_args = riscv64_syscall_args,
    .syscall_result = riscv64_syscall_result,
};

static const struct syscall_abi_dispatch *syscall_dispatch_for_abi(enum guest_abi abi) {
    switch (abi) {
    case GUEST_ABI_AMD64:
        return &amd64_syscall_dispatch;
    case GUEST_ABI_ARM64:
        return &arm64_syscall_dispatch;
    case GUEST_ABI_RISCV64:
        return &riscv64_syscall_dispatch;
    case GUEST_ABI_I386:
    default:
        return &i386_syscall_dispatch;
    }
}

void dump_stack(int lines);

static bool syscall_is_logged_stub(syscall_t syscall) {
    return syscall == (syscall_t) syscall_stub;
}

static bool syscall_result_is_errno(dword_t result) {
    sdword_t signed_result = (sdword_t) result;
    return signed_result < 0 && signed_result >= -4095;
}

static bool syscall_result_should_restart(dword_t result) {
    sdword_t r = (sdword_t) result;
    if (r != _ERESTART && r != _ERESTART_NOHAND) {
        if (current != NULL)
            current->restart_nohand_pending = false;
        return false;
    }
    // Remember which flavour, for the handler-setup path: an _ERESTART_NOHAND
    // restart is cancelled by a handler running before the syscall re-executes
    // (Linux's ERESTARTNOHAND), an _ERESTART one is not. Recorded here because
    // this predicate is checked immediately before every PC rewind.
    if (current != NULL)
        current->restart_nohand_pending = (r == _ERESTART_NOHAND);
    return true;
}

// Undo prepare_syscall_restart: step the PC forward over the syscall
// instruction again and hand the guest EINTR instead. Linux does exactly this
// in handle_signal() when the pending restart is an ERESTARTNOHAND one and a
// handler is about to run.
void cancel_syscall_restart(void) {
    struct cpu_state *cpu = &current->cpu;
    switch (current->abi) {
        case GUEST_ABI_AMD64:
            cpu->amd64_rip += 2;
            cpu->eip = (dword_t) cpu->amd64_rip;
            cpu->amd64_regs[amd64_rax] = (qword_t) (sqword_t) _EINTR;
            cpu->eax = (dword_t) _EINTR;
            break;
        case GUEST_ABI_ARM64:
            cpu->arm64_pc += 4;
            cpu->arm64_regs[arm64_x0] = (qword_t) (sqword_t) _EINTR;
            break;
        case GUEST_ABI_RISCV64:
            cpu->riscv64_pc += 4;
            cpu->riscv64_regs[riscv64_a0] = (qword_t) (sqword_t) _EINTR;
            break;
        case GUEST_ABI_I386:
        default:
            cpu->eip += 2;
            cpu->eax = (dword_t) _EINTR;
            break;
    }
}

static sdword_t syscall_result_errno(dword_t result) {
    return (sdword_t) result;
}

static void prepare_syscall_restart(struct cpu_state *cpu, const struct syscall_abi_dispatch *dispatch,
                                    qword_t syscall_num, qword_t orig_arg0) {
    if (dispatch->abi == GUEST_ABI_AMD64) {
        cpu->amd64_regs[amd64_rax] = syscall_num;
        cpu->eax = (dword_t) syscall_num;
        cpu->amd64_rip -= 2;
        cpu->eip = (dword_t) cpu->amd64_rip;
        return;
    }
    if (dispatch->abi == GUEST_ABI_ARM64) {
        // X8 still holds the syscall number (nothing clobbers it across
        // the SVC); rewind PC over the 4-byte SVC to re-execute it.
        // X0 is BOTH the return register and the first-argument register
        // on arm64 (unlike i386/amd64, where restoring eax/rax to the
        // syscall number suffices) — anything that wrote a result before
        // the restart decision leaves the restarted syscall running with
        // x0 = -ERESTART* as its first argument. The ptrace syscall-exit
        // stop does exactly that (the tracer must see the -ERESTART
        // result, mirroring Linux, which then restores orig_x0 just like
        // this): `strace cmake` died on libuv's fd >= 0 assert because a
        // restarted epoll_pwait got the exposed result as its epoll fd.
        // Restore the original argument unconditionally, like orig_x0.
        cpu->arm64_regs[arm64_x8] = syscall_num;
        cpu->arm64_regs[arm64_x0] = orig_arg0;
        cpu->arm64_pc -= 4;
        return;
    }
    if (dispatch->abi == GUEST_ABI_RISCV64) {
        // Same shape as arm64 above: a0 is both the return register and
        // the first-argument register, so restore the original argument
        // unconditionally (orig_a0). ECALL has no compressed form, so the
        // rewind is always 4 bytes.
        cpu->riscv64_regs[riscv64_a7] = syscall_num;
        cpu->riscv64_regs[riscv64_a0] = orig_arg0;
        cpu->riscv64_pc -= 4;
        return;
    }

    cpu->eax = (dword_t) syscall_num;
    cpu->eip -= 2;
}

static inline qword_t i386_syscall_number(const struct cpu_state *cpu) {
    return cpu->eax;
}

static inline void i386_syscall_args(const struct cpu_state *cpu, qword_t args[6]) {
    args[0] = cpu->ebx;
    args[1] = cpu->ecx;
    args[2] = cpu->edx;
    args[3] = cpu->esi;
    args[4] = cpu->edi;
    args[5] = cpu->ebp;
}

static inline void i386_syscall_result(struct cpu_state *cpu, dword_t result) {
    cpu->eax = result;
}

static inline qword_t amd64_syscall_number(const struct cpu_state *cpu) {
    return cpu->amd64_regs[amd64_rax];
}

static inline void amd64_syscall_args(const struct cpu_state *cpu, qword_t args[6]) {
    args[0] = cpu->amd64_regs[amd64_rdi];
    args[1] = cpu->amd64_regs[amd64_rsi];
    args[2] = cpu->amd64_regs[amd64_rdx];
    args[3] = cpu->amd64_regs[amd64_r10];
    args[4] = cpu->amd64_regs[amd64_r8];
    args[5] = cpu->amd64_regs[amd64_r9];
}

static inline void amd64_syscall_result(struct cpu_state *cpu, dword_t result) {
    if (syscall_result_is_errno(result))
        cpu->amd64_regs[amd64_rax] = (qword_t) (sqword_t) syscall_result_errno(result);
    else
        cpu->amd64_regs[amd64_rax] = (qword_t) result;
    cpu->eax = result;
    cpu->amd64_regs[amd64_rcx] = cpu->amd64_rip;
    cpu->amd64_regs[amd64_r11] = cpu->eflags;
}

// AArch64 syscall ABI: number in X8, args in X0-X5, result in X0. No
// rcx/r11-clobber equivalent — that's a side effect of the x86 SYSCALL
// instruction specifically (amd64_syscall_result mirrors it because real
// hardware does it), not something ARM's SVC exception entry does.
static inline qword_t arm64_syscall_number(const struct cpu_state *cpu) {
    return cpu->arm64_regs[arm64_x8];
}

static inline void arm64_syscall_args(const struct cpu_state *cpu, qword_t args[6]) {
    args[0] = cpu->arm64_regs[arm64_x0];
    args[1] = cpu->arm64_regs[arm64_x1];
    args[2] = cpu->arm64_regs[arm64_x2];
    args[3] = cpu->arm64_regs[arm64_x3];
    args[4] = cpu->arm64_regs[arm64_x4];
    args[5] = cpu->arm64_regs[arm64_x5];
}

static inline void arm64_syscall_result(struct cpu_state *cpu, dword_t result) {
    if (syscall_result_is_errno(result))
        cpu->arm64_regs[arm64_x0] = (qword_t) (sqword_t) syscall_result_errno(result);
    else
        cpu->arm64_regs[arm64_x0] = (qword_t) result;
}

// riscv64: number in a7, args a0-a5, result a0. Same asm-generic
// conventions as arm64 modulo register names (a0 has x0's dual
// first-arg/return role; see prepare_syscall_restart).
static inline qword_t riscv64_syscall_number(const struct cpu_state *cpu) {
    return cpu->riscv64_regs[riscv64_a7];
}

static inline void riscv64_syscall_args(const struct cpu_state *cpu, qword_t args[6]) {
    args[0] = cpu->riscv64_regs[riscv64_a0];
    args[1] = cpu->riscv64_regs[riscv64_a1];
    args[2] = cpu->riscv64_regs[riscv64_a2];
    args[3] = cpu->riscv64_regs[riscv64_a3];
    args[4] = cpu->riscv64_regs[riscv64_a4];
    args[5] = cpu->riscv64_regs[riscv64_a5];
}

static inline void riscv64_syscall_result(struct cpu_state *cpu, dword_t result) {
    if (syscall_result_is_errno(result))
        cpu->riscv64_regs[riscv64_a0] = (qword_t) (sqword_t) syscall_result_errno(result);
    else
        cpu->riscv64_regs[riscv64_a0] = (qword_t) result;
}

static inline void amd64_syscall_result_qword(struct cpu_state *cpu, qword_t result) {
    sqword_t signed_result = (sqword_t) result;
    sdword_t signed_result32 = (sdword_t) (dword_t) result;
    if (signed_result < 0 && signed_result >= -4095)
        cpu->amd64_regs[amd64_rax] = (qword_t) signed_result;
    else if ((result >> 32) == 0 && signed_result32 < 0 && signed_result32 >= -4095)
        cpu->amd64_regs[amd64_rax] = (qword_t) (sqword_t) signed_result32;
    else
        cpu->amd64_regs[amd64_rax] = result;
    cpu->eax = (dword_t) cpu->amd64_regs[amd64_rax];
    cpu->amd64_regs[amd64_rcx] = cpu->amd64_rip;
    cpu->amd64_regs[amd64_r11] = cpu->eflags;
}

static bool syscall_arg_fits_legacy_dword(qword_t arg) {
    return arg <= UINT32_MAX || (arg >> 32) == UINT32_MAX;
}

// Set while syscall_dispatch_native (below) is running a syscall on behalf of
// a natively-compiled program -- host code with no guest register file to
// write a result into. The two places that would touch `cpu` divert here
// instead, which is what lets the whole asm-generic switch be reused unchanged
// rather than duplicated for native callers.
static __thread bool native_syscall_active;
static __thread qword_t native_syscall_value;

// Full-width result write for the asm-generic 64-bit ABIs served by
// handle_asm_generic_native_syscall below: arm64's x0 or riscv64's a0.
static inline void asm_generic_syscall_result_qword(struct cpu_state *cpu, qword_t result) {
    if (native_syscall_active) {
        native_syscall_value = result;
        return;
    }
    if (current->abi == GUEST_ABI_RISCV64)
        cpu->riscv64_regs[riscv64_a0] = result;
    else
        cpu->arm64_regs[arm64_x0] = result;
}

// AArch64 open(2) flags -> this codebase's internal (i386-valued) flags.
// The generic 64-bit ABI moves four O_ constants relative to x86:
//   aarch64 O_DIRECTORY = 0x04000   x86/internal = 0x10000
//   aarch64 O_NOFOLLOW  = 0x08000   x86/internal = 0x20000
//   aarch64 O_DIRECT    = 0x10000   x86/internal = 0x04000
//   aarch64 O_LARGEFILE = 0x20000   x86/internal = 0x08000
// Without this, EVERY musl-aarch64 open() — which unconditionally ORs in
// O_LARGEFILE — carried what the fs layer reads as O_NOFOLLOW_, so any
// open through a final-component symlink returned ELOOP. First seen on a
// real iPad as apk failing to load libz.so.1 ("Symbolic link loop").
//
// Checked against OpenMinis (the usual first stop, see
// /CREDITS-aarch64.md): their calls_arm64.c has NO translation — raw
// aarch64 flags flow into the same x86-valued fs layer. It's latent
// there because upstream iSH ignores O_NOFOLLOW; it bites here because
// iSH-AOK's fs layer enforces O_NOFOLLOW->ELOOP (fs conformance work).
// Their tree also silently drops genuine O_DIRECTORY/O_NOFOLLOW from
// aarch64 guests for the same reason.
dword_t arm64_open_flags_to_internal(qword_t flags) {
    dword_t out = (dword_t) flags & ~0x3c000u;
    if (flags & 0x04000) out |= 0x10000; // O_DIRECTORY
    if (flags & 0x08000) out |= 0x20000; // O_NOFOLLOW
    if (flags & 0x10000) out |= 0x04000; // O_DIRECT
    if (flags & 0x20000) out |= 0x08000; // O_LARGEFILE (ignored downstream)
    return out;
}

// Exact inverse of the above, for results that carry open flags back OUT to
// an aarch64 guest (fcntl F_GETFL). Same four relocated constants, mirrored.
static dword_t internal_to_arm64_open_flags(dword_t flags) {
    dword_t out = flags & ~0x3c000u;
    if (flags & 0x10000) out |= 0x04000; // O_DIRECTORY
    if (flags & 0x20000) out |= 0x08000; // O_NOFOLLOW
    if (flags & 0x04000) out |= 0x10000; // O_DIRECT
    if (flags & 0x08000) out |= 0x20000; // O_LARGEFILE
    return out;
}

// fcntl file-status commands: values are ABI-universal (fcntl.h) and defined
// privately in fs/fd.c; mirrored here for the arm64 flag translation in
// handle_asm_generic_native_syscall's case 25.
#define F_GETFL_ 3
#define F_SETFL_ 4

// AArch64 full-width syscall dispatch: the arm64 counterpart of
// handle_amd64_native_memory_syscall below, for the same reason — the
// legacy syscall_t signature marshals every argument through dword_t,
// silently truncating 64-bit guest pointers, and arm64 binaries live
// well above 4 GiB (the loader places PIEs high in the 47-bit mmap
// window; kernel/abi.h). Same sys_*_guest implementations the amd64
// path uses (they're 64-bit-ABI-shaped, not x86-shaped — see the
// arm64_syscall_table comment), keyed by AArch64 (asm-generic) syscall
// numbers. Note clone's argument order: AArch64 follows the generic
// (flags, stack, parent_tid, tls, child_tid) order — tls and child_tid
// are SWAPPED relative to amd64's — so the sys_clone_guest call below
// deliberately differs from the amd64 case's argument positions.
//
// dword-returning cases funnel through the shared restart/sign-extend
// tail; the three address-returning calls (brk/mmap/mremap) and lseek
// set the full 64-bit result directly.
static bool handle_asm_generic_native_syscall(struct cpu_state *cpu, qword_t syscall_num,
        const qword_t raw_args[6]) {
    dword_t result;
    switch (syscall_num) {
    // -- full-width results --
    case 139: { // rt_sigreturn: restores the whole register file; only
                // overwrite X0 on error (mirrors amd64's case-15 handling)
        qword_t sigreturn_result;
        if (current->abi == GUEST_ABI_RISCV64)
            sigreturn_result = sys_rt_sigreturn_riscv64();
        else
            sigreturn_result = sys_rt_sigreturn_arm64();
        sqword_t signed_result = (sqword_t) sigreturn_result;
        if (signed_result < 0 && signed_result >= -4095)
            asm_generic_syscall_result_qword(cpu, sigreturn_result);
        return true;
    }
    case 62: // lseek
        asm_generic_syscall_result_qword(cpu, (qword_t) sys_lseek_amd64_guest(
                (fd_t) raw_args[0], (off_t_) raw_args[1], (dword_t) raw_args[2]));
        return true;
    case 214: // brk
        asm_generic_syscall_result_qword(cpu, sys_brk_guest(raw_args[0]));
        return true;
    case 196: // shmat: returns an address
        asm_generic_syscall_result_qword(cpu, (qword_t) sys_shmat_guest(
                (int_t) raw_args[0], raw_args[1], (int_t) raw_args[2]));
        return true;
    case 216: // mremap
        asm_generic_syscall_result_qword(cpu, sys_mremap_guest(raw_args[0], raw_args[1],
                raw_args[2], (dword_t) raw_args[3], raw_args[4]));
        return true;
    case 222: // mmap
        asm_generic_syscall_result_qword(cpu, sys_mmap_guest(raw_args[0], raw_args[1],
                (dword_t) raw_args[2], (dword_t) raw_args[3], (fd_t) raw_args[4],
                raw_args[5]));
        return true;

    // -- dword results (shared restart/sign-extend tail below) --
    case 17: result = sys_getcwd_guest(raw_args[0], (dword_t) raw_args[1]); break;
    // fsopen/fsconfig/move_mount (new mount API, fs/mount.c) carry real
    // 64-bit guest pointer args (fsname/key/value/from_path/to_path) that
    // the legacy marshalled dispatch's dword-fit check would reject on a
    // typical arm64 mmap address (e.g. 0x7fff...) -- route them natively,
    // same reasoning as getcwd's path buffer above. fsmount has no pointer
    // args and stays on the plain legacy path (see its arg-count entries).
    case 430: // fsopen(fsname, flags)
        result = (dword_t) sys_fsopen_guest(raw_args[0], (dword_t) raw_args[1]); break;
    case 431: // fsconfig(fd, cmd, key, value, aux)
        result = (dword_t) sys_fsconfig_guest((fd_t) raw_args[0], (dword_t) raw_args[1],
                raw_args[2], raw_args[3], (int_t) raw_args[4]); break;
    case 429: // move_mount(from_dfd, from_path, to_dfd, to_path, flags)
        result = (dword_t) sys_move_mount_guest((fd_t) raw_args[0], raw_args[1],
                (fd_t) raw_args[2], raw_args[3], (dword_t) raw_args[4]); break;

    // The io_* family. An aio_context_t is the address of the context's ring
    // page, so arg 0 is a full 64-bit guest address on every call after
    // io_setup -- the legacy marshalled path would have to truncate it, and
    // refuses (SIGSYS). Results are counts or -errno, so the dword sign-
    // extension at the bottom of this function is the right writeback.
    case 0: // io_setup(nr_events, ctx_idp)
        result = (dword_t) sys_io_setup_guest((uint_t) raw_args[0], raw_args[1]); break;
    case 1: // io_destroy(ctx)
        result = (dword_t) sys_io_destroy_guest(raw_args[0]); break;
    case 2: // io_submit(ctx, nr, iocbpp)
        result = (dword_t) sys_io_submit_guest(raw_args[0], (sqword_t) raw_args[1],
                raw_args[2]); break;
    case 3: // io_cancel(ctx, iocb, result)
        result = (dword_t) sys_io_cancel_guest(raw_args[0], raw_args[1], raw_args[2]); break;
    case 4: // io_getevents(ctx, min_nr, nr, events, timeout)
        result = (dword_t) sys_io_getevents_guest(raw_args[0], (sqword_t) raw_args[1],
                (sqword_t) raw_args[2], raw_args[3], raw_args[4]); break;
    case 25: { // fcntl — F_SETFL takes, and F_GETFL returns, open flags in the
               // aarch64 encoding; translate both directions at this boundary
               // (same four relocated O_ constants as openat's flags, above).
        dword_t fcntl_cmd = (dword_t) raw_args[1];
        qword_t fcntl_arg = raw_args[2];
        // riscv64 does NOT relocate the O_ constants: unlike arm64 (which
        // overrides asm-generic fcntl.h), riscv uses the plain asm-generic
        // values, which are bit-identical to this codebase's internal/i386
        // ones — translating them here MIS-translates (riscv O_LARGEFILE
        // 0x8000 is arm64's O_NOFOLLOW; musl ORs O_LARGEFILE into every
        // open, so every open through a final symlink returned ELOOP: apk
        // "Error loading shared library libz.so.1: Symbolic link loop",
        // the same symptom the arm64 port hit for the opposite reason).
        bool relocate = current->abi == GUEST_ABI_ARM64;
        if (fcntl_cmd == F_SETFL_ && relocate)
            fcntl_arg = arm64_open_flags_to_internal(fcntl_arg);
        result = sys_fcntl_amd64_guest((fd_t) raw_args[0], fcntl_cmd, fcntl_arg);
        if (fcntl_cmd == F_GETFL_ && relocate && !syscall_result_is_errno(result))
            result = internal_to_arm64_open_flags(result);
        break;
    }
    case 29: result = sys_ioctl_guest((fd_t) raw_args[0], (dword_t) raw_args[1], raw_args[2]); break;
    case 34: result = sys_mkdirat_guest((fd_t) raw_args[0], raw_args[1], (mode_t_) raw_args[2]); break;
    case 35: result = sys_unlinkat_guest((fd_t) raw_args[0], raw_args[1], (int_t) raw_args[2]); break;
    case 37: result = sys_linkat_guest((fd_t) raw_args[0], raw_args[1], (fd_t) raw_args[2], raw_args[3]); break;
    case 38: result = sys_renameat_guest((fd_t) raw_args[0], raw_args[1], (fd_t) raw_args[2], raw_args[3]); break;
    case 43: result = sys_statfs_amd64_guest(raw_args[0], raw_args[1]); break;
    case 44: result = sys_fstatfs_amd64_guest((fd_t) raw_args[0], raw_args[1]); break;
    // faccessat (48) is a THREE-argument syscall on aarch64 (flags belong
    // to faccessat2, 439); x3 is whatever garbage the caller left in the
    // register — the amd64 port's "classify by MIN arity" lesson. Passing
    // it through made gcc's access(cc1, X_OK) fail EINVAL, so the driver
    // concluded cc1 didn't exist ("cannot execute 'cc1'").
    case 48: result = sys_faccessat_guest((fd_t) raw_args[0], raw_args[1], (mode_t_) raw_args[2], 0); break;
    case 439: result = sys_faccessat_guest((fd_t) raw_args[0], raw_args[1], (mode_t_) raw_args[2], (dword_t) raw_args[3]); break;
    case 49: result = sys_chdir_guest(raw_args[0]); break;
    // syslog's buffer pointer must come through full-width: the legacy
    // table truncated it to 32 bits, so dmesg read into a garbage address
    // and printed an empty/blank buffer.
    case 116: result = (dword_t) sys_syslog_guest((int_t) raw_args[0], raw_args[1], (int_t) raw_args[2]); break;
    case 53: result = sys_fchmodat_guest((fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]); break;
    case 54: result = sys_fchownat_guest((fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (dword_t) raw_args[3], (int) raw_args[4]); break;
    // xattr family (5-16): the legacy table's sys_xattr_stub entries are
    // unreachable from here — the 64-bit path/name/value pointers trip the
    // legacy marshal's full-width check, SIGSYS-killing the caller (cp -p,
    // rsync, tar --xattrs). Dispatch natively through the same *_guest
    // wrappers the amd64 cases (188-199) use; l-variants share the plain
    // wrappers exactly as the amd64 dispatch does (all funnel to the
    // ENOTSUP stub today, but full-width so nothing dies on the way).
    case 5: case 6: result = (dword_t) sys_setxattr_guest( raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3], (dword_t) raw_args[4]); break; // setxattr/lsetxattr
    case 7: result = (dword_t) sys_fsetxattr_guest( (fd_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3], (dword_t) raw_args[4]); break; // fsetxattr
    case 8: case 9: result = (dword_t) sys_getxattr_guest( raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]); break; // getxattr/lgetxattr
    case 10: result = (dword_t) sys_fgetxattr_guest( (fd_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]); break; // fgetxattr
    case 11: case 12: result = (dword_t) sys_listxattr_guest( raw_args[0], raw_args[1], (dword_t) raw_args[2]); break; // listxattr/llistxattr
    case 13: result = (dword_t) sys_flistxattr_guest( (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]); break; // flistxattr
    case 14: case 15: result = (dword_t) sys_removexattr_guest( raw_args[0], raw_args[1]); break; // removexattr/lremovexattr
    case 16: result = (dword_t) sys_fremovexattr_guest( (fd_t) raw_args[0], raw_args[1]); break; // fremovexattr
    // truncate/ftruncate/fallocate carry 64-bit sizes in SINGLE registers
    // on aarch64; the legacy table routed them to the *_amd64 shims, which
    // expect the amd64 marshal's SPLIT low/high convention — so size_high
    // was the next (garbage) argument register, the size became gigantic,
    // and the host returned EFBIG. On device: apk failing to install gcc
    // ("Failed to create usr/libexec/.../cc1: File too large").
    case 45: result = sys_truncate64_guest(raw_args[0],
                     (dword_t) raw_args[1], (dword_t) (raw_args[1] >> 32)); break;
    case 46: result = sys_ftruncate64((fd_t) raw_args[0],
                     (dword_t) raw_args[1], (dword_t) (raw_args[1] >> 32)); break;
    case 47: result = sys_fallocate((fd_t) raw_args[0], (dword_t) raw_args[1],
                     (dword_t) raw_args[2], (dword_t) (raw_args[2] >> 32),
                     (dword_t) raw_args[3], (dword_t) (raw_args[3] >> 32)); break;
    case 56: result = sys_openat_guest((fd_t) raw_args[0], raw_args[1],
                     current->abi == GUEST_ABI_ARM64
                         ? arm64_open_flags_to_internal(raw_args[2])
                         : (dword_t) raw_args[2], // riscv64: asm-generic == internal, see case 25
                     (mode_t_) raw_args[3]); break;
    case 71: result = sys_sendfile_guest((fd_t) raw_args[0], (fd_t) raw_args[1],
                     raw_args[2], raw_args[3]); break;
    case 101: result = sys_nanosleep_amd64_guest(raw_args[0], raw_args[1]); break;
    case 61: result = sys_getdents64_guest((fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]); break;
    case 63: result = sys_read_guest((fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]); break;
    case 64: result = sys_write_guest((fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]); break;
    case 65: result = sys_readv_amd64_guest((fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]); break;
    case 66: result = sys_writev_amd64_guest((fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]); break;
    case 67: result = sys_pread_guest((fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (off_t_) raw_args[3]); break;
    case 68: result = sys_pwrite_guest((fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (off_t_) raw_args[3]); break;
    case 72: result = sys_pselect_amd64_guest((fd_t) raw_args[0], raw_args[1], raw_args[2], raw_args[3], raw_args[4], raw_args[5]); break;
    case 73: result = sys_ppoll_amd64_guest(raw_args[0], (dword_t) raw_args[1], raw_args[2], raw_args[3], (dword_t) raw_args[4]); break;
    case 78: result = sys_readlinkat_guest((fd_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]); break;
    // stat family: arm64-layout marshalling, NOT the amd64 struct — the
    // asm-generic struct stat swaps mode/nlink and resizes fields
    // (fs/stat.h's arm64_stat_ comment; found via busybox ls treating
    // every directory as a file).
    case 79: result = sys_newfstatat_arm64_guest((fd_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]); break;
    case 80: result = sys_fstat_arm64_guest((fd_t) raw_args[0], raw_args[1]); break;
    case 88: result = sys_utimensat_amd64_guest((fd_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]); break;
    case 96: result = (dword_t) sys_set_tid_address_guest(raw_args[0]); break;
    case 98: result = sys_futex_guest(raw_args[0], (dword_t) raw_args[1], (dword_t) raw_args[2], raw_args[3], raw_args[4], (dword_t) raw_args[5]); break;
    case 99: result = (dword_t) sys_set_robust_list_amd64_guest(raw_args[0], (dword_t) raw_args[1]); break;
    case 100: result = (dword_t) sys_get_robust_list_amd64_guest((pid_t_) raw_args[0], raw_args[1], raw_args[2]); break;
    case 113: result = sys_clock_gettime_amd64_guest((dword_t) raw_args[0], raw_args[1]); break;
    case 134: result = sys_rt_sigaction_guest((dword_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]); break;
    case 135: result = sys_rt_sigprocmask_guest((dword_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]); break;
    case 160: result = sys_uname_guest(raw_args[0]); break;
    case 215: result = (dword_t) sys_munmap_guest(raw_args[0], raw_args[1]); break;
    case 220: // clone — AArch64 argument order, see the function comment
        result = sys_clone_guest(raw_args[0], raw_args[1], raw_args[2], raw_args[3], raw_args[4]);
        break;
    case 221: result = (dword_t) sys_execve_guest(raw_args[0], raw_args[1], raw_args[2]); break;
    case 226: result = (dword_t) sys_mprotect_guest(raw_args[0], raw_args[1], (int_t) raw_args[2]); break;
    case 233: result = sys_madvise_guest(raw_args[0], raw_args[1], (dword_t) raw_args[2]); break;
    case 260: result = sys_wait4_guest((pid_t_) raw_args[0], raw_args[1], (dword_t) raw_args[2], raw_args[3]); break;
    case 278: result = sys_getrandom_guest(raw_args[0], (dword_t) raw_args[1], (dword_t) raw_args[2]); break;
    case 291: result = sys_statx_amd64_guest((fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (dword_t) raw_args[3], raw_args[4]); break;
    // Socket family: all pointer-bearing (sockaddr/msghdr/optval), so the
    // legacy dword marshalling truncates the 64-bit guest pointers — which
    // is exactly why busybox login's NSS group lookup failed on device
    // ("can't set groups": a high-stack sockaddr connect() truncated to a
    // garbage 32-bit address returned EINVAL). Same *_guest implementations
    // the amd64 native path uses (64-bit-pointer-safe), keyed by AArch64
    // numbers. socket/listen/shutdown are all-integer and handled fine by
    // the legacy table, so they're omitted here.
    case 199: result = sys_socketpair_guest((dword_t) raw_args[0], (dword_t) raw_args[1], (dword_t) raw_args[2], raw_args[3]); break;
    case 200: result = sys_bind_guest((fd_t) raw_args[0], raw_args[1], (uint_t) raw_args[2]); break;
    case 202: result = sys_accept_guest((fd_t) raw_args[0], raw_args[1], raw_args[2]); break;
    case 203: result = sys_connect_guest((fd_t) raw_args[0], raw_args[1], (uint_t) raw_args[2]); break;
    case 204: result = sys_getsockname_guest((fd_t) raw_args[0], raw_args[1], raw_args[2]); break;
    case 205: result = sys_getpeername_guest((fd_t) raw_args[0], raw_args[1], raw_args[2]); break;
    case 206: result = sys_sendto_guest((fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (dword_t) raw_args[3], raw_args[4], (dword_t) raw_args[5]); break;
    case 207: result = sys_recvfrom_guest((fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (dword_t) raw_args[3], raw_args[4], raw_args[5]); break;
    // the plain sockopt wrappers hardcode the I386 struct layouts
    // (timeval &c); aarch64 uses the generic 64-bit ones == amd64's.
    case 208: result = sys_setsockopt_amd64_guest((fd_t) raw_args[0], (dword_t) raw_args[1], (dword_t) raw_args[2], raw_args[3], (dword_t) raw_args[4]); break;
    case 209: result = sys_getsockopt_amd64_guest((fd_t) raw_args[0], (dword_t) raw_args[1], (dword_t) raw_args[2], raw_args[3], raw_args[4]); break;
    case 211: result = sys_sendmsg_amd64_guest((fd_t) raw_args[0], raw_args[1], (int_t) raw_args[2]); break;
    case 212: result = sys_recvmsg_amd64_guest((fd_t) raw_args[0], raw_args[1], (int_t) raw_args[2]); break;
    case 242: result = sys_accept4_guest((fd_t) raw_args[0], raw_args[1], raw_args[2], (int_t) raw_args[3]); break;
    case 269: result = sys_sendmmsg_amd64_guest((fd_t) raw_args[0], raw_args[1], (uint_t) raw_args[2], (int_t) raw_args[3]); break;
    // -- audit batch: every remaining pointer-taking syscall in the arm64
    // legacy table, routed through the same 64-bit-shaped sys_*_guest
    // wrappers the amd64 native dispatch uses (bodies mirror those cases;
    // the *_amd64_/*64_ variants are the generic 64-bit ABI layouts, which
    // aarch64 shares). Before this, dmesg-style silent pointer truncation
    // applied to all of them.
    case 21: result = (dword_t) sys_epoll_ctl_guest( (fd_t) raw_args[0], (int_t) raw_args[1], (fd_t) raw_args[2], raw_args[3]); break; // epoll_ctl
    case 22: result = (dword_t) sys_epoll_pwait_guest( (fd_t) raw_args[0], raw_args[1], (int_t) raw_args[2], (int_t) raw_args[3], raw_args[4], (dword_t) raw_args[5]); break; // epoll_pwait
    case 27: result = (dword_t) sys_inotify_add_watch_guest( (fd_t) raw_args[0], raw_args[1], (uint_t) raw_args[2]); break; // inotify_add_watch
    case 33: result = (dword_t) sys_mknodat_guest( (fd_t) raw_args[0], raw_args[1], (mode_t_) raw_args[2], (dev_t_) raw_args[3]); break; // mknodat
    case 36: result = (dword_t) sys_symlinkat_guest( raw_args[0], (fd_t) raw_args[1], raw_args[2]); break; // symlinkat
    case 39: result = (dword_t) sys_umount2_guest( raw_args[0], (dword_t) raw_args[1]); break; // umount2
    case 40: result = (dword_t) sys_mount_guest( raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3], raw_args[4]); break; // mount
    case 51: result = (dword_t) sys_chroot_guest(raw_args[0]); break; // chroot
    case 59: result = (dword_t) sys_pipe2( raw_args[0], (int_t) raw_args[1]); break; // pipe2
    case 86: result = (dword_t) sys_timerfd_settime64_guest( (fd_t) raw_args[0], (int_t) raw_args[1], raw_args[2], raw_args[3]); break; // timerfd_settime
    case 87: result = (dword_t) sys_timerfd_gettime64_guest( (fd_t) raw_args[0], raw_args[1]); break; // timerfd_gettime
    case 90: result = (dword_t) sys_capget_guest( raw_args[0], raw_args[1]); break; // capget
    case 91: result = (dword_t) sys_capset_guest( raw_args[0], raw_args[1]); break; // capset
    case 95: result = (dword_t) sys_waitid_guest( (int_t) raw_args[0], (pid_t_) raw_args[1], raw_args[2], (int_t) raw_args[3]); break; // waitid
    case 103: result = (dword_t) sys_setitimer_amd64_guest( (int_t) raw_args[0], raw_args[1], raw_args[2]); break; // setitimer
    case 102: result = (dword_t) sys_getitimer_amd64_guest((int_t) raw_args[0], raw_args[1]); break; // getitimer (was entirely missing)
    case 107: result = (dword_t) sys_timer_create_amd64_guest( (dword_t) raw_args[0], raw_args[1], raw_args[2]); break; // timer_create
    case 108: result = (dword_t) sys_timer_gettime64_guest( (dword_t) raw_args[0], raw_args[1]); break; // timer_gettime
    case 110: result = (dword_t) sys_timer_settime64_guest( (dword_t) raw_args[0], (int_t) raw_args[1], raw_args[2], raw_args[3]); break; // timer_settime
    case 112: result = (dword_t) sys_clock_settime( (dword_t) raw_args[0], raw_args[1]); break; // clock_settime
    case 114: result = (dword_t) sys_clock_getres_amd64_guest( (dword_t) raw_args[0], raw_args[1]); break; // clock_getres
    case 115: result = (dword_t) sys_clock_nanosleep_amd64_guest( (dword_t) raw_args[0], (int_t) raw_args[1], raw_args[2], raw_args[3]); break; // clock_nanosleep
    case 117: result = (dword_t) sys_ptrace_guest( (dword_t) raw_args[0], (dword_t) raw_args[1], raw_args[2], raw_args[3]); break; // ptrace
    case 119: result = (dword_t) sys_sched_setscheduler_guest( (pid_t_) raw_args[0], (int_t) raw_args[1], raw_args[2]); break; // sched_setscheduler
    // case 121 (sched_getparam) is handled below, grouped with the other
    // sched_*/setdomainname/adjtimex entries added together.
    case 122: result = (dword_t) sys_sched_setaffinity_guest( (pid_t_) raw_args[0], (dword_t) raw_args[1], raw_args[2]); break; // sched_setaffinity
    case 123: result = (dword_t) sys_sched_getaffinity_guest( (pid_t_) raw_args[0], (dword_t) raw_args[1], raw_args[2]); break; // sched_getaffinity
    case 132: result = (dword_t) sys_sigaltstack_guest( raw_args[0], raw_args[1]); break; // sigaltstack
    case 133: result = (dword_t) sys_rt_sigsuspend_guest( raw_args[0], (uint_t) raw_args[1]); break; // rt_sigsuspend
    case 136: result = (dword_t) sys_rt_sigpending_guest(raw_args[0]); break; // rt_sigpending
    case 137: result = (dword_t) sys_rt_sigtimedwait_guest( raw_args[0], raw_args[1], raw_args[2], (uint_t) raw_args[3]); break; // rt_sigtimedwait
    case 138: result = (dword_t) sys_rt_sigqueueinfo_guest( (pid_t_) raw_args[0], (dword_t) raw_args[1], raw_args[2]); break; // rt_sigqueueinfo
    case 153: result = (dword_t) sys_times_guest(raw_args[0]); break; // times
    case 158: result = (dword_t) sys_getgroups_guest( (dword_t) raw_args[0], raw_args[1]); break; // getgroups
    case 159: result = (dword_t) sys_setgroups_guest( (dword_t) raw_args[0], raw_args[1]); break; // setgroups
    case 161: result = (dword_t) sys_sethostname_guest( raw_args[0], (dword_t) raw_args[1]); break; // sethostname
    case 163: result = (dword_t) sys_getrlimit64_guest( (dword_t) raw_args[0], raw_args[1]); break; // getrlimit (64-bit layout)
    case 164: result = (dword_t) sys_setrlimit64_guest( (dword_t) raw_args[0], raw_args[1]); break; // setrlimit
    case 165: result = (dword_t) sys_getrusage_guest( (dword_t) raw_args[0], raw_args[1]); break; // getrusage
    case 167: result = (dword_t) sys_prctl_guest( (dword_t) raw_args[0], raw_args[1], raw_args[2], raw_args[3], raw_args[4]); break; // prctl
    case 169: result = (dword_t) sys_gettimeofday_amd64_guest( raw_args[0], raw_args[1]); break; // gettimeofday
    case 179: result = (dword_t) sys_sysinfo_guest(raw_args[0]); break; // sysinfo
    case 227: result = (dword_t) sys_msync_guest(raw_args[0], raw_args[1], (int_t) raw_args[2]); break; // msync
    case 228: result = (dword_t) sys_mlock_guest(raw_args[0], raw_args[1]); break; // mlock
    case 229: result = (dword_t) sys_munlock_guest(raw_args[0], raw_args[1]); break; // munlock
    case 232: result = (dword_t) sys_mincore_guest(raw_args[0], raw_args[1], raw_args[2]); break; // mincore
    case 235: result = (dword_t) sys_mbind_guest( raw_args[0], raw_args[1], (int_t) raw_args[2], raw_args[3], raw_args[4], (uint_t) raw_args[5]); break; // mbind
    case 243: result = (dword_t) sys_recvmmsg_amd64_guest( (fd_t) raw_args[0], raw_args[1], (uint_t) raw_args[2], (int_t) raw_args[3], raw_args[4]); break; // recvmmsg
    case 261: result = (dword_t) sys_prlimit64_guest( (pid_t_) raw_args[0], (dword_t) raw_args[1], raw_args[2], raw_args[3]); break; // prlimit64
    case 276: result = (dword_t) sys_renameat2_guest( (fd_t) raw_args[0], raw_args[1], (fd_t) raw_args[2], raw_args[3], (int_t) raw_args[4]); break; // renameat2
    case 281: result = (dword_t) sys_execveat_guest( (fd_t) raw_args[0], raw_args[1], raw_args[2], raw_args[3], (int_t) raw_args[4]); break; // execveat
    case 285: result = (dword_t) sys_copy_file_range_guest( (fd_t) raw_args[0], raw_args[1], (fd_t) raw_args[2], raw_args[3], raw_args[4], (uint_t) raw_args[5]); break; // copy_file_range
    case 435: result = (dword_t) sys_clone3_guest( raw_args[0], (dword_t) raw_args[1]); break; // clone3
    case 437: result = (dword_t) sys_openat2_guest( (fd_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]); break; // openat2
    case 441: result = (dword_t) sys_epoll_pwait2_guest( (fd_t) raw_args[0], raw_args[1], (int_t) raw_args[2], raw_args[3], raw_args[4], (dword_t) raw_args[5]); break; // epoll_pwait2
    // splice takes two 64-bit loff_t pointers and a 64-bit count, so it has to
    // be dispatched natively rather than through the legacy marshaller -- and
    // with the REAL arguments. It used to be called with zeros here because it
    // was a stub that read none of them; leaving that in place after
    // implementing it would have made every arm64 and riscv64 splice an EINVAL
    // on a null pipe fd, with nothing to say why.
    case 76: result = (dword_t) sys_splice_guest((fd_t) raw_args[0], raw_args[1],
                 (fd_t) raw_args[2], raw_args[3], raw_args[4], (dword_t) raw_args[5]); break; // splice
    case 75: result = (dword_t) sys_vmsplice_guest((fd_t) raw_args[0], raw_args[1],
                 raw_args[2], (dword_t) raw_args[3]); break; // vmsplice
    case 77: result = (dword_t) sys_tee((fd_t) raw_args[0], (fd_t) raw_args[1],
                 (dword_t) raw_args[2], (dword_t) raw_args[3]); break; // tee
    // settimeofday is an argument-ignoring stub (EPERM); dispatch natively
    // with zeros so 64-bit pointers can't be rejected or truncated on the way
    // to code that never reads them.
    case 170: result = sys_settimeofday(0, 0); break; // settimeofday (stub)
    case 148: result = (dword_t) sys_getresuid_guest(raw_args[0], raw_args[1], raw_args[2]); break; // getresuid
    case 150: result = (dword_t) sys_getresgid_guest(raw_args[0], raw_args[1], raw_args[2]); break; // getresgid
    case 194: result = (dword_t) sys_shmget_guest((dword_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]); break; // shmget
    case 195: result = (dword_t) sys_shmctl_guest((int_t) raw_args[0], (int_t) raw_args[1], raw_args[2]); break; // shmctl (64-bit shmid_ds)
    case 197: result = (dword_t) sys_shmdt_guest(raw_args[0]); break; // shmdt
    // SysV message queues (fakeroot). msgrcv's msgtyp is a signed long in
    // its own register; full-width so 64-bit msgp/msgtyp survive.
    case 186: result = (dword_t) sys_msgget_guest((dword_t) raw_args[0], (int_t) raw_args[1]); break; // msgget
    case 187: result = (dword_t) sys_msgctl_guest((int_t) raw_args[0], (int_t) raw_args[1], raw_args[2]); break; // msgctl (64-bit msqid_ds)
    case 188: result = (dword_t) sys_msgrcv_guest((int_t) raw_args[0], raw_args[1], raw_args[2], (sqword_t) raw_args[3], (int_t) raw_args[4]); break; // msgrcv
    case 189: result = (dword_t) sys_msgsnd_guest((int_t) raw_args[0], raw_args[1], raw_args[2], (int_t) raw_args[3]); break; // msgsnd
    case 190: result = (dword_t) sys_semget_guest((dword_t) raw_args[0], (int_t) raw_args[1], (int_t) raw_args[2]); break; // semget
    case 191: result = (dword_t) sys_semctl_guest((int_t) raw_args[0], (int_t) raw_args[1], (int_t) raw_args[2], raw_args[3]); break; // semctl (union semun in register)
    case 192: result = (dword_t) sys_semtimedop_guest((int_t) raw_args[0], raw_args[1], (uint_t) raw_args[2], raw_args[3]); break; // semtimedop
    case 193: result = (dword_t) sys_semop_guest((int_t) raw_args[0], raw_args[1], (uint_t) raw_args[2]); break; // semop
    case 223: result = 0; break; // fadvise64: advisory, ignored (64-bit off/len args)
    // -- OpenMinis-parity sweep (see the table comment) --
    // real implementations
    case 69: result = (dword_t) sys_preadv_guest((fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (off_t_) raw_args[3]); break; // preadv (arm64: 64-bit offset in single register)
    case 70: result = (dword_t) sys_pwritev_guest((fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (off_t_) raw_args[3]); break; // pwritev (arm64: 64-bit offset in single register)
    case 74: result = (dword_t) sys_signalfd4_guest((int_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (int_t) raw_args[3]); break; // signalfd4
    case 236: result = (dword_t) sys_get_mempolicy_guest(raw_args[0], raw_args[1], raw_args[2], raw_args[3], raw_args[4]); break; // get_mempolicy
    case 237: result = (dword_t) sys_set_mempolicy_guest((int) raw_args[0], raw_args[1], raw_args[2]); break; // set_mempolicy
    case 240: result = (dword_t) sys_rt_tgsigqueueinfo_guest((pid_t_) raw_args[0], (pid_t_) raw_args[1], (dword_t) raw_args[2], raw_args[3]); break; // rt_tgsigqueueinfo
    case 266: result = (dword_t) sys_clock_adjtime_amd64_guest((dword_t) raw_args[0], raw_args[1]); break; // clock_adjtime (EPERM inside)
    case 270: result = (dword_t) sys_process_vm_readv_guest((pid_t_) raw_args[0], raw_args[1], (dword_t) raw_args[2], raw_args[3], (dword_t) raw_args[4], (dword_t) raw_args[5]); break; // process_vm_readv
    case 279: result = (dword_t) sys_memfd_create_guest(raw_args[0], (uint_t) raw_args[1]); break; // memfd_create (was a silent stub)
    case 283: result = (dword_t) sys_membarrier((dword_t) raw_args[0], (dword_t) raw_args[1], 0); break; // membarrier (2-arg base ABI; 3rd reg is caller garbage)
    case 293: result = (dword_t) sys_rseq_guest(raw_args[0], (dword_t) raw_args[1], (dword_t) raw_args[2], (dword_t) raw_args[3]); break; // rseq
    case 452: result = (dword_t) sys_fchmodat2_guest((fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (dword_t) raw_args[3]); break; // fchmodat2 (tar)
    case 118: result = (dword_t) sys_sched_setparam_guest((pid_t_) raw_args[0], raw_args[1]); break; // sched_setparam
    case 121: result = (dword_t) sys_sched_getparam_guest((pid_t_) raw_args[0], raw_args[1]); break; // sched_getparam
    case 127: result = (dword_t) sys_sched_rr_get_interval_guest((pid_t_) raw_args[0], raw_args[1]); break; // sched_rr_get_interval
    case 162: result = (dword_t) sys_setdomainname_guest(raw_args[0], (dword_t) raw_args[1]); break; // setdomainname
    case 171: result = (dword_t) sys_adjtimex_guest(raw_args[0]); break; // adjtimex
    // advisory no-ops (amd64 parity)
    case 58:  // vhangup (0 args; the table stub predates marshal validation,
              // which SIGSYSed it on garbage arg registers — sendfile_vhangup
              // regression caught it)
    case 81:  // sync
    case 84:  // sync_file_range
    case 168: // getcpu
    case 213: // readahead
        result = 0; break;
    // EOPNOTSUPP probes (amd64 parity: elogind/systemd fall back)
    case 264: // name_to_handle_at
    case 265: // open_by_handle_at
    case 277: // seccomp
        result = _EOPNOTSUPP; break;
    // preadv2/pwritev2: iov pointer and offset are full 64-bit values that
    // trip the legacy marshal's dword-fit check (SIGSYS). Route natively,
    // same pattern as the amd64 intercept in handle_amd64_native_memory_syscall.
    case 286: // preadv2(fd, iov, iovcnt, offset, flags)
        result = (dword_t) sys_preadv2_guest((fd_t) raw_args[0], raw_args[1],
                     (dword_t) raw_args[2], (off_t_) raw_args[3], (uint_t) raw_args[4]); break;
    case 287: // pwritev2(fd, iov, iovcnt, offset, flags)
        result = (dword_t) sys_pwritev2_guest((fd_t) raw_args[0], raw_args[1],
                      (dword_t) raw_args[2], (off_t_) raw_args[3], (uint_t) raw_args[4]); break;
    case 272: // kcmp(pid1, pid2, type, idx1, idx2)
        result = (dword_t) sys_kcmp((pid_t_) raw_args[0], (pid_t_) raw_args[1],
                      (dword_t) raw_args[2], (dword_t) raw_args[3], (dword_t) raw_args[4]); break;
    // clean ENOSYS: known syscalls with no implementation; native so
    // 64-bit pointer args never trip the legacy-marshal validation
    // (0-4 are the io_* family, implemented above -- they were on this
    // ENOSYS list, which is why wiring arm64_syscall_table alone changed
    // nothing: this native list runs first and answered for them.)
    case 18: case 41: case 42:
    case 60: case 89: case 104: case 105: case 106:
    case 128:
    case 180: case 181: case 182: case 183: case 184: case 185:
    // (186-193 are the real SysV msg/sem implementations above)
    case 217: case 218: case 224:
    case 225: case 234: case 238: case 239: case 241: case 262:
    case 263: case 268: case 271: case 273:
    case 274: case 275: case 280: case 282: case 284:
    case 288: case 289: case 290: case 294:
    case 425: case 426: case 427: case 438: case 440:
    case 449: // futex_waitv
        result = _ENOSYS; break;
    case 0xacc0: // iSH crypto accelerator: AEAD seal/open (ISH_SYS_AEAD)
        result = sys_ish_aead_guest(raw_args[0]); break;
    case 0xacc1: // iSH pixman accelerator: FILL/COPY/OVER (ISH_SYS_PIXOP)
        result = sys_ish_pixop_guest(raw_args[0]); break;
    default:
        return false; // not handled here: fall through to the legacy-marshalled table
    }
    if (syscall_result_should_restart(result)) {
        // A native caller has no PC to rewind: it gets the restart code back
        // and re-issues the call itself (kernel/native_syscall.c).
        if (native_syscall_active) {
            native_syscall_value = (qword_t) (sqword_t) (sdword_t) result;
            return true;
        }
        // handle_asm_generic_native_syscall is shared by both arm64 and
        // riscv64 guests; prepare_syscall_restart's PC-rewind and
        // syscall-number/first-arg restore are register-file-specific
        // (arm64_regs/arm64_pc vs riscv64_regs/riscv64_pc), so the dispatch
        // table passed here must match the actual guest ABI, not be
        // hardcoded to arm64 -- otherwise a riscv64 syscall interrupted by
        // an SA_RESTART signal never gets its real PC rewound over the
        // ecall (the write lands in the unused arm64 register shadow
        // instead), and resumes with garbage in a0 (signal_restart,
        // eventfd_interrupt, futex_core device failures).
        prepare_syscall_restart(cpu, current->abi == GUEST_ABI_RISCV64 ?
                &riscv64_syscall_dispatch : &arm64_syscall_dispatch, syscall_num, raw_args[0]);
        return true;
    }
    // Sign-extend: a dword -errno (or any negative 32-bit result, e.g.
    // wait4's -1... which IS an errno encoding here) widens to the 64-bit
    // negative X0 the guest's errno check expects.
    asm_generic_syscall_result_qword(cpu, (qword_t) (sqword_t) (sdword_t) result);
    return true;
}

static bool handle_amd64_native_memory_syscall(struct cpu_state *cpu, qword_t syscall_num,
        const qword_t raw_args[6]) {
    switch (syscall_num) {
    case 2:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_open_guest(
                raw_args[0], (dword_t) raw_args[1], (mode_t_) raw_args[2]));
        return true;
    case 0:
    {
        dword_t result = sys_read_guest((fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]);
        if (syscall_result_should_restart(result)) {
            prepare_syscall_restart(cpu, &amd64_syscall_dispatch, syscall_num, raw_args[0]);
        } else {
            amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) result);
        }
        return true;
    }
    case 1:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_write_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]));
        return true;
    case 4:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_stat_amd64_guest(
                raw_args[0], raw_args[1]));
        return true;
    case 5:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_fstat_amd64_guest(
                (fd_t) raw_args[0], raw_args[1]));
        return true;
    case 6:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_lstat_amd64_guest(
                raw_args[0], raw_args[1]));
        return true;
    case 9:
        amd64_syscall_result_qword(cpu, sys_mmap_guest(raw_args[0], raw_args[1],
                (dword_t) raw_args[2], (dword_t) raw_args[3], (fd_t) raw_args[4],
                raw_args[5]));
        return true;
    case 10:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_mprotect_guest(raw_args[0],
                raw_args[1], (int_t) raw_args[2]));
        return true;
    case 13:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_rt_sigaction_guest(
                (dword_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]));
        return true;
    case 14:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_rt_sigprocmask_guest(
                (dword_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]));
        return true;
    // Through the restart protocol, not straight into the result register.
    // These are the ERESTARTNOHAND waits: interrupted by a job-control stop
    // they return _ERESTART_NOHAND (-512), which is an INTERNAL code. The
    // caller is meant to either rewind the PC and re-execute, or -- if a
    // handler ran -- convert it to EINTR. Written directly to rax it was
    // neither, so an amd64 guest Ctrl-Z'd inside poll() and resumed got
    // errno 512 back out of poll(). Every one of these leaked it, with and
    // without a handler. arm64 never did: its legacy switch routes them
    // through this same predicate. read (case 0) above is the shape.
    case 7:
    {
        dword_t result = sys_poll_guest(
                    raw_args[0], (dword_t) raw_args[1], (int_t) raw_args[2]);
        if (syscall_result_should_restart(result))
            prepare_syscall_restart(cpu, &amd64_syscall_dispatch, syscall_num, raw_args[0]);
        else
            amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) result);
        return true;
    }
    case 16:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_ioctl_guest(
                (fd_t) raw_args[0], (dword_t) raw_args[1], raw_args[2]));
        return true;
    case 42:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_connect_guest(
                (fd_t) raw_args[0], raw_args[1], (uint_t) raw_args[2]));
        return true;
    case 43:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_accept_guest(
                (fd_t) raw_args[0], raw_args[1], raw_args[2]));
        return true;
    case 44:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_sendto_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (dword_t) raw_args[3],
                raw_args[4], (dword_t) raw_args[5]));
        return true;
    case 8:
        amd64_syscall_result_qword(cpu, (qword_t) sys_lseek_amd64_guest(
                (fd_t) raw_args[0], (off_t_) raw_args[1], (dword_t) raw_args[2]));
        return true;
    case 45:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_recvfrom_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (dword_t) raw_args[3],
                raw_args[4], raw_args[5]));
        return true;
    case 17:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_pread_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (off_t_) raw_args[3]));
        return true;
    case 18:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_pwrite_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (off_t_) raw_args[3]));
        return true;
    case 19:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_readv_amd64_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]));
        return true;
    case 20:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_writev_amd64_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]));
        return true;
    case 21:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_access_guest(
                raw_args[0], (dword_t) raw_args[1]));
        return true;
    case 22:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_pipe(
                raw_args[0]));
        return true;
    case 23:
    {
        dword_t result = sys_select_amd64_guest(
                    (fd_t) raw_args[0], raw_args[1], raw_args[2], raw_args[3], raw_args[4]);
        if (syscall_result_should_restart(result))
            prepare_syscall_restart(cpu, &amd64_syscall_dispatch, syscall_num, raw_args[0]);
        else
            amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) result);
        return true;
    }
    case 293:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_pipe2(
                raw_args[0], (int_t) raw_args[1]));
        return true;
    case 35:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_nanosleep_amd64_guest(
                raw_args[0], raw_args[1]));
        return true;
    case 36:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_getitimer_amd64_guest(
                (int_t) raw_args[0], raw_args[1]));
        return true;
    case 38:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_setitimer_amd64_guest(
                (int_t) raw_args[0], raw_args[1], raw_args[2]));
        return true;
    case 49:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_bind_guest(
                (fd_t) raw_args[0], raw_args[1], (uint_t) raw_args[2]));
        return true;
    case 51:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_getsockname_guest(
                (fd_t) raw_args[0], raw_args[1], raw_args[2]));
        return true;
    case 52:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_getpeername_guest(
                (fd_t) raw_args[0], raw_args[1], raw_args[2]));
        return true;
    case 54:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_setsockopt_amd64_guest(
                (fd_t) raw_args[0], (dword_t) raw_args[1], (dword_t) raw_args[2],
                raw_args[3], (dword_t) raw_args[4]));
        return true;
    case 55:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_getsockopt_amd64_guest(
                (fd_t) raw_args[0], (dword_t) raw_args[1], (dword_t) raw_args[2],
                raw_args[3], raw_args[4]));
        return true;
    case 46:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_sendmsg_amd64_guest(
                (fd_t) raw_args[0], raw_args[1], (int_t) raw_args[2]));
        return true;
    case 47:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_recvmsg_amd64_guest(
                (fd_t) raw_args[0], raw_args[1], (int_t) raw_args[2]));
        return true;
    case 53:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_socketpair_guest(
                (dword_t) raw_args[0], (dword_t) raw_args[1], (dword_t) raw_args[2], raw_args[3]));
        return true;
    case 11:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_munmap_guest(raw_args[0],
                raw_args[1]));
        return true;
    case 12:
        amd64_syscall_result_qword(cpu, sys_brk_guest(raw_args[0]));
        return true;
    case 25:
        amd64_syscall_result_qword(cpu, sys_mremap_guest(raw_args[0], raw_args[1],
                raw_args[2], (dword_t) raw_args[3], raw_args[4]));
        return true;
    case 26:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_msync_guest(raw_args[0],
                raw_args[1], (int_t) raw_args[2]));
        return true;
    case 27:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_mincore_guest(raw_args[0],
                raw_args[1], raw_args[2]));
        return true;
    case 28:
        amd64_syscall_result_qword(cpu, sys_madvise_guest(raw_args[0], raw_args[1],
                (dword_t) raw_args[2]));
        return true;
    case 76:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_truncate64_guest(
                raw_args[0], (dword_t) raw_args[1], (dword_t) (raw_args[1] >> 32)));
        return true;
    case 78:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_getdents_amd64_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]));
        return true;
    case 79:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_getcwd_guest(
                raw_args[0], (dword_t) raw_args[1]));
        return true;
    case 80:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_chdir_guest(raw_args[0]));
        return true;
    case 82:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_rename_guest(
                raw_args[0], raw_args[1]));
        return true;
    case 83:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_mkdir_guest(
                raw_args[0], (mode_t_) raw_args[1]));
        return true;
    case 84:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_rmdir_guest(raw_args[0]));
        return true;
    case 85:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_creat_guest(
                raw_args[0], (mode_t_) raw_args[1]));
        return true;
    case 86:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_link_guest(
                raw_args[0], raw_args[1]));
        return true;
    case 87:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_unlink_guest(raw_args[0]));
        return true;
    case 88:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_symlink_guest(
                raw_args[0], raw_args[1]));
        return true;
    case 89:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_readlink_guest(
                raw_args[0], raw_args[1], (dword_t) raw_args[2]));
        return true;
    case 90:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_chmod_guest(
                raw_args[0], (dword_t) raw_args[1]));
        return true;
    case 92:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_chown_amd64_guest(
                raw_args[0], (uid_t_) raw_args[1], (uid_t_) raw_args[2]));
        return true;
    case 93:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_fchown_amd64(
                (fd_t) raw_args[0], (uid_t_) raw_args[1], (uid_t_) raw_args[2]));
        return true;
    case 94:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_lchown_amd64_guest(
                raw_args[0], (uid_t_) raw_args[1], (uid_t_) raw_args[2]));
        return true;
    case 96:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_gettimeofday_amd64_guest(
                raw_args[0], raw_args[1]));
        return true;
    case 97:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_getrlimit64_guest(
                (dword_t) raw_args[0], raw_args[1]));
        return true;
    case 63:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_uname_guest(raw_args[0]));
        return true;
    case 98:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_getrusage_guest(
                (dword_t) raw_args[0], raw_args[1]));
        return true;
    case 103:
        amd64_syscall_result_qword(cpu, (qword_t) sys_syslog_guest(
                (int_t) raw_args[0], raw_args[1], (int_t) raw_args[2]));
        return true;
    case 118:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_getresuid_guest(
                raw_args[0], raw_args[1], raw_args[2]));
        return true;
    case 120:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_getresgid_guest(
                raw_args[0], raw_args[1], raw_args[2]));
        return true;
    case 127:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_rt_sigpending_guest(raw_args[0]));
        return true;
    case 130:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_rt_sigsuspend_guest(
                raw_args[0], (uint_t) raw_args[1]));
        return true;
    case 99:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_sysinfo_guest(raw_args[0]));
        return true;
    case 100:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_times_guest(raw_args[0]));
        return true;
    case 101:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_ptrace_guest(
                (dword_t) raw_args[0], (dword_t) raw_args[1], raw_args[2], raw_args[3]));
        return true;
    case 201:
        amd64_syscall_result_qword(cpu, sys_time_amd64_guest(raw_args[0]));
        return true;
    case 131:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_sigaltstack_guest(
                raw_args[0], raw_args[1]));
        return true;
    case 128:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_rt_sigtimedwait_guest(
                raw_args[0], raw_args[1], raw_args[2], (uint_t) raw_args[3]));
        return true;
    case 129:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_rt_sigqueueinfo_guest(
                (pid_t_) raw_args[0], (dword_t) raw_args[1], raw_args[2]));
        return true;
    case 132:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_utime_amd64_guest(
                raw_args[0], raw_args[1]));
        return true;
    case 133:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_mknod_guest(
                raw_args[0], (mode_t_) raw_args[1], (dev_t_) raw_args[2]));
        return true;
    case 137:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_statfs_amd64_guest(
                raw_args[0], raw_args[1]));
        return true;
    case 138:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_fstatfs_amd64_guest(
                (fd_t) raw_args[0], raw_args[1]));
        return true;
    case 149:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_mlock_guest(raw_args[0],
                raw_args[1]));
        return true;
    case 150:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_munlock_guest(raw_args[0],
                raw_args[1]));
        return true;
    case 158:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_arch_prctl_guest(
                (int_t) raw_args[0], raw_args[1]));
        return true;
    case 157:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_prctl_guest(
                (dword_t) raw_args[0], raw_args[1], raw_args[2], raw_args[3], raw_args[4]));
        return true;
    case 160:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_setrlimit64_guest(
                (dword_t) raw_args[0], raw_args[1]));
        return true;
    case 159: // adjtimex: the timex buffer is a pointer, so it needs the full
              // 64-bit addr (mirrors the asm-generic [171] case)
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_adjtimex_guest(
                raw_args[0]));
        return true;
    case 125:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_capget_guest(
                raw_args[0], raw_args[1]));
        return true;
    case 126:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_capset_guest(
                raw_args[0], raw_args[1]));
        return true;
    // 142/148 join 143/144 here rather than riding the legacy table: their
    // struct sched_param / struct timespec pointer is a full 64-bit guest
    // address, and an amd64 binary's rodata and heap sit above 4GiB (a static
    // string literal lands around 0x555555556000), so the marshaller's
    // dword-fit check would SIGSYS every call made with anything but a stack
    // buffer. Same reasoning as sethostname/170 below.
    case 142:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_sched_setparam_guest(
                (pid_t_) raw_args[0], raw_args[1]));
        return true;
    case 143:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_sched_getparam_guest(
                (pid_t_) raw_args[0], raw_args[1]));
        return true;
    case 148:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_sched_rr_get_interval_guest(
                (pid_t_) raw_args[0], raw_args[1]));
        return true;
    case 144:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_sched_setscheduler_guest(
                (pid_t_) raw_args[0], (int_t) raw_args[1], raw_args[2]));
        return true;
    case 202:
    {
        dword_t result = sys_futex_guest(
                raw_args[0], (dword_t) raw_args[1], (dword_t) raw_args[2], raw_args[3],
                raw_args[4], (dword_t) raw_args[5]);
        if (syscall_result_should_restart(result)) {
            prepare_syscall_restart(cpu, &amd64_syscall_dispatch, syscall_num, raw_args[0]);
        } else {
            amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) result);
        }
        return true;
    }
    case 161:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_chroot_guest(raw_args[0]));
        return true;
    case 165:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_mount_guest(
                raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3], raw_args[4]));
        return true;
    case 166: // umount2: target is a pointer, so it needs the full 64-bit addr
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_umount2_guest(
                raw_args[0], (dword_t) raw_args[1]));
        return true;
    case 170:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_sethostname_guest(
                raw_args[0], (dword_t) raw_args[1]));
        return true;
    case 171:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_setdomainname_guest(
                raw_args[0], (dword_t) raw_args[1]));
        return true;
    case 218:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_set_tid_address_guest(
                raw_args[0]));
        return true;
    case 222:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_timer_create_amd64_guest(
                (dword_t) raw_args[0], raw_args[1], raw_args[2]));
        return true;
    case 223:
        // amd64 itimerspec is the 64-bit layout -> use the 64-bit marshalling.
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_timer_settime64_guest(
                (dword_t) raw_args[0], (int_t) raw_args[1], raw_args[2], raw_args[3]));
        return true;
    case 224:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_timer_gettime64_guest(
                (dword_t) raw_args[0], raw_args[1]));
        return true;
    case 227:
        // clock_settime: handled natively like clock_gettime(228) so the
        // legacy 32-bit arg marshaller does not reject the 64-bit guest
        // timespec pointer with "needs full-width args" and raise SIGSYS
        // (observed: a process that calls clock_settime dies with "Bad system
        // call" instead of returning the EINVAL/EPERM the clock id warrants).
        //
        // The pointer is passed through. It used to be hardcoded 0 here,
        // because iSH cannot set a host clock and the implementation ignored
        // the timespec entirely -- but it now reads and range-checks it before
        // refusing, the way Linux orders EFAULT, EINVAL and EPERM, and a
        // hardcoded 0 made every well-formed call report EFAULT.
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_clock_settime(
                (dword_t) raw_args[0], raw_args[1]));
        return true;
    case 228:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_clock_gettime_amd64_guest(
                (dword_t) raw_args[0], raw_args[1]));
        return true;
    case 229:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_clock_getres_amd64_guest(
                (dword_t) raw_args[0], raw_args[1]));
        return true;
    case 230:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_clock_nanosleep_amd64_guest(
                (dword_t) raw_args[0], (int_t) raw_args[1], raw_args[2], raw_args[3]));
        return true;
    case 305:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_clock_adjtime_amd64_guest(
                (dword_t) raw_args[0], raw_args[1]));
        return true;
    case 217:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_getdents64_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]));
        return true;
    case 254:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_inotify_add_watch_guest(
                (fd_t) raw_args[0], raw_args[1], (uint_t) raw_args[2]));
        return true;
    case 318:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_getrandom_guest(
                raw_args[0], (dword_t) raw_args[1], (dword_t) raw_args[2]));
        return true;
    case 232:
    {
        dword_t result = sys_epoll_wait_guest(
                    (fd_t) raw_args[0], raw_args[1], (int_t) raw_args[2], (int_t) raw_args[3]);
        if (syscall_result_should_restart(result))
            prepare_syscall_restart(cpu, &amd64_syscall_dispatch, syscall_num, raw_args[0]);
        else
            amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) result);
        return true;
    }
    case 233:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_epoll_ctl_guest(
                (fd_t) raw_args[0], (int_t) raw_args[1], (fd_t) raw_args[2], raw_args[3]));
        return true;
    case 235:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_utimes_amd64_guest(
                raw_args[0], raw_args[1]));
        return true;
    case 288:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_accept4_guest(
                (fd_t) raw_args[0], raw_args[1], raw_args[2], (int_t) raw_args[3]));
        return true;
    case 257:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_openat_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (mode_t_) raw_args[3]));
        return true;
    case 258:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_mkdirat_guest(
                (fd_t) raw_args[0], raw_args[1], (mode_t_) raw_args[2]));
        return true;
    case 259:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_mknodat_guest(
                (fd_t) raw_args[0], raw_args[1], (mode_t_) raw_args[2], (dev_t_) raw_args[3]));
        return true;
    case 260:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_fchownat_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (dword_t) raw_args[3],
                (int) raw_args[4]));
        return true;
    case 261:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_futimesat_amd64_guest(
                (fd_t) raw_args[0], raw_args[1], raw_args[2]));
        return true;
    case 262:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_newfstatat_amd64_guest(
                (fd_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]));
        return true;
    case 263:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_unlinkat_guest(
                (fd_t) raw_args[0], raw_args[1], (int_t) raw_args[2]));
        return true;
    case 264:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_renameat_guest(
                (fd_t) raw_args[0], raw_args[1], (fd_t) raw_args[2], raw_args[3]));
        return true;
    case 316:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_renameat2_guest(
                (fd_t) raw_args[0], raw_args[1], (fd_t) raw_args[2], raw_args[3], (int_t) raw_args[4]));
        return true;
    case 265:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_linkat_guest(
                (fd_t) raw_args[0], raw_args[1], (fd_t) raw_args[2], raw_args[3]));
        return true;
    case 266:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_symlinkat_guest(
                raw_args[0], (fd_t) raw_args[1], raw_args[2]));
        return true;
    case 267:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_readlinkat_guest(
                (fd_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]));
        return true;
    case 268:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_fchmodat_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]));
        return true;
    case 269:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_faccessat_guest(
                (fd_t) raw_args[0], raw_args[1], (mode_t_) raw_args[2], 0));
        return true;
    case 439:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_faccessat_guest(
                (fd_t) raw_args[0], raw_args[1], (mode_t_) raw_args[2], (dword_t) raw_args[3]));
        return true;
    case 270:
    {
        dword_t result = sys_pselect_amd64_guest(
                    (fd_t) raw_args[0], raw_args[1], raw_args[2], raw_args[3], raw_args[4], raw_args[5]);
        if (syscall_result_should_restart(result))
            prepare_syscall_restart(cpu, &amd64_syscall_dispatch, syscall_num, raw_args[0]);
        else
            amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) result);
        return true;
    }
    case 271:
    {
        dword_t result = sys_ppoll_amd64_guest(
                    raw_args[0], (dword_t) raw_args[1], raw_args[2], raw_args[3], (dword_t) raw_args[4]);
        if (syscall_result_should_restart(result))
            prepare_syscall_restart(cpu, &amd64_syscall_dispatch, syscall_num, raw_args[0]);
        else
            amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) result);
        return true;
    }
    case 273:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_set_robust_list_amd64_guest(
                raw_args[0], (dword_t) raw_args[1]));
        return true;
    case 274:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_get_robust_list_amd64_guest(
                (pid_t_) raw_args[0], raw_args[1], raw_args[2]));
        return true;
    case 280:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_utimensat_amd64_guest(
                (fd_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]));
        return true;
    case 282:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_signalfd_guest(
                (int_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]));
        return true;
    case 281:
    {
        dword_t result = sys_epoll_pwait_guest(
                    (fd_t) raw_args[0], raw_args[1], (int_t) raw_args[2], (int_t) raw_args[3],
                    raw_args[4], (dword_t) raw_args[5]);
        if (syscall_result_should_restart(result))
            prepare_syscall_restart(cpu, &amd64_syscall_dispatch, syscall_num, raw_args[0]);
        else
            amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) result);
        return true;
    }
    case 283:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_timerfd_create(
                (int_t) raw_args[0], (int_t) raw_args[1]));
        return true;
    case 289:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_signalfd4_guest(
                (int_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (int_t) raw_args[3]));
        return true;
    case 299:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_recvmmsg_amd64_guest(
                (fd_t) raw_args[0], raw_args[1], (uint_t) raw_args[2], (int_t) raw_args[3],
                raw_args[4]));
        return true;
    case 302:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_prlimit64_guest(
                (pid_t_) raw_args[0], (dword_t) raw_args[1], raw_args[2], raw_args[3]));
        return true;
    case 188:
    case 189:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_setxattr_guest(
                raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3], (dword_t) raw_args[4]));
        return true;
    case 190:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_fsetxattr_guest(
                (fd_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3], (dword_t) raw_args[4]));
        return true;
    case 191:
    case 192:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_getxattr_guest(
                raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]));
        return true;
    case 193:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_fgetxattr_guest(
                (fd_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]));
        return true;
    case 194:
    case 195:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_listxattr_guest(
                raw_args[0], raw_args[1], (dword_t) raw_args[2]));
        return true;
    case 196:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_flistxattr_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]));
        return true;
    case 197:
    case 198:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_removexattr_guest(
                raw_args[0], raw_args[1]));
        return true;
    case 199:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_fremovexattr_guest(
                (fd_t) raw_args[0], raw_args[1]));
        return true;
    case 307:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_sendmmsg_amd64_guest(
                (fd_t) raw_args[0], raw_args[1], (uint_t) raw_args[2], (int_t) raw_args[3]));
        return true;
    case 286:
        // amd64 itimerspec is the 64-bit layout -> use the 64-bit marshalling.
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_timerfd_settime64_guest(
                (fd_t) raw_args[0], (int_t) raw_args[1], raw_args[2], raw_args[3]));
        return true;
    case 287:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_timerfd_gettime64_guest(
                (fd_t) raw_args[0], raw_args[1]));
        return true;
    case 332:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_statx_amd64_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (dword_t) raw_args[3],
                raw_args[4]));
        return true;
    case 319:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_memfd_create_guest(
                raw_args[0], (uint_t) raw_args[1]));
        return true;
    case 334:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_rseq_guest(
                raw_args[0], (dword_t) raw_args[1], (dword_t) raw_args[2], (dword_t) raw_args[3]));
        return true;
    case 237:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_mbind_guest(
                raw_args[0], raw_args[1], (int_t) raw_args[2], raw_args[3], raw_args[4],
                (uint_t) raw_args[5]));
        return true;
    case 238:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_set_mempolicy_guest(
                (int_t) raw_args[0], raw_args[1], raw_args[2]));
        return true;
    case 239:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_get_mempolicy_guest(
                raw_args[0], raw_args[1], raw_args[2], raw_args[3], raw_args[4]));
        return true;
    case 310:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_process_vm_readv_guest(
                (pid_t_) raw_args[0], raw_args[1], (dword_t) raw_args[2], raw_args[3],
                (dword_t) raw_args[4], (dword_t) raw_args[5]));
        return true;
    case 72:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_fcntl_amd64_guest(
                (fd_t) raw_args[0], (dword_t) raw_args[1], raw_args[2]));
        return true;
    case 29:
    case 395:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_shmget_guest(
                (dword_t) raw_args[0], raw_args[1], (dword_t) raw_args[2]));
        return true;
    case 30:
    case 397:
        amd64_syscall_result_qword(cpu, (qword_t) sys_shmat_guest(
                (int_t) raw_args[0], raw_args[1], (int_t) raw_args[2]));
        return true;
    case 67:
    case 398:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_shmdt_guest(raw_args[0]));
        return true;
    case 31:
    case 396:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_shmctl_guest(
                (int_t) raw_args[0], (int_t) raw_args[1], raw_args[2]));
        return true;
    // SysV semaphores (fakeroot); semctl's union semun rides full-width.
    case 64:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_semget_guest(
                (dword_t) raw_args[0], (int_t) raw_args[1], (int_t) raw_args[2]));
        return true;
    case 65:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_semop_guest(
                (int_t) raw_args[0], raw_args[1], (uint_t) raw_args[2]));
        return true;
    case 66:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_semctl_guest(
                (int_t) raw_args[0], (int_t) raw_args[1], (int_t) raw_args[2], raw_args[3]));
        return true;
    case 220:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_semtimedop_guest(
                (int_t) raw_args[0], raw_args[1], (uint_t) raw_args[2], raw_args[3]));
        return true;
    // SysV message queues (fakeroot); full-width msgp/msgtyp.
    case 68:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_msgget_guest(
                (dword_t) raw_args[0], (int_t) raw_args[1]));
        return true;
    case 69:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_msgsnd_guest(
                (int_t) raw_args[0], raw_args[1], raw_args[2], (int_t) raw_args[3]));
        return true;
    case 70:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_msgrcv_guest(
                (int_t) raw_args[0], raw_args[1], raw_args[2], (sqword_t) raw_args[3],
                (int_t) raw_args[4]));
        return true;
    case 71:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_msgctl_guest(
                (int_t) raw_args[0], (int_t) raw_args[1], raw_args[2]));
        return true;
    case 297:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_rt_tgsigqueueinfo_guest(
                (pid_t_) raw_args[0], (pid_t_) raw_args[1], (dword_t) raw_args[2], raw_args[3]));
        return true;
    case 56:
        // x86_64 clone syscall order is flags, stack, parent_tid, child_tid, tls.
        // sys_clone_guest uses the legacy internal order flags, stack, parent_tid, tls, child_tid.
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_clone_guest(
                raw_args[0], raw_args[1], raw_args[2], raw_args[4], raw_args[3]));
        return true;
    case 61:
    {
        dword_t result = sys_wait4_guest(
                (pid_t_) raw_args[0], raw_args[1], (dword_t) raw_args[2], raw_args[3]);
        if (syscall_result_should_restart(result)) {
            prepare_syscall_restart(cpu, &amd64_syscall_dispatch, 61, raw_args[0]);
        } else {
            amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) result);
        }
        return true;
    }
    case 203:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_sched_setaffinity_guest(
                (pid_t_) raw_args[0], (dword_t) raw_args[1], raw_args[2]));
        return true;
    case 204:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_sched_getaffinity_guest(
                (pid_t_) raw_args[0], (dword_t) raw_args[1], raw_args[2]));
        return true;
    case 247:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_waitid_guest(
                (int_t) raw_args[0], (pid_t_) raw_args[1], raw_args[2], (int_t) raw_args[3]));
        return true;
    case 422:
    {
        dword_t result = sys_futex_time64_guest(
                raw_args[0], (dword_t) raw_args[1], (dword_t) raw_args[2], raw_args[3],
                raw_args[4], (dword_t) raw_args[5]);
        if (syscall_result_should_restart(result)) {
            prepare_syscall_restart(cpu, &amd64_syscall_dispatch, syscall_num, raw_args[0]);
        } else {
            amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) result);
        }
        return true;
    }
    case 421:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_rt_sigtimedwait_time64_guest(
                raw_args[0], raw_args[1], raw_args[2], (uint_t) raw_args[3]));
        return true;
    case 417:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_recvmmsg_time64_guest(
                (fd_t) raw_args[0], raw_args[1], (uint_t) raw_args[2], (int_t) raw_args[3],
                raw_args[4]));
        return true;
    case 403:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_clock_gettime64_guest(
                (dword_t) raw_args[0], raw_args[1]));
        return true;
    case 406:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_clock_getres_time64_guest(
                (dword_t) raw_args[0], raw_args[1]));
        return true;
    case 407:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_clock_nanosleep_time64_guest(
                (dword_t) raw_args[0], (int_t) raw_args[1], raw_args[2], raw_args[3]));
        return true;
    case 408:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_timer_gettime64_guest(
                (dword_t) raw_args[0], raw_args[1]));
        return true;
    case 409:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_timer_settime64_guest(
                (dword_t) raw_args[0], (int_t) raw_args[1], raw_args[2], raw_args[3]));
        return true;
    case 410:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_timerfd_gettime64_guest(
                (fd_t) raw_args[0], raw_args[1]));
        return true;
    case 411:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_timerfd_settime64_guest(
                (fd_t) raw_args[0], (int_t) raw_args[1], raw_args[2], raw_args[3]));
        return true;
    case 412: // utimensat64 == utimensat on amd64 (64-bit time_t); use the full-width handler
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_utimensat_amd64_guest(
                (fd_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]));
        return true;
    case 413: // pselect6_time64 == pselect6 on amd64; use the full-width handler
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_pselect_amd64_guest(
                (fd_t) raw_args[0], raw_args[1], raw_args[2], raw_args[3], raw_args[4], raw_args[5]));
        return true;
    case 414: // ppoll_time64 == ppoll on amd64; use the full-width handler
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_ppoll_amd64_guest(
                raw_args[0], (dword_t) raw_args[1], raw_args[2], raw_args[3], (dword_t) raw_args[4]));
        return true;
    case 59:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_execve_guest(
                raw_args[0], raw_args[1], raw_args[2]));
        return true;
    case 322:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_execveat_guest(
                (fd_t) raw_args[0], raw_args[1], raw_args[2], raw_args[3], (int_t) raw_args[4]));
        return true;
    case 437:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_openat2_guest(
                (fd_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]));
        return true;
    case 435:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_clone3_guest(
                raw_args[0], (dword_t) raw_args[1]));
        return true;
    case 436:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_close_range(
                (dword_t) raw_args[0], (dword_t) raw_args[1], (dword_t) raw_args[2]));
        return true;
    case 115:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_getgroups_guest(
                (dword_t) raw_args[0], raw_args[1]));
        return true;
    case 116:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_setgroups_guest(
                (dword_t) raw_args[0], raw_args[1]));
        return true;
    case 441:
    {
        dword_t result = sys_epoll_pwait2_guest(
                    (fd_t) raw_args[0], raw_args[1], (int_t) raw_args[2], raw_args[3], raw_args[4],
                    (dword_t) raw_args[5]);
        if (syscall_result_should_restart(result))
            prepare_syscall_restart(cpu, &amd64_syscall_dispatch, syscall_num, raw_args[0]);
        else
            amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) result);
        return true;
    }
    case 452:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_fchmodat2_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2], (dword_t) raw_args[3]));
        return true;
    case 40: // sendfile(out_fd, in_fd, off_t *offset, size_t count) -- native so
             // the 64-bit count and offset pointer survive the marshaller
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_sendfile_guest(
                (fd_t) raw_args[0], (fd_t) raw_args[1], raw_args[2], raw_args[3]));
        return true;
    case 326: // copy_file_range(fd_in, off_in, fd_out, off_out, len, flags)
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_copy_file_range_guest(
                (fd_t) raw_args[0], raw_args[1], (fd_t) raw_args[2], raw_args[3],
                raw_args[4], (uint_t) raw_args[5]));
        return true;
    // preadv/pwritev(fd, iov, iovcnt, pos_l, pos_h) -- native so the 64-bit
    // iovec pointer and a >4GiB offset survive the marshaller. On a 64-bit ABI
    // pos_l already carries the whole offset (the kernel's pos_from_hilo
    // shifts pos_h clean out of a 64-bit loff_t), same as arm64's 69/70.
    case 295:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_preadv_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2],
                (off_t_) raw_args[3]));
        return true;
    case 296:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_pwritev_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2],
                (off_t_) raw_args[3]));
        return true;
    case 327: // preadv2(fd, iov, iovcnt, pos_l, pos_h, flags) -- native so the
              // 64-bit iovec pointer and pos survive the marshaller
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_preadv2_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2],
                (off_t_) (raw_args[3] | (raw_args[4] << 32)), (uint_t) raw_args[5]));
        return true;
    case 328: // pwritev2(fd, iov, iovcnt, pos_l, pos_h, flags)
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_pwritev2_guest(
                (fd_t) raw_args[0], raw_args[1], (dword_t) raw_args[2],
                (off_t_) (raw_args[3] | (raw_args[4] << 32)), (uint_t) raw_args[5]));
        return true;
    // vmsplice(fd, iov, iovcnt, flags). The legacy table routed this to
    // sys_vmsplice, which is the i386 entry point: it takes a 32-bit addr_t
    // and, worse, hardcodes GUEST_ABI_I386 for the iovec layout. An amd64
    // iovec is {void *64, size_t 64}; read as i386's {u32, u32}, the base and
    // length of every entry come out of the wrong halves, so the call failed
    // with EFAULT on addresses that were perfectly valid. The legacy dispatch's
    // dword-fit check could never have caught it: a guest stack address does
    // fit in 32 bits, and it was the STRUCT being misread, not the pointer.
    //
    // sys_vmsplice_guest takes the full-width address and uses current->abi,
    // which is how arm64 has always reached it (case 75 of the arm64 legacy
    // switch, which is why only amd64 failed). readv/writev above are the
    // same shape.
    case 278:
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_vmsplice_guest(
                (fd_t) raw_args[0], raw_args[1], raw_args[2], (dword_t) raw_args[3]));
        return true;
    // fsopen/fsconfig/move_mount (new mount API, fs/mount.c) carry real
    // 64-bit guest pointer args (fsname/key/value/from_path/to_path) that
    // would trip the legacy marshalled dispatch's dword-fit check on a
    // typical amd64 mmap address -- native, same reasoning as sendfile/
    // copy_file_range above. fsmount has no pointer args and stays on the
    // plain legacy path (see amd64_syscall_legacy_arg_count).
    case 430: // fsopen(fsname, flags)
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_fsopen_guest(
                raw_args[0], (dword_t) raw_args[1]));
        return true;
    case 431: // fsconfig(fd, cmd, key, value, aux)
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_fsconfig_guest(
                (fd_t) raw_args[0], (dword_t) raw_args[1], raw_args[2], raw_args[3],
                (int_t) raw_args[4]));
        return true;
    case 429: // move_mount(from_dfd, from_path, to_dfd, to_path, flags)
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_move_mount_guest(
                (fd_t) raw_args[0], raw_args[1], (fd_t) raw_args[2], raw_args[3],
                (dword_t) raw_args[4]));
        return true;

    // The io_* family, for the same reason again, and here it is unavoidable
    // rather than merely typical: an aio_context_t IS the address of the
    // context's ring page, so EVERY call after io_setup carries a 64-bit
    // address in arg 0. The legacy path refused to truncate it and SIGSYS'd
    // io_submit -- correctly; truncating an opaque handle would have been the
    // worse outcome. min_nr/nr are longs, hence the sqword casts.
    case 206: // io_setup(nr_events, ctx_idp)
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_io_setup_guest(
                (uint_t) raw_args[0], raw_args[1]));
        return true;
    case 207: // io_destroy(ctx)
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_io_destroy_guest(
                raw_args[0]));
        return true;
    case 208: // io_getevents(ctx, min_nr, nr, events, timeout)
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_io_getevents_guest(
                raw_args[0], (sqword_t) raw_args[1], (sqword_t) raw_args[2],
                raw_args[3], raw_args[4]));
        return true;
    case 209: // io_submit(ctx, nr, iocbpp)
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_io_submit_guest(
                raw_args[0], (sqword_t) raw_args[1], raw_args[2]));
        return true;
    case 210: // io_cancel(ctx, iocb, result)
        amd64_syscall_result_qword(cpu, (qword_t) (sqword_t) sys_io_cancel_guest(
                raw_args[0], raw_args[1], raw_args[2]));
        return true;
    default:
        return false;
    }
}

static unsigned amd64_syscall_legacy_arg_count(qword_t syscall_num) {
    switch (syscall_num) {
    // Corrected arg counts: these handlers read fewer/more args than the grouped
    // fall-through below would validate. An over-count makes the marshaller check
    // unused upper arg registers (which hold caller garbage) and SIGSYS; an
    // under-count zero-fills a real arg. Same bug class as fchmod(91)/keyctl(250).
    case 200: // tkill(tid, sig) -- was grouped as 1, dropping the signal number
    case 324: // membarrier -- base ABI is (cmd, flags); the optional 3rd arg cpuid is
              // unset garbage in liburcu's 2-arg syscall(__NR_membarrier, cmd, flags)
              // calls (glibc's syscall() varargs leaves rdx untouched), and
              // sys_membarrier ignores flags+cpuid anyway. Over-counting to 3 made the
              // marshaller validate the garbage rdx and SIGSYS syslog-ng.
        return 2;
    // The io_* family (206-210) is absent on purpose: it is dispatched
    // natively in handle_amd64_native_memory_syscall and never reaches this
    // classifier. It cannot -- an aio_context_t is the ring page's address,
    // so arg 0 is a real 64-bit pointer and no arity would make it fit a
    // dword. Narrowing the count here was the first attempt and it did not
    // help; the marshaller was right to refuse.
    case 145: // sched_getscheduler(pid)
    case 146: // sched_get_priority_max(policy)
    case 147: // sched_get_priority_min(policy)
    case 213: // epoll_create(size) -- handler ignores size
    case 306: // syncfs(fd) -- single fd arg; upper regs are garbage
        return 1;
    case 277: // sync_file_range -- success stub ignores all args
    case 152: // munlockall() -- no args at all
        return 0;
    case 15:  // rt_sigreturn
    case 24:  // sched_yield
    case 34:  // pause
    case 39:  // getpid
    case 57:  // fork
    case 58:  // vfork
    case 102: // getuid
    case 104: // getgid
    case 107: // geteuid
    case 108: // getegid
    case 110: // getppid
    case 111: // getpgrp
    case 112: // setsid
    case 153: // vhangup (no args; tty-cleanup no-op stub)
    case 162: // sync
    case 186: // gettid
    case 187: // readahead stubbed
    case 221: // fadvise64 stubbed (advisory; args ignored)
    case 253: // inotify_init
    case 303: // name_to_handle_at (EOPNOTSUPP stub; args ignored, and its
    case 304: // open_by_handle_at  pointer args are 64-bit guest addresses)
    case 317: // seccomp (EOPNOTSUPP stub; arg3 is a 64-bit sock_fprog* -- man-db's
              // sandbox probe; classify 0-arg so the pointer doesn't SIGSYS the marshaller)
    case 309: // getcpu stubbed
    case 425: // io_uring_setup    (ENOSYS stub; callers use ordinary syscalls)
    case 426: // io_uring_enter
    case 427: // io_uring_register
    case 428: // open_tree   (silent ENOSYS stub; args ignored, so skip full-width arg validation)
    // move_mount/fsopen/fsconfig/fsmount (429-432) are now real implementations
    // (fs/mount.c) taking genuine pointer args -- removed from this 0-arg
    // group so the default full-width classification below validates them.
    case 433: // fspick
    case 438: // pidfd_getfd
    case 440: // process_madvise
    // Same reasoning: silent ENOSYS stubs whose args are never read, several
    // of which take real 64-bit guest pointers (mount_setattr's uattr,
    // landlock's ruleset/rule attr, cachestat's range/result structs) that
    // would otherwise trip the dword-fit check and SIGSYS the caller (the
    // arm64 table hit this exact bug with process_mrelease/448).
    case 442: // mount_setattr
    case 443: // quotactl_fd
    case 444: // landlock_create_ruleset
    case 445: // landlock_add_rule
    case 446: // landlock_restrict_self
    case 447: // memfd_secret
    case 448: // process_mrelease
    case 449: // futex_waitv
    case 450: // set_mempolicy_home_node
    case 451: // cachestat
    case 453: // map_shadow_stack
    // 454-462: see arm64's matching comment -- same 0-arg treatment.
    case 454: // futex_wake
    case 455: // futex_wait
    case 456: // futex_requeue
    case 457: // statmount
    case 458: // listmount
    case 459: // lsm_get_self_attr
    case 460: // lsm_set_self_attr
    case 461: // lsm_list_modules
    case 462: // mseal
    // 463-469: xattrat family / open_tree_attr / file_[gs]etattr silent
    // stubs -- path/name/args pointers are full 64-bit guest addresses.
    case 463: case 464: case 465: case 466: case 467: case 468: case 469:
        return 0;
    case 3:   // close
    case 12:  // brk
    case 22:  // pipe
    case 32:  // dup
    case 37:  // alarm
    case 60:  // exit
    case 63:  // uname
    case 74:  // fsync
    case 75:  // fdatasync
    case 80:  // chdir
    case 81:  // fchdir
    case 84:  // rmdir
    case 87:  // unlink
    case 95:  // umask
    case 97:  // getrlimit
    case 98:  // getrusage
    case 99:  // sysinfo
    case 100: // times
    case 105: // setuid
    case 106: // setgid
    case 121: // getpgid
    case 124: // getsid
    case 122: // setfsuid
    case 123: // setfsgid
    case 125: // capget
    case 126: // capset
    case 127: // rt_sigpending
    case 130: // rt_sigsuspend
    case 135: // personality
    case 149: // mlock
    case 150: // munlock
    case 151: // mlockall(flags) -- x86-64 rsi-rbp are caller garbage here
    case 161: // chroot
    case 164: // settimeofday
    case 166: // umount2
    case 170: // sethostname
    case 201: // time
    case 218: // set_tid_address
    case 231: // exit_group
    case 272: // unshare
    case 273: // set_robust_list
    case 284: // eventfd
    case 291: // epoll_create
    case 294: // inotify_init1
    case 225: // timer_getoverrun
    case 226: // timer_delete
    case 250: // keyctl (sys_keyctl stub only switches on cmd; arg2-arg5 are ignored)
        return 1;
    case 4:   // stat
    case 5:   // fstat
    case 6:   // lstat
    case 132: // utime
    case 21:  // access
    case 33:  // dup2
    case 48:  // shutdown
    case 50:  // listen
    case 62:  // kill
    case 73:  // flock
    case 79:  // getcwd
    case 82:  // rename
    case 85:  // creat
    case 86:  // link
    case 88:  // symlink
    case 90:  // chmod
    case 91:  // fchmod
    case 109: // setpgid
    case 113: // setreuid
    case 114: // setregid
    case 115: // getgroups
    case 116: // setgroups
    case 35:  // nanosleep
    case 36:  // getitimer
    case 96:  // gettimeofday
    case 131: // sigaltstack
    case 140: // getpriority
    case 142: // sched_setparam
    case 143: // sched_getparam
    case 144: // sched_setscheduler
    case 148: // sched_rr_get_interval(pid, tp)
    case 171: // setdomainname(name, len)
    case 160: // setrlimit
    case 158: // arch_prctl
    case 229: // clock_getres
    case 235: // utimes
    case 252: // ioprio_get
    case 255: // inotify_rm_watch
    case 283: // timerfd_create
    case 287: // timerfd_gettime
    case 228: // clock_gettime
    case 290: // eventfd2
    case 293: // pipe2
    case 403: // clock_gettime64
    case 404: // clock_settime64
    case 406: // clock_getres_time64
    case 408: // timer_gettime64
    case 410: // timerfd_gettime64
    case 435: // clone3
    case 319: // memfd_create
    case 434: // pidfd_open(pid, flags)
        return 2;
    case 0:   // read
    case 1:   // write
    case 2:   // open
    case 7:   // poll
    case 8:   // lseek
    case 10:  // mprotect
    case 16:  // ioctl
    case 19:  // readv
    case 20:  // writev
    case 26:  // msync
    case 28:  // madvise
    case 31:  // shmctl
    case 41:  // socket
    case 42:  // connect
    case 43:  // accept
    case 46:  // sendmsg
    case 47:  // recvmsg
    case 49:  // bind
    case 51:  // getsockname
    case 52:  // getpeername
    case 72:  // fcntl
    case 83:  // mkdir
    case 89:  // readlink
    case 92:  // chown
    case 93:  // fchown
    case 94:  // lchown
    case 101: // ptrace
    case 117: // setresuid
    case 118: // getresuid
    case 119: // setresgid
    case 120: // getresgid
    case 133: // mknod
    case 141: // setpriority
    case 165: // mount
    case 203: // sched_setaffinity
    case 204: // sched_getaffinity
    case 217: // getdents64
    case 227: // clock_settime
    case 230: // clock_nanosleep
    case 234: // tgkill
    case 129: // rt_sigqueueinfo
    case 178: // rt_sigqueueinfo
    case 297: // rt_tgsigqueueinfo
    case 251: // ioprio_set
    case 254: // inotify_add_watch
    case 258: // mkdirat
    case 261: // futimesat
    case 263: // unlinkat
    case 266: // symlinkat
    case 268: // fchmodat
    case 269: // faccessat
    case 103: // syslog
    case 38:  // setitimer
    case 59:  // execve
    case 169: // reboot
    case 292: // dup3
    case 274: // get_robust_list
    case 282: // signalfd
    case 334: // rseq
    case 222: // timer_create
    case 436: // close_range
        return 3;
    case 13:  // rt_sigaction
    case 14:  // rt_sigprocmask
    case 78:  // getdents
    case 128: // rt_sigtimedwait
    case 232: // epoll_wait
    case 233: // epoll_ctl
    case 53:  // socketpair
    case 54:  // setsockopt
    case 61:  // wait4
    case 257: // openat
    case 259: // mknodat
    case 262: // newfstatat
    case 264: // renameat
    case 267: // readlinkat
    case 280: // utimensat
    case 288: // accept4
    case 289: // signalfd4
    case 302: // prlimit64
    case 307: // sendmmsg
    case 223: // timer_settime
    case 407: // clock_nanosleep_time64
    case 409: // timer_settime64
    case 411: // timerfd_settime64
    case 412: // utimensat64
    case 421: // rt_sigtimedwait_time64
    case 437: // openat2
    case 439: // faccessat2 wired to faccessat for now
    case 452: // fchmodat2
    case 424: // pidfd_send_signal(pidfd, sig, info, flags) -- info is ignored
              // (UNUSED, never dereferenced), so the raw pointer arg is safe here
        return 4;
    case 56:  // clone
    case 25:  // mremap
    case 55:  // getsockopt
    case 157: // prctl
    case 23:  // select
    case 260: // fchownat
    case 265: // linkat
    case 271: // ppoll
    case 286: // timerfd_settime
    case 299: // recvmmsg
    case 316: // renameat2
    case 332: // statx
    case 322: // execveat
    case 247: // waitid
    case 312: // kcmp(pid1, pid2, type, idx1, idx2)
    case 414: // ppoll_time64
    case 417: // recvmmsg_time64
        return 5;
    // fsopen/fsconfig/move_mount (429-431) are handled natively in
    // handle_amd64_native_memory_syscall (their pointer args need full
    // width) and never reach this classifier. fsmount (432, no pointer
    // args) is the one new-mount-API syscall still on this legacy path.
    case 432: // fsmount(fd, flags, attr_flags)
        return 3;
    case 9:   // mmap
    case 44:  // sendto
    case 45:  // recvfrom
    case 202: // futex
    case 275: // splice
    case 310: // process_vm_readv
    case 413: // pselect_time64
    case 422: // futex_time64
    case 441: // epoll_pwait2
        return 6;
    default:
        return 6;
    }
}


// Argument count for the AArch64 syscalls still dispatched through the
// legacy 32-bit-marshalled table. Everything listed takes only ints/fds
// that fit in 32 bits; unused arg registers hold caller garbage (the
// raw-syscall() lesson from the amd64 port), so validation must only
// look at the args a syscall actually has. Any number NOT listed
// defaults to 6 = validate everything, so a pointer-taking syscall
// added to the legacy table by mistake fails loudly with SIGSYS
// instead of silently truncating (how dmesg broke).
static unsigned arm64_syscall_legacy_arg_count(qword_t syscall_num) {
    switch (syscall_num) {
    case 258: // riscv_hwprobe: ENOSYS stub ignores its 5 args, one of which
              // is a full-width pointer — classify 0-arg so it can't SIGSYS
              // (the pidfd_open/seccomp precedent; apk probes at startup)
    case 259: // riscv_flush_icache: no-op stub; start/end args are 64-bit
    case 124: // sched_yield
    case 157: // setsid
    case 172: // getpid
    case 173: // getppid
    case 174: // getuid
    case 175: // geteuid
    case 176: // getgid
    case 177: // getegid
    case 178: // gettid
    case 231: // munlockall
    // New mount API family: silent ENOSYS stubs (arm64_syscall_table) whose
    // args are entirely ignored, so classify 0-arg to skip validating their
    // filename/path pointers -- ordinary 64-bit guest addresses that would
    // otherwise trip the legacy dword-fit check and SIGSYS the caller
    // instead of the intended silent ENOSYS (mount(8) probing the new API).
    // move_mount/fsopen/fsconfig/fsmount (429-432) are now real
    // implementations (fs/mount.c) taking genuine pointer args -- removed
    // from this 0-arg group so the default full-width classification below
    // validates them.
    case 428: // open_tree
    case 433: // fspick
    // Same reasoning: silent ENOSYS stubs (arm64_syscall_table) that never
    // read their args, several of which take real 64-bit guest pointers
    // (mount_setattr's uattr, landlock's ruleset/rule attr, cachestat's
    // range/result structs) that would otherwise trip the dword-fit check
    // and SIGSYS the caller instead of the intended silent ENOSYS (observed:
    // stress-ng's process_mrelease (448) probe got SIGSYS'd this way).
    case 442: // mount_setattr
    case 443: // quotactl_fd
    case 444: // landlock_create_ruleset
    case 445: // landlock_add_rule
    case 446: // landlock_restrict_self
    case 447: // memfd_secret
    case 448: // process_mrelease
    case 450: // set_mempolicy_home_node
    case 451: // cachestat
    case 453: // map_shadow_stack
    // 454-462: next batch of post-6.x syscalls (see arm64_syscall_table's
    // matching comment) -- same 0-arg treatment for the same reason.
    case 454: // futex_wake
    case 455: // futex_wait
    case 456: // futex_requeue
    case 457: // statmount
    case 458: // listmount
    case 459: // lsm_get_self_attr
    case 460: // lsm_set_self_attr
    case 461: // lsm_list_modules
    case 462: // mseal
    // 463-469: xattrat family / open_tree_attr / file_[gs]etattr silent
    // stubs -- path/name/args pointers are full 64-bit guest addresses
    // (getxattrat/464 is the one Arch useradd's skel copy actually probes).
    case 463: case 464: case 465: case 466: case 467: case 468: case 469:
        return 0;
    case 20:  // epoll_create1
    case 23:  // dup
    case 26:  // inotify_init1
    case 50:  // fchdir
    case 57:  // close
    case 82:  // fsync
    case 83:  // fdatasync
    case 267: // syncfs(fd) -- single fd arg; upper regs are caller garbage
    case 92:  // personality
    case 93:  // exit
    case 94:  // exit_group
    case 97:  // unshare(flags) -- x1-x5 are caller garbage (only x0 is set),
              // which would otherwise fail the dword-fit check and SIGSYS
              // (e.g. dpkg/apt maintainer scripts sandboxing via unshare)
    case 109: // timer_getoverrun
    case 111: // timer_delete
    case 120: // sched_getscheduler
    case 125: // sched_get_priority_max
    case 126: // sched_get_priority_min
    case 144: // setgid
    case 146: // setuid
    case 151: // setfsuid
    case 152: // setfsgid
    case 155: // getpgid
    case 156: // getsid
    case 166: // umask
    case 219: // keyctl (sys_keyctl only switches on cmd; arg2-arg5 are ignored --
              // arg2 is a pointer for KEYCTL_JOIN_SESSION_KEYRING/etc, so
              // over-counting would validate a real 64-bit guest address the
              // function never reads and SIGSYS login's pam_keyinit)
    case 230: // mlockall
        return 1;
    case 19:  // eventfd2
    case 28:  // inotify_rm_watch
    case 31:  // ioprio_get
    case 32:  // flock
    case 52:  // fchmod
    case 85:  // timerfd_create
    case 129: // kill
    case 130: // tkill
    case 141: // getpriority
    case 143: // setregid
    case 145: // setreuid
    case 154: // setpgid
    case 201: // listen
    case 210: // shutdown
    case 434: // pidfd_open(pid, flags)
    case 430: // fsopen(fsname, flags) -- real implementation (fs/mount.c); must
              // classify its true arg count, not fall to the default(6) below,
              // or an unused upper register full of caller garbage SIGSYS's
              // the marshaller.
        return 2;
    case 24:  // dup3
    case 30:  // ioprio_set
    case 55:  // fchown
    case 131: // tgkill
    case 140: // setpriority
    case 142: // reboot (4th arg is a pointer sys_reboot never reads)
    case 147: // setresuid
    case 149: // setresgid
    case 198: // socket
    case 436: // close_range
    case 432: // fsmount(fd, flags, attr_flags) -- same reasoning as fsopen above
        return 3;
    case 424: // pidfd_send_signal(pidfd, sig, info, flags) -- info is ignored
              // (UNUSED, never dereferenced), so the raw pointer arg is safe here
        return 4;
    case 272: // kcmp(pid1, pid2, type, idx1, idx2)
    case 286: // preadv2(fd, iov, iovcnt, offset, flags)
    case 287: // pwritev2(fd, iov, iovcnt, offset, flags)
    case 429: // move_mount(from_dfd, from_path, to_dfd, to_path, flags) -- same
              // reasoning as fsopen above
    case 431: // fsconfig(fd, cmd, key, value, aux) -- same reasoning
        return 5;
    default:
        return 6;
    }
}

// True when every argument this syscall actually uses is a 32-bit scalar (fd,
// flag, mode, pid, signal, uid, ...) rather than a pointer or a 64-bit value.
//
// Both 64-bit psABIs leave the UPPER HALF of an argument register undefined
// when the argument's declared type is 32 bits, and real Linux truncates
// accordingly (SYSCALL_DEFINEn casts each arg to its declared type before the
// handler sees it). So the dword-fit check in marshal_syscall_args_legacy must
// not run for these: a caller that legally left junk above bit 31 of a 32-bit
// argument would be killed for making a perfectly valid syscall. Observed: uv
// (Rust) calls fchmod(fd, 0666) with the fd loaded as a whole 64-bit stack
// word -- LLVM knows only w0 is architecturally meaningful, so the neighbouring
// struct field rides along in the upper half -- and `uv venv` died with "Bad
// system call" on an aarch64 guest.
//
// The lists are exhaustive for what actually reaches the legacy marshaller:
// every other syscall is either dispatched full-width by
// handle_asm_generic_native_syscall / handle_amd64_native_memory_syscall, or
// is a stub classified 0-arg by the arg-count tables above. Anything NOT
// listed keeps the old validate-everything behaviour, so a pointer-taking
// syscall added to a legacy table by mistake still fails loudly instead of
// silently truncating a guest address (how arm64 dmesg broke).
static bool syscall_legacy_args_are_scalars(enum guest_abi abi, qword_t syscall_num) {
    // riscv64 shares arm64's numbering and its legacy table (see
    // riscv64_syscall_dispatch), so it shares this list too.
    if (abi == GUEST_ABI_ARM64 || abi == GUEST_ABI_RISCV64) {
        switch (syscall_num) {
        case 19:  // eventfd2
        case 20:  // epoll_create
        case 23:  // dup
        case 24:  // dup3
        case 26:  // inotify_init1
        case 28:  // inotify_rm_watch
        case 30:  // ioprio_set
        case 31:  // ioprio_get
        case 32:  // flock
        case 50:  // fchdir
        case 52:  // fchmod
        case 55:  // fchown
        case 57:  // close
        case 82:  // fsync
        case 83:  // fdatasync
        case 85:  // timerfd_create
        case 92:  // personality
        case 93:  // exit
        case 94:  // exit_group
        case 97:  // unshare
        case 109: // timer_getoverrun
        case 111: // timer_delete
        case 120: // sched_getscheduler
        case 124: // sched_yield
        case 125: // sched_get_priority_max
        case 126: // sched_get_priority_min
        case 129: // kill
        case 130: // tkill
        case 131: // tgkill
        case 140: // setpriority
        case 141: // getpriority
        case 142: // reboot (the 4th arg is a pointer sys_reboot never reads)
        case 143: // setregid
        case 144: // setgid
        case 145: // setreuid
        case 146: // setuid
        case 147: // setresuid
        case 149: // setresgid
        case 151: // setfsuid
        case 152: // setfsgid
        case 154: // setpgid
        case 155: // getpgid
        case 156: // getsid
        case 157: // setsid
        case 166: // umask
        case 172: // getpid
        case 173: // getppid
        case 174: // getuid
        case 175: // geteuid
        case 176: // getgid
        case 177: // getegid
        case 178: // gettid
        case 198: // socket
        case 201: // listen
        case 210: // shutdown
        case 219: // keyctl (sys_keyctl only switches on cmd; arg2-arg5 unread)
        case 230: // mlockall
        case 231: // munlockall
        case 258: // riscv_hwprobe (ENOSYS stub; args unread)
        case 259: // riscv_flush_icache (no-op stub; args unread)
        case 267: // syncfs
        case 424: // pidfd_send_signal (the siginfo pointer is never dereferenced)
        case 432: // fsmount
        case 434: // pidfd_open
        case 436: // close_range
            return true;
        }
        return false;
    }

    if (abi == GUEST_ABI_AMD64) {
        switch (syscall_num) {
        // Deliberately absent, and the reason this is a list rather than a
        // blanket "the legacy path is scalars-only" rule: settimeofday (164),
        // splice (275) and clock_settime64 (404) reach the legacy marshaller
        // WITH real pointer args, so they must keep failing loudly until they
        // get full-width dispatch.
        case 3:   // close
        case 15:  // rt_sigreturn
        case 24:  // sched_yield
        case 32:  // dup
        case 33:  // dup2
        case 34:  // pause
        case 37:  // alarm
        case 39:  // getpid
        case 41:  // socket
        case 48:  // shutdown
        case 50:  // listen
        case 57:  // fork
        case 58:  // vfork
        case 60:  // exit
        case 62:  // kill
        case 73:  // flock
        case 74:  // fsync
        case 75:  // fdatasync
        case 81:  // fchdir
        case 91:  // fchmod
        case 95:  // umask
        case 102: // getuid
        case 104: // getgid
        case 105: // setuid
        case 106: // setgid
        case 107: // geteuid
        case 108: // getegid
        case 109: // setpgid
        case 110: // getppid
        case 111: // getpgrp
        case 112: // setsid
        case 113: // setreuid
        case 114: // setregid
        case 117: // setresuid
        case 119: // setresgid
        case 121: // getpgid
        case 122: // setfsuid
        case 123: // setfsgid
        case 124: // getsid
        case 135: // personality
        case 140: // getpriority
        case 141: // setpriority
        case 145: // sched_getscheduler
        case 146: // sched_get_priority_max
        case 147: // sched_get_priority_min
        case 151: // mlockall
        case 152: // munlockall
        case 169: // reboot (4th arg is a pointer sys_reboot never reads)
        case 186: // gettid
        case 200: // tkill
        case 213: // epoll_create
        case 225: // timer_getoverrun
        case 226: // timer_delete
        case 231: // exit_group
        case 234: // tgkill
        case 250: // keyctl (arg2-arg5 unread, as on arm64)
        case 251: // ioprio_set
        case 252: // ioprio_get
        case 253: // inotify_init
        case 255: // inotify_rm_watch
        case 272: // unshare
        case 284: // eventfd
        case 290: // eventfd2
        case 291: // epoll_create1
        case 292: // dup3
        case 294: // inotify_init1
        case 306: // syncfs
        case 312: // kcmp
        case 324: // membarrier
        case 424: // pidfd_send_signal (siginfo pointer never dereferenced)
        case 432: // fsmount
        case 434: // pidfd_open
            return true;
        }
        return false;
    }

    return false;
}

static bool marshal_syscall_args_legacy(enum guest_abi abi, qword_t syscall_num,
        const qword_t raw_args[6], dword_t args[6]) {
    if (abi == GUEST_ABI_AMD64) {
        switch (syscall_num) {
        case 17: // pread64
        case 18: // pwrite64
            if (!syscall_arg_fits_legacy_dword(raw_args[0]) ||
                    !syscall_arg_fits_legacy_dword(raw_args[1]) ||
                    !syscall_arg_fits_legacy_dword(raw_args[2]))
                return false;
            args[0] = (dword_t) raw_args[0];
            args[1] = (dword_t) raw_args[1];
            args[2] = (dword_t) raw_args[2];
            args[3] = (dword_t) raw_args[3];
            args[4] = (dword_t) (raw_args[3] >> 32);
            args[5] = 0;
            return true;
        case 76: // truncate
            if (!syscall_arg_fits_legacy_dword(raw_args[0]))
                return false;
            args[0] = (dword_t) raw_args[0];
            args[1] = (dword_t) raw_args[1];
            args[2] = (dword_t) (raw_args[1] >> 32);
            args[3] = 0;
            args[4] = 0;
            args[5] = 0;
            return true;
        case 77: // ftruncate
            if (!syscall_arg_fits_legacy_dword(raw_args[0]))
                return false;
            args[0] = (dword_t) raw_args[0];
            args[1] = (dword_t) raw_args[1];
            args[2] = (dword_t) (raw_args[1] >> 32);
            args[3] = 0;
            args[4] = 0;
            args[5] = 0;
            return true;
        case 285: // fallocate
            if (!syscall_arg_fits_legacy_dword(raw_args[0]) ||
                    !syscall_arg_fits_legacy_dword(raw_args[1]))
                return false;
            args[0] = (dword_t) raw_args[0];
            args[1] = (dword_t) raw_args[1];
            args[2] = (dword_t) raw_args[2];
            args[3] = (dword_t) (raw_args[2] >> 32);
            args[4] = (dword_t) raw_args[3];
            args[5] = (dword_t) (raw_args[3] >> 32);
            return true;
        case 56: // clone
            if (!syscall_arg_fits_legacy_dword(raw_args[0]) ||
                    !syscall_arg_fits_legacy_dword(raw_args[1]) ||
                    !syscall_arg_fits_legacy_dword(raw_args[2]) ||
                    !syscall_arg_fits_legacy_dword(raw_args[3]) ||
                    !syscall_arg_fits_legacy_dword(raw_args[4]))
                return false;
            args[0] = (dword_t) raw_args[0];
            args[1] = (dword_t) raw_args[1];
            args[2] = (dword_t) raw_args[2];
            args[3] = (dword_t) raw_args[4];
            args[4] = (dword_t) raw_args[3];
            args[5] = 0;
            return true;
        case 202: // futex
        case 422: // futex_time64
            args[0] = (dword_t) raw_args[0];
            args[1] = (dword_t) raw_args[1];
            args[2] = (dword_t) raw_args[2];
            args[3] = (dword_t) raw_args[3];
            args[4] = (dword_t) raw_args[4];
            args[5] = (dword_t) raw_args[5];
            return true;
        }
    }

    // riscv64 shares arm64's arity table along with its syscall numbering
    // (same asm-generic ABI; see riscv64_syscall_dispatch).
    unsigned arg_count = abi == GUEST_ABI_AMD64 ? amd64_syscall_legacy_arg_count(syscall_num)
                       : (abi == GUEST_ABI_ARM64 || abi == GUEST_ABI_RISCV64)
                           ? arm64_syscall_legacy_arg_count(syscall_num)
                       : 6;
    // ...unless every argument is a 32-bit scalar, in which case the upper
    // half of each argument register is architecturally undefined and there is
    // nothing to validate — see syscall_legacy_args_are_scalars.
    bool scalar_args = syscall_legacy_args_are_scalars(abi, syscall_num);
    for (unsigned i = 0; i < arg_count; i++) {
        // All 64-bit ABIs validate: a 64-bit value reaching the 32-bit
        // marshalling is a dispatch bug, and failing loudly here (SIGSYS)
        // beats silently truncating a pointer (how arm64 dmesg broke).
        if (guest_abi_is_64bit(abi) && !scalar_args &&
                !syscall_arg_fits_legacy_dword(raw_args[i]))
            return false;
        args[i] = (dword_t) raw_args[i];
    }
    for (unsigned i = arg_count; i < 6; i++)
        args[i] = 0;
    return true;
}

// One syscall on behalf of a natively-compiled program (kernel/native.h). This
// is the same dispatch a translated guest gets -- the asm-generic switch first,
// the legacy marshalled table behind it -- reached from host code instead of
// from an SVC.
//
// The numbering is asm-generic (arm64/riscv64), always, whatever ABI the
// calling task's guest image had. A native program is guest-ABI-independent by
// construction, so it needs ONE numbering; picking the task's would mean the
// shim spoke a different language per rootfs. The handlers themselves are
// ABI-neutral -- they take guest addresses and go through user_read/user_write
// -- so only the numbering and the few explicitly-arm64 struct layouts
// (newfstatat, fstat) follow from this choice, and both are what the shim
// marshals against.
//
// STRACE prints the same prefix the guest path does, so a native program's
// calls appear in a trace attributed to its pid and comm, interleaved with
// every other task's. That is most of the point of routing through here.
sqword_t syscall_dispatch_native(qword_t syscall_num, const qword_t raw_args[6]) {
    if (current == NULL)
        return _EFAULT;

    STRACE("%d(%s) %d:%d native call %-3llu ", current->pid, current->comm,
           current->reference.count,
           __atomic_load_n(&current->locks_held.count, __ATOMIC_RELAXED),
           (unsigned long long) syscall_num);

    sqword_t result;
    native_syscall_active = true;
    native_syscall_value = (qword_t) (sqword_t) _ENOSYS;
    bool handled = handle_asm_generic_native_syscall(NULL, syscall_num, raw_args);
    native_syscall_active = false;

    if (handled) {
        result = (sqword_t) native_syscall_value;
    } else if (syscall_num >= arm64_syscall_dispatch.num_syscalls ||
            arm64_syscall_dispatch.table[syscall_num] == NULL) {
        result = _ENOSYS;
    } else {
        // The legacy table truncates every argument to a dword. A native
        // caller's pointers come out of its own scratch region, which is
        // mapped low enough to survive that (kernel/native_syscall.c); a
        // refusal here means the shim passed something that cannot fit, which
        // is a bug in the shim rather than in the guest.
        dword_t args[6];
        if (!marshal_syscall_args_legacy(GUEST_ABI_ARM64, syscall_num, raw_args, args)) {
            printk("ERROR: %d(%s) native syscall %llu needs full-width args "
                   "(args %#llx %#llx %#llx %#llx %#llx %#llx)\n",
                   current->pid, current->comm, (unsigned long long) syscall_num,
                   (unsigned long long) raw_args[0], (unsigned long long) raw_args[1],
                   (unsigned long long) raw_args[2], (unsigned long long) raw_args[3],
                   (unsigned long long) raw_args[4], (unsigned long long) raw_args[5]);
            result = _EFAULT;
        } else {
            syscall_t syscall = arm64_syscall_dispatch.table[syscall_num];
            result = (sqword_t) (sdword_t) syscall(args[0], args[1], args[2],
                    args[3], args[4], args[5]);
        }
    }

    STRACE(" = 0x%llx\n", (unsigned long long) result);
    return result;
}

static void log_stub_syscall(struct cpu_state *cpu, const struct syscall_abi_dispatch *dispatch,
        unsigned syscall_num, const char *kind) {
    enum { STUB_LOG_MAX = 512, STUB_LOG_BUDGET = 5 };
    static unsigned short stub_log_count[2][STUB_LOG_MAX];
    bool always_log = dispatch->abi == GUEST_ABI_AMD64 && strcmp(current->comm, "bash") == 0;

    unsigned short *count = NULL;
    if (!always_log && dispatch->abi < 2 && syscall_num < STUB_LOG_MAX)
        count = &stub_log_count[dispatch->abi][syscall_num];

    if (count != NULL) {
        if (*count >= STUB_LOG_BUDGET) {
            if (*count == STUB_LOG_BUDGET) {
                printk("INFO: suppressing further %s %s syscall logs for %u\n",
                       dispatch->name, kind, syscall_num);
                (*count)++;
            }
            return;
        }
        (*count)++;
    }

    if (dispatch->abi == GUEST_ABI_AMD64) {
        printk("ERROR: %d(%s) %s %s syscall %u rip=0x%llx rax=0x%llx rdi=0x%llx rsi=0x%llx rdx=0x%llx\n",
               current->pid, current->comm, dispatch->name, kind, syscall_num,
               (unsigned long long) cpu->amd64_rip,
               (unsigned long long) cpu->amd64_syscall.rax,
               (unsigned long long) cpu->amd64_syscall.rdi,
               (unsigned long long) cpu->amd64_syscall.rsi,
               (unsigned long long) cpu->amd64_syscall.rdx);
        return;
    }

    // Was falling into the i386 branch below for arm64 too, printing
    // cpu->eip/eax/ebx/ecx/edx -- fields an arm64 task never touches, so the
    // "registers" in the log were meaningless leftover struct contents (seen
    // as e.g. eip=0x13b87920 eax=0x1d on a real arm64 "missing syscall" log).
    if (dispatch->abi == GUEST_ABI_ARM64) {
        printk("ERROR: %d(%s) %s %s syscall %u pc=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx\n",
               current->pid, current->comm, dispatch->name, kind, syscall_num,
               (unsigned long long) cpu->arm64_pc,
               (unsigned long long) cpu->arm64_regs[arm64_x0],
               (unsigned long long) cpu->arm64_regs[arm64_x1],
               (unsigned long long) cpu->arm64_regs[arm64_x2],
               (unsigned long long) cpu->arm64_regs[arm64_x3]);
        return;
    }
    if (dispatch->abi == GUEST_ABI_RISCV64) {
        printk("ERROR: %d(%s) %s %s syscall %u pc=0x%llx a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx\n",
               current->pid, current->comm, dispatch->name, kind, syscall_num,
               (unsigned long long) cpu->riscv64_pc,
               (unsigned long long) cpu->riscv64_regs[riscv64_a0],
               (unsigned long long) cpu->riscv64_regs[riscv64_a1],
               (unsigned long long) cpu->riscv64_regs[riscv64_a2],
               (unsigned long long) cpu->riscv64_regs[riscv64_a3]);
        return;
    }

    printk("ERROR: %d(%s) %s %s syscall %u eip=0x%x eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x\n",
           current->pid, current->comm, dispatch->name, kind, syscall_num, cpu->eip,
           cpu->eax, cpu->ebx, cpu->ecx, cpu->edx);
}

static void amd64_syscall_seed_legacy_regs(struct cpu_state *cpu) {
    cpu->amd64_syscall.rax = cpu->amd64_regs[amd64_rax];
    cpu->amd64_syscall.rdi = cpu->amd64_regs[amd64_rdi];
    cpu->amd64_syscall.rsi = cpu->amd64_regs[amd64_rsi];
    cpu->amd64_syscall.rdx = cpu->amd64_regs[amd64_rdx];
    cpu->amd64_syscall.r10 = cpu->amd64_regs[amd64_r10];
    cpu->amd64_syscall.r8 = cpu->amd64_regs[amd64_r8];
    cpu->amd64_syscall.r9 = cpu->amd64_regs[amd64_r9];
    cpu->amd64_syscall.rcx = cpu->amd64_rip;
    cpu->amd64_syscall.r11 = cpu->eflags;
}

static void handle_arm64_syscall_interrupt(struct cpu_state *cpu) {
    if (current->abi != GUEST_ABI_ARM64) {
        printk("ERROR: %d(%s) arm64 SVC in non-arm64 task at pc=%#llx\n",
               current->pid, current->comm, (unsigned long long) cpu->arm64_pc);
        struct siginfo_ info = {
            .code = ILL_ILLOPC_,
            .fault.addr = current_fault_ip(cpu),
        };
        deliver_signal(current, SIGILL_, info);
        return;
    }
    // No legacy-register-seeding equivalent to amd64_syscall_seed_legacy_regs
    // below: that exists because amd64 grew out of the i386-only register
    // file and some code paths still read the i386 mirror fields. arm64
    // never had that history — nothing reads cpu->eax/etc for an arm64 task.
    handle_syscall_interrupt(cpu);
}

static void handle_riscv64_syscall_interrupt(struct cpu_state *cpu) {
    if (current->abi != GUEST_ABI_RISCV64) {
        printk("ERROR: %d(%s) riscv64 ECALL in non-riscv64 task at pc=%#llx\n",
               current->pid, current->comm, (unsigned long long) cpu->riscv64_pc);
        struct siginfo_ info = {
            .code = ILL_ILLOPC_,
            .fault.addr = current_fault_ip(cpu),
        };
        deliver_signal(current, SIGILL_, info);
        return;
    }
    // Same no-legacy-seeding rationale as arm64 above.
    handle_syscall_interrupt(cpu);
}

static void handle_amd64_syscall_interrupt(struct cpu_state *cpu) {
    qword_t rip_before = cpu->amd64_rip;
    if (current->abi != GUEST_ABI_AMD64) {
        printk("ERROR: %d(%s) amd64 syscall instruction in non-amd64 task at 0x%x\n",
               current->pid, current->comm, cpu->eip);
        struct siginfo_ info = {
            .code = ILL_ILLOPC_,
            .fault.addr = current_fault_ip(cpu),
        };
        deliver_signal(current, SIGILL_, info);
        return;
    }

    amd64_syscall_seed_legacy_regs(cpu);
    handle_syscall_interrupt(cpu);
    if (rip_before != 0 && cpu->amd64_rip == 0) {
        printk("[amd64-jit] syscall zero-rip comm=%s pid=%d before=%#llx nr=%#llx rax=%#llx rsp=%#llx\n",
               current != NULL ? current->comm : "?",
               current != NULL ? current->pid : -1,
               (unsigned long long) rip_before,
               (unsigned long long) cpu->amd64_syscall.rax,
               (unsigned long long) cpu->amd64_regs[amd64_rax],
               (unsigned long long) cpu->amd64_regs[amd64_rsp]);
    }
}

void handle_syscall_interrupt(struct cpu_state *cpu) {
    const struct syscall_abi_dispatch *dispatch = syscall_dispatch_for_abi(current->abi);
    if (dispatch->table == NULL) {
        printk("ERROR: %d(%s) syscall dispatch for guest ABI %s is not implemented yet\n",
               current->pid, current->comm, dispatch->name);
        deliver_signal(current, SIGSYS_, SIGINFO_NIL);
        return;
    }

    // The tracer can rewrite the live register file at syscall-entry stops,
    // so decode the syscall only after the tracee is resumed.
    if (current->ptrace.traced && current->ptrace.stop_at_syscall) {
        ptrace_syscall_stop(cpu);
    }

    qword_t raw_args[6];
    dword_t args[6];
    qword_t syscall_num = dispatch->syscall_number(cpu);
    // iSH-private syscalls live above every real syscall range (ISH_SYS_AEAD =
    // 0xacc0, ISH_SYS_PIXOP = 0xacc1), so they'd fail the range check below
    // and index the table out of bounds. Intercept them here, for EVERY guest
    // ABI, before that check.
    //
    // Both accelerators are ABI-neutral: the single argument is a guest
    // pointer to a fixed-layout request struct (the two leading u32s pack the
    // 64-bit fields to the same offsets under i386's 4-byte long long
    // alignment as under any 64-bit ABI), and everything inside is an
    // explicitly-sized field. Wiring only arm64/riscv64 didn't just make the
    // accelerators unavailable to x86 guests -- it made *probing* for them
    // fatal: an unknown syscall gets SIGSYS here, not real Linux's ENOSYS, so
    // the guest-side consumers whose whole fall-back story is "probe once, and
    // on ENOSYS pass everything through" (opt/AOK/tools/pixman's LD_PRELOAD
    // shim, opt/AOK/crypto's OpenSSL provider, tests/manual/pixman_accel.c)
    // died at the probe on i386/amd64 instead of falling back.
    if (syscall_num == 0xacc0 || syscall_num == 0xacc1) {
        dispatch->syscall_args(cpu, raw_args);
        if (dispatch->abi == GUEST_ABI_ARM64 || dispatch->abi == GUEST_ABI_RISCV64) {
            handle_asm_generic_native_syscall(cpu, syscall_num, raw_args);
            return;
        }
        // i386/amd64: no legacy marshalling (raw_args[0] is a full guest
        // address), and the result goes back through the ABI's own writer,
        // which already sign-extends a negative errno the way each expects.
        dispatch->syscall_result(cpu, syscall_num == 0xacc0 ?
                sys_ish_aead_guest(raw_args[0]) : sys_ish_pixop_guest(raw_args[0]));
        return;
    }
    if (syscall_num >= dispatch->num_syscalls) {
        // ENOSYS, not SIGSYS. Linux answers an unknown syscall number with
        // -ENOSYS and nothing else -- SIGSYS is seccomp's, and a kernel does
        // not raise it on its own. Killing the caller instead breaks the
        // ordinary way a program finds out whether a syscall exists: glibc,
        // libseccomp's own tests and every "call it and see" feature probe do
        // exactly this, and here they died mid-probe with no way to catch it.
        // A caller that has genuinely gone off the rails still learns so from
        // the errno.
        //
        // Left as a printk because a guest reaching this is usually a real
        // gap worth seeing in the log, just not worth a signal.
        printk("ERROR: %d(%s) missing %s syscall %d\n",
               current->pid, current->comm, dispatch->name, (int) syscall_num);
        dispatch->syscall_result(cpu, _ENOSYS);
        return;
    }

    syscall_t syscall = dispatch->table[syscall_num];
    if (syscall == NULL) {
        log_stub_syscall(cpu, dispatch, (unsigned) syscall_num, "missing");
        dispatch->syscall_result(cpu, syscall_stub());
        return;
    }
    if (syscall_is_logged_stub(syscall))
        log_stub_syscall(cpu, dispatch, (unsigned) syscall_num, "stub");

    dispatch->syscall_args(cpu, raw_args);
    amd64_tty2_shell_syscall_trace_enter(syscall_num, raw_args);
    amd64_tracked_proc_trace_enter(syscall_num, raw_args);
    if (dispatch->abi == GUEST_ABI_AMD64 && syscall_num == 15) {
        qword_t result = sys_rt_sigreturn_amd64();
        sqword_t signed_result = (sqword_t) result;
        if (signed_result < 0 && signed_result >= -4095)
            amd64_syscall_result_qword(cpu, result);
        amd64_trace_track_child(syscall_num, result);
        amd64_tty_process_trace(syscall_num, raw_args, result);
        amd64_tracked_enoent_path_trace(syscall_num, raw_args, result);
        amd64_tracked_einval_trace(syscall_num, raw_args, result);
        amd64_tracked_errno_trace(syscall_num, raw_args, result);
        dpkg_overflow_syscall_trace(syscall_num, raw_args, result);
        amd64_tracked_sigabrt_trace(syscall_num, raw_args, result);
        amd64_tracked_write_trace(syscall_num, raw_args, result);
        amd64_enomem_syscall_trace(syscall_num, raw_args, result);
        amd64_tracked_proc_trace_exit(syscall_num, raw_args, result);
        amd64_tty2_shell_syscall_trace_exit(syscall_num, result);
        if (current->ptrace.traced && current->ptrace.stop_at_syscall &&
                current->ptrace.syscall_stopped)
            ptrace_syscall_stop(cpu);
        return;
    }
    if (dispatch->abi == GUEST_ABI_AMD64 &&
            handle_amd64_native_memory_syscall(cpu, syscall_num, raw_args)) {
        qword_t result = dispatch->abi == GUEST_ABI_AMD64 ? cpu->amd64_regs[amd64_rax] : cpu->eax;
        // A restart code must never survive as the syscall's answer. Of the
        // ~220 cases in the handler above, only a handful run the result
        // through syscall_result_should_restart themselves; every other one
        // writes it straight to RAX, so an _ERESTART came back to the guest as
        // errno 85 and an _ERESTART_NOHAND as errno 512. Measured: on an amd64
        // guest, ^Z a program writing to a pipe and its write(2) fails with
        // "errno 85 (No error information)" -- because realfs_write returns
        // EINTR the moment a signal is pending, sys_write_common turns that
        // into _ERESTART meaning "restart transparently", and nothing ever
        // did. i386/arm64/riscv64 route the same calls through the legacy
        // path, which has always checked, which is why this was amd64-only.
        //
        // Catching it here rather than in each case: one place cannot be
        // forgotten by the next syscall someone adds to that switch. The
        // cases that DO handle it already leave a normal value in RAX, so
        // they fall straight through.
        //
        // Compared at FULL WIDTH, not truncated to 32 bits. A 64-bit return --
        // an mmap address, a large lseek offset -- can perfectly well have low
        // bits of 0xffffffab, and truncating would turn one of those into a
        // spurious restart.
        // Only ever SET the flag here, never clear it. The cases that handle
        // their own restart call syscall_result_should_restart inside the
        // handler -- which sets it -- and then leave a normal value in RAX;
        // clearing it on the way past destroyed it before the handler-setup
        // path could read it, turning every handler-cancelled ERESTARTNOHAND
        // back into a restart. That regressed all five of the poll-family
        // cases this backstop was not even meant to touch.
        bool restart_pending =
            (sqword_t) result == (sqword_t) (sdword_t) _ERESTART ||
            (sqword_t) result == (sqword_t) (sdword_t) _ERESTART_NOHAND;
        if (restart_pending) {
            if (current != NULL)
                current->restart_nohand_pending =
                    (sqword_t) result == (sqword_t) (sdword_t) _ERESTART_NOHAND;
            prepare_syscall_restart(cpu, &amd64_syscall_dispatch, syscall_num, raw_args[0]);
            if (current->ptrace.traced && current->ptrace.stop_at_syscall &&
                    current->ptrace.syscall_stopped)
                ptrace_syscall_stop(cpu);
            return;
        }
        amd64_trace_track_child(syscall_num, result);
        amd64_tty_process_trace(syscall_num, raw_args, result);
        amd64_tracked_enoent_path_trace(syscall_num, raw_args, result);
        amd64_tracked_einval_trace(syscall_num, raw_args, result);
        amd64_tracked_errno_trace(syscall_num, raw_args, result);
        dpkg_overflow_syscall_trace(syscall_num, raw_args, result);
        amd64_tracked_sigabrt_trace(syscall_num, raw_args, result);
        amd64_tracked_write_trace(syscall_num, raw_args, result);
        amd64_enomem_syscall_trace(syscall_num, raw_args, result);
        amd64_tracked_proc_trace_exit(syscall_num, raw_args, result);
        amd64_tty2_shell_syscall_trace_exit(syscall_num, result);
        if (current->ptrace.traced && current->ptrace.stop_at_syscall &&
                current->ptrace.syscall_stopped)
            ptrace_syscall_stop(cpu);
        return;
    }
    if ((dispatch->abi == GUEST_ABI_ARM64 || dispatch->abi == GUEST_ABI_RISCV64) &&
            handle_asm_generic_native_syscall(cpu, syscall_num, raw_args)) {
        if (current->ptrace.traced && current->ptrace.stop_at_syscall &&
                current->ptrace.syscall_stopped)
            ptrace_syscall_stop(cpu);
        return;
    }
    if (!marshal_syscall_args_legacy(dispatch->abi, syscall_num, raw_args, args)) {
        printk("ERROR: %d(%s) %s syscall %llu needs full-width args before it can be emulated "
               "(args %#llx %#llx %#llx %#llx %#llx %#llx)\n",
               current->pid, current->comm, dispatch->name, syscall_num,
               (unsigned long long) raw_args[0], (unsigned long long) raw_args[1],
               (unsigned long long) raw_args[2], (unsigned long long) raw_args[3],
               (unsigned long long) raw_args[4], (unsigned long long) raw_args[5]);
        deliver_signal(current, SIGSYS_, SIGINFO_NIL);
        return;
    }

    STRACE("%d(%s) %d:%d %s call %-3llu ", current->pid, current->comm,
           current->reference.count,
           __atomic_load_n(&current->locks_held.count, __ATOMIC_RELAXED),
           dispatch->name, syscall_num);
    dword_t result = syscall(args[0], args[1], args[2], args[3], args[4], args[5]);
    qword_t trace_result = syscall_result_is_errno(result) ?
        (qword_t) (sqword_t) syscall_result_errno(result) : (qword_t) result;
    amd64_trace_track_child(syscall_num, trace_result);
    amd64_tty_process_trace(syscall_num, raw_args, trace_result);
    amd64_tracked_enoent_path_trace(syscall_num, raw_args, trace_result);
    amd64_tracked_einval_trace(syscall_num, raw_args, trace_result);
    amd64_tracked_errno_trace(syscall_num, raw_args, trace_result);
    dpkg_overflow_syscall_trace(syscall_num, raw_args, trace_result);
    amd64_tracked_sigabrt_trace(syscall_num, raw_args, trace_result);
    amd64_tracked_write_trace(syscall_num, raw_args, trace_result);
    amd64_enomem_syscall_trace(syscall_num, raw_args, trace_result);
    amd64_tracked_proc_trace_exit(syscall_num, raw_args, trace_result);
    amd64_tty2_shell_syscall_trace_exit(syscall_num, trace_result);
    if (syscall_result_should_restart(result)) {
        if (current->ptrace.traced && current->ptrace.stop_at_syscall) {
            dispatch->syscall_result(cpu, result);
            if (current->ptrace.syscall_stopped)
                ptrace_syscall_stop(cpu);
        }
        prepare_syscall_restart(cpu, dispatch, syscall_num, raw_args[0]);
        STRACE(" = restart\n");
        return;
    }
    dispatch->syscall_result(cpu, result);
    if (current->ptrace.traced && current->ptrace.stop_at_syscall &&
            current->ptrace.syscall_stopped)
        ptrace_syscall_stop(cpu);
    STRACE(" = 0x%x\n", result);
}

static void dump_opcode_window(guest_addr_t ip);
static void dump_addr_backing(const char *label, guest_addr_t addr);
static void dump_amd64_regs(const struct cpu_state *cpu);
static void dump_amd64_hlt_tls_state(const struct cpu_state *cpu);
static bool amd64_hlt_is_zero_sentinel_trap(guest_addr_t ip);
static void dump_amd64_hlt_zero_sentinel_state(const struct cpu_state *cpu);
static void dump_amd64_loader_state(const struct cpu_state *cpu);
static void dump_amd64_store_trace(const struct cpu_state *cpu);
static void dump_fault_pt_state(guest_addr_t addr);
static bool amd64_verbose_fault_trace_enabled(void);
static bool handle_i386_read_fault_gpf(struct cpu_state *cpu);
static bool handle_i386_write_fault_gpf(struct cpu_state *cpu);
static bool handle_i386_call_stack_gpf(struct cpu_state *cpu);
static bool handle_i386_stack_store_gpf(struct cpu_state *cpu);

void dump_mem(guest_addr_t start, uint_t len);

// ISH_ARM64_FAULT_MEMDUMP="1,3" dumps guest memory around x1 and x3 on an
// arm64 page fault (0x40 before to 0x140 after each register value).
static void dump_arm64_fault_memdump(const struct cpu_state *cpu) {
    const char *spec = getenv("ISH_ARM64_FAULT_MEMDUMP");
    if (spec == NULL)
        return;
    char buf[64];
    strlcpy(buf, spec, sizeof(buf));
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
        int r = atoi(tok);
        if (r < 0 || r > 30)
            continue;
        guest_addr_t v = cpu->arm64_regs[r] & ~(guest_addr_t) 7;
        printk("memdump around x%d=%#llx:\n", r, (unsigned long long) cpu->arm64_regs[r]);
        dump_mem(v - 0x40, 0x180);
    }
}

static void record_guest_fault_event(const char *kind, const struct cpu_state *cpu,
                                     guest_addr_t fault_addr, bool is_write) {
    char summary[512];
    char detail[1024];
    char opcode[128];
    size_t opcode_pos = 0;
    guest_addr_t ip = current_fault_ip(cpu);

    snprintf(summary, sizeof(summary),
             "pid=%d comm=%s abi=%s ip=%#llx fault_addr=%#llx access=%s",
             current != NULL ? current->pid : -1,
             current != NULL ? current->comm : "?",
             (current != NULL && current->abi == GUEST_ABI_AMD64) ? "amd64" : "i386",
             (unsigned long long) ip,
             (unsigned long long) fault_addr,
             is_write ? "write" : "read");

    for (int i = -4; i < 12 && opcode_pos + 4 < sizeof(opcode); i++) {
        uint8_t byte = 0;
        guest_addr_t addr = ip + i;
        if (i == 0 && opcode_pos + 1 < sizeof(opcode))
            opcode[opcode_pos++] = '[';
        if (user_get(addr, byte)) {
            opcode_pos += snprintf(opcode + opcode_pos, sizeof(opcode) - opcode_pos, "??");
        } else {
            opcode_pos += snprintf(opcode + opcode_pos, sizeof(opcode) - opcode_pos, "%02x", byte);
        }
        if (i == 0 && opcode_pos + 1 < sizeof(opcode))
            opcode[opcode_pos++] = ']';
        if (i != 11 && opcode_pos + 1 < sizeof(opcode))
            opcode[opcode_pos++] = ' ';
    }
    opcode[opcode_pos < sizeof(opcode) ? opcode_pos : sizeof(opcode) - 1] = '\0';

    if (current != NULL && current->abi == GUEST_ABI_AMD64) {
        snprintf(detail, sizeof(detail),
                 "opcode window: %s\n"
                 "rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx\n"
                 "rsi=%#llx rdi=%#llx rbp=%#llx rsp=%#llx rip=%#llx",
                 opcode,
                 (unsigned long long) cpu->amd64_regs[amd64_rax],
                 (unsigned long long) cpu->amd64_regs[amd64_rbx],
                 (unsigned long long) cpu->amd64_regs[amd64_rcx],
                 (unsigned long long) cpu->amd64_regs[amd64_rdx],
                 (unsigned long long) cpu->amd64_regs[amd64_rsi],
                 (unsigned long long) cpu->amd64_regs[amd64_rdi],
                 (unsigned long long) cpu->amd64_regs[amd64_rbp],
                 (unsigned long long) cpu->amd64_regs[amd64_rsp],
                 (unsigned long long) cpu->amd64_rip);
    } else {
        snprintf(detail, sizeof(detail),
                 "opcode window: %s\n"
                 "eax=%#x ebx=%#x ecx=%#x edx=%#x\n"
                 "esi=%#x edi=%#x ebp=%#x esp=%#x eip=%#x",
                 opcode,
                 cpu->eax, cpu->ebx, cpu->ecx, cpu->edx,
                 cpu->esi, cpu->edi, cpu->ebp, cpu->esp, cpu->eip);
    }

    ISHDiagnosticsRecordGuestFatalSync(kind, summary, detail);
}

static guest_addr_t current_fault_ip(const struct cpu_state *cpu) {
    if (current->abi == GUEST_ABI_AMD64)
        return cpu->amd64_rip;
    if (current->abi == GUEST_ABI_ARM64)
        return cpu->arm64_pc;
    if (current->abi == GUEST_ABI_RISCV64)
        return cpu->riscv64_pc;
    return cpu->eip;
}

void handle_page_fault_interrupt(struct cpu_state *cpu) {
    void *ptr = mem_ptr_fault(current->mem, cpu->segfault_addr,
                              cpu->segfault_was_write ? MEM_WRITE : MEM_READ);

    if (ptr == NULL) {
        printk("ERROR: %d(%s) [%s] page fault on %#llx at %#llx%s\n",
               current->pid, current->comm, guest_abi_desc(current->abi).name,
               (unsigned long long) cpu->segfault_addr,
               (unsigned long long) current_fault_ip(cpu),
               cpu->segfault_was_write ? " (write)" : " (read)");
        if (current->abi == GUEST_ABI_ARM64) {
            for (int i = 0; i < 31; i += 4)
                printk("  x%d=%#llx x%d=%#llx x%d=%#llx x%d=%#llx\n",
                       i, (unsigned long long) cpu->arm64_regs[i],
                       i + 1, i + 1 < 31 ? (unsigned long long) cpu->arm64_regs[i + 1] : 0,
                       i + 2, i + 2 < 31 ? (unsigned long long) cpu->arm64_regs[i + 2] : 0,
                       i + 3, i + 3 < 31 ? (unsigned long long) cpu->arm64_regs[i + 3] : 0);
            printk("  sp=%#llx pc=%#llx tpidr=%#llx\n",
                   (unsigned long long) cpu->arm64_sp,
                   (unsigned long long) cpu->arm64_pc,
                   (unsigned long long) cpu->arm64_tpidr);
            dump_addr_backing("  pc-backing", current_fault_ip(cpu));
            dump_arm64_fault_memdump(cpu);
            arm64_watch_dump();
        }
        if (current->abi == GUEST_ABI_RISCV64) {
            for (int i = 0; i < 32; i += 4)
                printk("  x%d=%#llx x%d=%#llx x%d=%#llx x%d=%#llx\n",
                       i, (unsigned long long) cpu->riscv64_regs[i],
                       i + 1, (unsigned long long) cpu->riscv64_regs[i + 1],
                       i + 2, (unsigned long long) cpu->riscv64_regs[i + 2],
                       i + 3, (unsigned long long) cpu->riscv64_regs[i + 3]);
            printk("  pc=%#llx\n", (unsigned long long) cpu->riscv64_pc);
        }
        dump_opcode_window(current_fault_ip(cpu));
        if (current->abi == GUEST_ABI_AMD64) {
            dump_amd64_regs(cpu);
            dump_fault_pt_state(cpu->segfault_addr);
            dump_amd64_cc1_trace(cpu);
            dump_amd64_cc1_jit_trace(cpu);
            if (amd64_verbose_fault_trace_enabled()) {
                dump_amd64_loader_state(cpu);
                dump_amd64_store_trace(cpu);
            }
        }
        record_guest_fault_event("page-fault", cpu, cpu->segfault_addr, cpu->segfault_was_write);
        struct siginfo_ info = {
            .code = mem_segv_reason(current->mem, cpu->segfault_addr),
            .fault.addr = cpu->segfault_addr,
        };
        dump_stack(8);
        deliver_signal(current, SIGSEGV_, info);
    }
}

static void dump_fault_pt_state(guest_addr_t addr) {
    enum { PT_FAULT_LOG_BUDGET = 8 };
    static unsigned fault_pt_log_count;
    if (fault_pt_log_count >= PT_FAULT_LOG_BUDGET)
        return;
    fault_pt_log_count++;

    read_lock(&current->mem->lock);
    page_t center = PAGE(addr);
    page_t start = center > 1 ? center - 1 : center;
    page_t end = center + 1;
    printk("amd64 fault pt: floor=%#llx ceiling=%#llx page=%#llx\n",
           (unsigned long long) ((guest_addr_t) current->mem->mmap_floor << PAGE_BITS),
           (unsigned long long) ((guest_addr_t) current->mem->mmap_ceiling << PAGE_BITS),
           (unsigned long long) center);
    for (page_t page = start; page <= end; page++) {
        struct pt_entry *pt = mem_pt(current->mem, page);
        if (pt == NULL) {
            printk("amd64 fault pt: %#llx unmapped\n",
                   (unsigned long long) ((guest_addr_t) page << PAGE_BITS));
            continue;
        }
        const char *name = NULL;
        if (pt->data != NULL)
            name = pt->data->name;
        printk("amd64 fault pt: %#llx flags=%#x anon=%d cow=%d shared=%d off=%#zx name=%s\n",
               (unsigned long long) ((guest_addr_t) page << PAGE_BITS),
               pt->flags,
               !!(pt->flags & P_ANONYMOUS),
               !!(pt->flags & P_COW),
               !!(pt->flags & P_SHARED),
               pt->offset,
               name != NULL ? name : "-");
    }
    read_unlock(&current->mem->lock);
}

static void handle_general_protection_interrupt(struct cpu_state *cpu) {
    if (handle_i386_read_fault_gpf(cpu))
        return;
    if (handle_i386_write_fault_gpf(cpu))
        return;
    if (handle_i386_call_stack_gpf(cpu))
        return;
    if (handle_i386_stack_store_gpf(cpu))
        return;

    guest_addr_t ip = current_fault_ip(cpu);
    uint8_t first_opcode = 0;
    bool have_first_opcode = user_get(ip, first_opcode) == 0;
    printk("ERROR: %d(%s) [%s] general protection fault at %#llx: ",
           current->pid, current->comm, guest_abi_desc(current->abi).name, (unsigned long long) ip);
    for (int i = 0; i < 8; i++) {
        uint8_t b;
        if (user_get(ip + i, b))
            break;
        printk("%02x ", b);
    }
    printk("\n");
    dump_opcode_window(ip);
    if (current->abi == GUEST_ABI_AMD64) {
        dump_amd64_regs(cpu);
        dump_amd64_cc1_trace(cpu);
        dump_amd64_cc1_jit_trace(cpu);
        if (have_first_opcode && first_opcode == 0xf4) {
            dump_amd64_hlt_tls_state(cpu);
            if (amd64_hlt_is_zero_sentinel_trap(ip))
                dump_amd64_hlt_zero_sentinel_state(cpu);
        }
        if (amd64_verbose_fault_trace_enabled()) {
            dump_amd64_loader_state(cpu);
            dump_amd64_store_trace(cpu);
        }
    }
    record_guest_fault_event("general-protection-fault", cpu, cpu->segfault_addr, cpu->segfault_was_write);
    dump_stack(8);
    cpu->trapno = INT_GPF;
    cpu->segfault_addr = 0;
    cpu->segfault_was_write = false;
    struct siginfo_ info = {
        .code = SI_KERNEL_,
        .fault.addr = current_fault_ip(cpu),
    };
    deliver_signal(current, SIGSEGV_, info);
}

static bool i386_gpf_fetch_u8(addr_t *ip, byte_t *value) {
    if (user_get(*ip, *value))
        return false;
    (*ip)++;
    return true;
}

static bool i386_gpf_fetch_i8(addr_t *ip, int8_t *value) {
    if (user_get(*ip, *value))
        return false;
    (*ip)++;
    return true;
}

static bool i386_gpf_fetch_i32(addr_t *ip, int32_t *value) {
    if (user_get(*ip, *value))
        return false;
    (*ip) += sizeof(*value);
    return true;
}

static bool i386_gpf_decode_ea32(struct cpu_state *cpu, addr_t *ip, byte_t modrm,
                                 guest_addr_t segment_base, guest_addr_t *addr) {
    unsigned mod = modrm >> 6;
    unsigned rm = modrm & 7;
    if (mod == 3)
        return false;

    dword_t effective = segment_base;
    if (rm == 4) {
        byte_t sib;
        if (!i386_gpf_fetch_u8(ip, &sib))
            return false;
        unsigned scale = sib >> 6;
        unsigned index = (sib >> 3) & 7;
        unsigned base = sib & 7;

        if (!(mod == 0 && base == 5))
            effective += cpu->regs[base];
        if (index != 4)
            effective += cpu->regs[index] << scale;
        if (mod == 0 && base == 5) {
            int32_t disp32;
            if (!i386_gpf_fetch_i32(ip, &disp32))
                return false;
            effective += disp32;
        } else if (mod == 1) {
            int8_t disp8;
            if (!i386_gpf_fetch_i8(ip, &disp8))
                return false;
            effective += disp8;
        } else if (mod == 2) {
            int32_t disp32;
            if (!i386_gpf_fetch_i32(ip, &disp32))
                return false;
            effective += disp32;
        }
    } else {
        if (!(mod == 0 && rm == 5))
            effective += cpu->regs[rm];
        if (mod == 0 && rm == 5) {
            int32_t disp32;
            if (!i386_gpf_fetch_i32(ip, &disp32))
                return false;
            effective += disp32;
        } else if (mod == 1) {
            int8_t disp8;
            if (!i386_gpf_fetch_i8(ip, &disp8))
                return false;
            effective += disp8;
        } else if (mod == 2) {
            int32_t disp32;
            if (!i386_gpf_fetch_i32(ip, &disp32))
                return false;
            effective += disp32;
        }
    }

    *addr = effective;
    return true;
}

static bool i386_gpf_decode_prefix_and_opcode(struct cpu_state *cpu, addr_t *ip, byte_t *opcode,
                                              guest_addr_t *segment_base) {
    *ip = cpu->eip;
    *segment_base = 0;

    for (;;) {
        byte_t prefix;
        if (!i386_gpf_fetch_u8(ip, &prefix))
            return false;
        switch (prefix) {
            case 0x64:
            case 0x65:
                *segment_base = cpu->tls_ptr;
                break;
            case 0x66:
            case 0x67:
            case 0xf0:
            case 0xf2:
            case 0xf3:
                break;
            default:
                *opcode = prefix;
                return true;
        }
    }
}

static bool i386_gpf_decode_write_addr(struct cpu_state *cpu, guest_addr_t *addr) {
    addr_t ip;
    byte_t opcode;
    byte_t modrm;
    guest_addr_t segment_base;
    if (!i386_gpf_decode_prefix_and_opcode(cpu, &ip, &opcode, &segment_base) ||
            !i386_gpf_fetch_u8(&ip, &modrm))
        return false;

    unsigned reg = (modrm >> 3) & 7;
    switch (opcode) {
        case 0x89:
            break;
        case 0xc7:
            if (reg != 0)
                return false;
            break;
        case 0x83:
            if (reg == 7)
                return false;
            break;
        default:
            return false;
    }

    return i386_gpf_decode_ea32(cpu, &ip, modrm, segment_base, addr);
}

static bool i386_gpf_decode_read_addr(struct cpu_state *cpu, guest_addr_t *addr) {
    addr_t ip;
    byte_t opcode;
    byte_t modrm;
    guest_addr_t segment_base;
    if (!i386_gpf_decode_prefix_and_opcode(cpu, &ip, &opcode, &segment_base) ||
            !i386_gpf_fetch_u8(&ip, &modrm))
        return false;

    unsigned reg = (modrm >> 3) & 7;
    switch (opcode) {
        case 0xff:
            if (reg != 2 && reg != 4)
                return false;
            break;
        default:
            return false;
    }

    return i386_gpf_decode_ea32(cpu, &ip, modrm, segment_base, addr);
}

static bool i386_gpf_addr_needs_page_fault(guest_addr_t fault_addr, int type) {
    read_lock(&current->mem->lock);
    struct pt_entry *entry = mem_pt(current->mem, PAGE(fault_addr));
    bool needs_page_fault = entry == NULL || entry->data == NULL || entry->data->data == NULL ||
            (type == MEM_WRITE && !P_WRITABLE(entry->flags));
    read_unlock(&current->mem->lock);
    return needs_page_fault;
}

static bool handle_i386_read_fault_gpf(struct cpu_state *cpu) {
    if (current->abi == GUEST_ABI_AMD64)
        return false;

    guest_addr_t fault_addr = 0;
    if (cpu->segfault_addr != 0 && !cpu->segfault_was_write) {
        // Read gadget explicitly reported the fault address: route it through
        // the page-fault path unconditionally (same reasoning as the write
        // helper -- a lock-free re-check races a sibling thread resolving the
        // page and would fall through to a spurious SIGSEGV). mem_ptr_fault
        // is a no-op if the page is already readable; the load re-executes.
        cpu->trapno = INT_PF;
        cpu->segfault_was_write = false;
        handle_page_fault_interrupt(cpu);
        return true;
    }
    if (!i386_gpf_decode_read_addr(cpu, &fault_addr))
        return false;
    if (!i386_gpf_addr_needs_page_fault(fault_addr, MEM_READ))
        return false;

    cpu->trapno = INT_PF;
    cpu->segfault_addr = fault_addr;
    cpu->segfault_was_write = false;
    handle_page_fault_interrupt(cpu);
    return true;
}

static bool handle_i386_write_fault_gpf(struct cpu_state *cpu) {
    if (current->abi == GUEST_ABI_AMD64)
        return false;

    guest_addr_t fault_addr = 0;
    if (cpu->segfault_was_write && cpu->segfault_addr != 0) {
        // The write gadget explicitly reported a translation failure at this
        // exact address (segfault_write in gadgets-*/memory.S set segfault_addr
        // + segfault_was_write before raising INT_GPF). Route it straight into
        // the page-fault path unconditionally: mem_ptr_fault resolves a COW
        // break / mapping under the write lock, or -- for a genuinely
        // unwritable page -- delivers a real SIGSEGV. Do NOT gate on the
        // lock-free i386_gpf_addr_needs_page_fault re-check here: after a
        // fork() the parent's shared pages are P_COW, and a sibling thread can
        // break the COW between the gadget's fault and this handler, making the
        // page read back as writable. Gating then falls through to a spurious
        // SIGSEGV instead of just restarting the (idempotent) faulting store --
        // exactly the signalfd_epoll_deadlock crash on a LOCK-prefixed atomic
        // (3 forker threads racing sibling waiters, issue #477). handle_page_
        // fault_interrupt is a no-op when the page is already writable, so the
        // instruction simply re-executes.
        cpu->trapno = INT_PF;
        cpu->segfault_was_write = true;
        handle_page_fault_interrupt(cpu);
        return true;
    }

    if (!i386_gpf_decode_write_addr(cpu, &fault_addr))
        return false;
    if (!i386_gpf_addr_needs_page_fault(fault_addr, MEM_WRITE))
        return false;
    cpu->trapno = INT_PF;
    cpu->segfault_addr = fault_addr;
    cpu->segfault_was_write = true;
    handle_page_fault_interrupt(cpu);
    return true;
}

static bool handle_i386_call_stack_gpf(struct cpu_state *cpu) {
    if (current->abi == GUEST_ABI_AMD64)
        return false;

    addr_t ip;
    byte_t opcode;
    byte_t modrm = 0;
    guest_addr_t segment_base;
    if (!i386_gpf_decode_prefix_and_opcode(cpu, &ip, &opcode, &segment_base))
        return false;

    bool is_call = false;
    switch (opcode) {
        case 0xe8:
            is_call = true;
            break;
        case 0xff:
            if (!i386_gpf_fetch_u8(&ip, &modrm))
                return false;
            is_call = ((modrm >> 3) & 7) == 2;
            break;
        default:
            return false;
    }
    if (!is_call)
        return false;

    guest_addr_t push_addr = cpu->esp - 4;
    if (!i386_gpf_addr_needs_page_fault(push_addr, MEM_WRITE))
        return false;
    cpu->trapno = INT_PF;
    cpu->segfault_addr = push_addr;
    cpu->segfault_was_write = true;
    handle_page_fault_interrupt(cpu);
    return true;
}

static bool handle_i386_stack_store_gpf(struct cpu_state *cpu) {
    if (current->abi == GUEST_ABI_AMD64)
        return false;

    byte_t opcode;
    byte_t modrm;
    byte_t sib;
    if (user_get(cpu->eip, opcode) || user_get(cpu->eip + 1, modrm) || user_get(cpu->eip + 2, sib))
        return false;

    // Detect `mov [esp], r32` style faults. If the stack page itself is the
    // real problem, report it as a write fault on ESP instead of a generic GPF
    // at EIP.
    if (opcode != 0x89 || (modrm & 0xc7) != 0x04 || sib != 0x24)
        return false;

    read_lock(&current->mem->lock);
    void *ptr = mem_ptr(current->mem, cpu->esp, MEM_WRITE);
    int segv_code = ptr == NULL ? mem_segv_reason(current->mem, cpu->esp) : 0;
    read_unlock(&current->mem->lock);

    if (ptr != NULL)
        return false;

    cpu->trapno = INT_PF;
    cpu->segfault_addr = cpu->esp;
    cpu->segfault_was_write = true;
    struct siginfo_ info = {
        .code = segv_code,
        .fault.addr = cpu->esp,
    };
    deliver_signal(current, SIGSEGV_, info);
    return true;
}

static int amd64_nop_instruction_len(guest_addr_t ip) {
    byte_t bytes[16];
    for (size_t i = 0; i < sizeof(bytes); i++) {
        if (user_get(ip + i, bytes[i]))
            return 0;
    }

    int i = 0;
    while (i < (int) sizeof(bytes) && (bytes[i] == 0x66 || bytes[i] == 0x2e || bytes[i] == 0x3e))
        i++;
    if (i >= (int) sizeof(bytes))
        return 0;

    if (bytes[i] == 0x90)
        return i + 1;

    if (i + 2 >= (int) sizeof(bytes) || bytes[i] != 0x0f || bytes[i + 1] != 0x1f)
        return 0;
    i += 2;

    byte_t modrm = bytes[i++];
    if (((modrm >> 3) & 7) != 0)
        return 0;

    int mod = modrm >> 6;
    int rm = modrm & 7;
    bool has_sib = mod != 3 && rm == 4;
    int disp_len = 0;

    if (has_sib) {
        byte_t sib = bytes[i++];
        int base = sib & 7;
        if (mod == 0 && base == 5)
            disp_len = 4;
    } else if (mod == 0 && rm == 5) {
        disp_len = 4;
    }

    if (mod == 1)
        disp_len = 1;
    else if (mod == 2)
        disp_len = 4;

    if (i + disp_len > (int) sizeof(bytes))
        return 0;
    return i + disp_len;
}

static bool amd64_verbose_fault_trace_enabled(void) {
    return false;
}

// Name the file/region backing a guest address, for fault diagnosis: tells
// whether a faulting PC lives in a mapped file (libc/node .text) vs an
// anonymous region (V8's own JIT-generated code), and the file offset so the
// exact instruction can be disassembled from the on-disk binary.
static void dump_addr_backing(const char *label, guest_addr_t addr) {
    struct mem *mem = current != NULL ? current->mem : NULL;
    if (mem == NULL)
        return;
    struct pt_entry *pt = mem_pt(mem, PAGE(addr));
    if (pt == NULL || pt->data == NULL) {
        printk("%s %#llx: unmapped\n", label, (unsigned long long) addr);
        return;
    }
    struct data *data = pt->data;
    char path[MAX_PATH] = "";
    if (data->name != NULL)
        strncpy(path, data->name, sizeof(path) - 1);
    else if (data->fd != NULL)
        generic_getpath(data->fd, path);
    // file_offset is the region's offset at its first page; add the address's
    // distance into this page's mapping via pt->offset (offset within data).
    printk("%s %#llx: %s%s flags=%#x data_off=%zu file_off=%zu\n",
           label, (unsigned long long) addr,
           path[0] ? path : "[anon]",
           (data->fd == NULL && data->name == NULL) ? " (anonymous/JIT)" : "",
           pt->flags, (size_t) pt->offset, (size_t) data->file_offset);
}

static void dump_opcode_window(guest_addr_t ip) {
    const int before = 4;
    const int after = 12;

    printk("opcode window around %#llx: ", (unsigned long long) ip);
    for (int i = -before; i < after; i++) {
        uint8_t b;
        guest_addr_t addr = ip + i;
        if (i == 0)
            printk("[");
        if (user_get(addr, b))
            printk("??");
        else
            printk("%02x", b);
        if (i == 0)
            printk("]");
        else
            printk(" ");
    }
    printk("\n");
}

static void dump_guest_byte_window(const char *label, guest_addr_t addr) {
    const int before = 8;
    const int after = 8;

    printk("%s around %#llx: ", label, (unsigned long long) addr);
    for (int i = -before; i < after; i++) {
        uint8_t b;
        guest_addr_t byte_addr = addr + i;
        if (i == 0)
            printk("[");
        if (user_get(byte_addr, b))
            printk("??");
        else
            printk("%02x", b);
        if (i == 0)
            printk("]");
        else
            printk(" ");
    }
    printk("\n");
}

static bool amd64_hlt_is_zero_sentinel_trap(guest_addr_t ip) {
    static const uint8_t pattern[] = {0xf4, 0x80, 0x3f, 0x00, 0x74, 0x01, 0xf4};
    uint8_t bytes[sizeof(pattern)];

    for (size_t i = 0; i < sizeof(bytes); i++) {
        if (user_get(ip + i, bytes[i]))
            return false;
    }
    return memcmp(bytes, pattern, sizeof(pattern)) == 0;
}

static void dump_amd64_regs(const struct cpu_state *cpu) {
    printk("amd64 regs: rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx\n",
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbx],
           (unsigned long long) cpu->amd64_regs[amd64_rcx],
           (unsigned long long) cpu->amd64_regs[amd64_rdx],
           (unsigned long long) cpu->amd64_regs[amd64_rsi],
           (unsigned long long) cpu->amd64_regs[amd64_rdi]);
    printk("amd64 regs: rbp=%#llx rsp=%#llx r8=%#llx r9=%#llx r10=%#llx r11=%#llx\n",
           (unsigned long long) cpu->amd64_regs[amd64_rbp],
           (unsigned long long) cpu->amd64_regs[amd64_rsp],
           (unsigned long long) cpu->amd64_regs[amd64_r8],
           (unsigned long long) cpu->amd64_regs[amd64_r9],
           (unsigned long long) cpu->amd64_regs[amd64_r10],
           (unsigned long long) cpu->amd64_regs[amd64_r11]);
    printk("amd64 regs: r12=%#llx r13=%#llx r14=%#llx r15=%#llx rip=%#llx\n",
           (unsigned long long) cpu->amd64_regs[amd64_r12],
           (unsigned long long) cpu->amd64_regs[amd64_r13],
           (unsigned long long) cpu->amd64_regs[amd64_r14],
           (unsigned long long) cpu->amd64_regs[amd64_r15],
           (unsigned long long) cpu->amd64_rip);
    printk("amd64 flags: eflags=%#x zf=%u cf=%u of=%u sf=%u pf=%u df=%u af_ops=%u\n",
           cpu->eflags,
           cpu->zf ? 1u : 0u,
           cpu->cf ? 1u : 0u,
           cpu->of ? 1u : 0u,
           cpu->sf ? 1u : 0u,
           cpu->pf ? 1u : 0u,
           cpu->df ? 1u : 0u,
           cpu->af_ops);
}

static void dump_amd64_hlt_tls_state(const struct cpu_state *cpu) {
    qword_t fs_base = cpu->tls_ptr;
    qword_t fs_self = 0;
    qword_t fs_dtv = 0;
    int self_fault = fs_base == 0 ? 1 : user_get((addr_t) fs_base, fs_self);
    int dtv_fault = fs_base == 0 ? 1 : user_get((addr_t) (fs_base + 8), fs_dtv);

    printk("amd64 hlt tls: fsbase=%#llx rax=%#llx rbp=%#llx fs:[0]=",
           (unsigned long long) fs_base,
           (unsigned long long) cpu->amd64_regs[amd64_rax],
           (unsigned long long) cpu->amd64_regs[amd64_rbp]);
    if (self_fault)
        printk("??");
    else
        printk("%#llx", (unsigned long long) fs_self);
    printk(" fs:[8]=");
    if (dtv_fault)
        printk("??");
    else
        printk("%#llx", (unsigned long long) fs_dtv);
    printk("\n");
}

static void dump_amd64_hlt_zero_sentinel_state(const struct cpu_state *cpu) {
    guest_addr_t r8 = cpu->amd64_regs[amd64_r8];
    guest_addr_t rdi = cpu->amd64_regs[amd64_rdi];
    uint8_t r8_byte = 0;
    uint8_t rdi_byte = 0;
    int r8_fault = user_get(r8, r8_byte);
    int rdi_fault = user_get(rdi, rdi_byte);

    printk("amd64 hlt zero-sentinel: r8=%#llx byte=",
           (unsigned long long) r8);
    if (r8_fault)
        printk("??");
    else
        printk("%02x", r8_byte);
    printk(" rdi=%#llx byte=",
           (unsigned long long) rdi);
    if (rdi_fault)
        printk("??");
    else
        printk("%02x", rdi_byte);
    printk("\n");

    dump_guest_byte_window("amd64 hlt r8 window", r8);
    dump_guest_byte_window("amd64 hlt rdi window", rdi);
}

static void dump_amd64_loader_state(const struct cpu_state *cpu) {
    qword_t dso = cpu->amd64_regs[amd64_rax];
    qword_t deps = 0, dep_count = 0, dep_index = 0;
    if (dso == 0)
        return;
    if (user_get((addr_t) dso + 0xb0, deps) || user_get((addr_t) dso + 0xc0, dep_count) ||
            user_get((addr_t) dso + 0xc8, dep_index)) {
        printk("amd64 loader: dso=%#llx (failed to read b0/c0/c8)\n",
               (unsigned long long) dso);
        return;
    }
    printk("amd64 loader: dso=%#llx deps=%#llx count=%#llx index=%#llx\n",
           (unsigned long long) dso,
           (unsigned long long) deps,
           (unsigned long long) dep_count,
           (unsigned long long) dep_index);
    if (deps != 0) {
        for (int i = 0; i < 4; i++) {
            qword_t slot = 0;
            if (user_get((addr_t) deps + i * sizeof(slot), slot))
                break;
            printk("amd64 loader: deps[%d]=%#llx\n", i, (unsigned long long) slot);
        }
    }
}

static void dump_amd64_store_trace(const struct cpu_state *cpu) {
    unsigned total = cpu->amd64_store_trace_next;
    unsigned start = total > AMD64_STORE_TRACE_COUNT ? total - AMD64_STORE_TRACE_COUNT : 0;
    for (unsigned i = start; i < total; i++) {
        const struct amd64_store_trace *trace = &cpu->amd64_store_trace[i % AMD64_STORE_TRACE_COUNT];
        if (trace->rip == 0)
            continue;
        printk("amd64 store: rip=%#llx opcode=%#x addr=%#llx value=%#llx\n",
               (unsigned long long) trace->rip,
               trace->opcode,
               (unsigned long long) trace->addr,
               (unsigned long long) trace->value);
    }
}

struct amd64_trap_rex_prefix {
    bool present;
    bool w;
    bool r;
    bool x;
    bool b;
};

struct amd64_trap_modrm {
    bool is_reg;
    uint8_t reg;
    uint8_t rm;
    bool has_base;
    uint8_t base;
    bool has_index;
    uint8_t index;
    uint8_t scale;
    bool rip_relative;
    int32_t disp;
};

static inline void amd64_trap_sync_legacy_regs(struct cpu_state *cpu) {
    cpu->eax = (dword_t) cpu->amd64_regs[amd64_rax];
    cpu->ecx = (dword_t) cpu->amd64_regs[amd64_rcx];
    cpu->edx = (dword_t) cpu->amd64_regs[amd64_rdx];
    cpu->ebx = (dword_t) cpu->amd64_regs[amd64_rbx];
    cpu->esp = (dword_t) cpu->amd64_regs[amd64_rsp];
    cpu->ebp = (dword_t) cpu->amd64_regs[amd64_rbp];
    cpu->esi = (dword_t) cpu->amd64_regs[amd64_rsi];
    cpu->edi = (dword_t) cpu->amd64_regs[amd64_rdi];
    cpu->eip = (dword_t) cpu->amd64_rip;
}

static inline qword_t amd64_trap_mask(unsigned size) {
    switch (size) {
    case 8: return 0xff;
    case 16: return 0xffff;
    case 32: return 0xffffffffu;
    case 64: return ~0ull;
    default: return 0;
    }
}

static inline qword_t amd64_trap_sign_bit(unsigned size) {
    return 1ull << (size - 1);
}

static inline qword_t amd64_trap_trunc(qword_t value, unsigned size) {
    return value & amd64_trap_mask(size);
}

static inline bool amd64_trap_guest_addr_ok(qword_t guest_addr, unsigned size, guest_addr_t *addr_out) {
    if (!guest_abi_range_valid(GUEST_ABI_AMD64, guest_addr, size))
        return false;
    *addr_out = guest_addr;
    return true;
}

static inline qword_t amd64_trap_reg_get(const struct cpu_state *cpu, unsigned reg, unsigned size) {
    qword_t value = cpu->amd64_regs[reg & 0xf];
    switch (size) {
    case 8: return value & 0xff;
    case 16: return value & 0xffff;
    case 32: return (uint32_t) value;
    case 64: return value;
    default: return value;
    }
}

static inline void amd64_trap_reg_set(struct cpu_state *cpu, unsigned reg, unsigned size, qword_t value) {
    reg &= 0xf;
    switch (size) {
    case 8:
        cpu->amd64_regs[reg] = (cpu->amd64_regs[reg] & ~0xffull) | (value & 0xff);
        break;
    case 16:
        cpu->amd64_regs[reg] = (cpu->amd64_regs[reg] & ~0xffffull) | (value & 0xffff);
        break;
    case 32:
        cpu->amd64_regs[reg] = (uint32_t) value;
        break;
    case 64:
        cpu->amd64_regs[reg] = value;
        break;
    default:
        break;
    }
}

static inline void amd64_trap_set_add_flags(struct cpu_state *cpu, qword_t lhs, qword_t rhs, qword_t result, unsigned size) {
    qword_t mask = amd64_trap_mask(size);
    qword_t lhs_masked = amd64_trap_trunc(lhs, size);
    qword_t rhs_masked = amd64_trap_trunc(rhs, size);
    qword_t res_masked = amd64_trap_trunc(result, size);
    cpu->cf = size == 64 ? res_masked < lhs_masked : ((lhs_masked + rhs_masked) & ~mask) != 0;
    cpu->of = ((~(lhs_masked ^ rhs_masked) & (lhs_masked ^ res_masked)) & amd64_trap_sign_bit(size)) != 0;
    cpu->af = ((lhs_masked ^ rhs_masked ^ res_masked) >> 4) & 1;
    cpu->af_ops = 0;
    cpu->zf = res_masked == 0;
    cpu->sf = (res_masked & amd64_trap_sign_bit(size)) != 0;
    cpu->pf = !__builtin_parity((unsigned) (res_masked & 0xff));
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    collapse_flags(cpu);
}

static bool amd64_trap_fetch_u8(guest_addr_t *ip, byte_t *out) {
    if (user_get(*ip, *out))
        return false;
    (*ip)++;
    return true;
}

static bool amd64_trap_fetch_i8(guest_addr_t *ip, int8_t *out) {
    return amd64_trap_fetch_u8(ip, (byte_t *) out);
}

static bool amd64_trap_fetch_i32(guest_addr_t *ip, int32_t *out) {
    if (user_get(*ip, *out))
        return false;
    *ip += sizeof(*out);
    return true;
}

static bool amd64_trap_mem_read(qword_t guest_addr, unsigned size, qword_t *value) {
    guest_addr_t addr;
    if (!amd64_trap_guest_addr_ok(guest_addr, size, &addr))
        return false;
    switch (size) {
    case 8: {
        uint8_t tmp;
        if (user_get(addr, tmp))
            return false;
        *value = tmp;
        return true;
    }
    case 16: {
        uint16_t tmp;
        if (user_get(addr, tmp))
            return false;
        *value = tmp;
        return true;
    }
    case 32: {
        uint32_t tmp;
        if (user_get(addr, tmp))
            return false;
        *value = tmp;
        return true;
    }
    case 64: {
        uint64_t tmp;
        if (user_get(addr, tmp))
            return false;
        *value = tmp;
        return true;
    }
    default:
        return false;
    }
}

static bool amd64_trap_mem_write(qword_t guest_addr, unsigned size, qword_t value) {
    guest_addr_t addr;
    if (!amd64_trap_guest_addr_ok(guest_addr, size, &addr))
        return false;
    switch (size) {
    case 8: {
        uint8_t tmp = value;
        return user_put(addr, tmp) == 0;
    }
    case 16: {
        uint16_t tmp = value;
        return user_put(addr, tmp) == 0;
    }
    case 32: {
        uint32_t tmp = value;
        return user_put(addr, tmp) == 0;
    }
    case 64: {
        uint64_t tmp = value;
        return user_put(addr, tmp) == 0;
    }
    default:
        return false;
    }
}

static bool amd64_trap_decode_modrm(guest_addr_t *ip, struct amd64_trap_rex_prefix rex, struct amd64_trap_modrm *modrm) {
    byte_t modrm_byte;
    if (!amd64_trap_fetch_u8(ip, &modrm_byte))
        return false;

    unsigned mod = modrm_byte >> 6;
    modrm->reg = ((modrm_byte >> 3) & 7) | (rex.r ? 8 : 0);
    modrm->rm = (modrm_byte & 7) | (rex.b ? 8 : 0);
    modrm->is_reg = mod == 3;
    modrm->has_base = false;
    modrm->has_index = false;
    modrm->rip_relative = false;
    modrm->disp = 0;
    modrm->scale = 0;

    if (modrm->is_reg)
        return true;

    unsigned rm_low = modrm_byte & 7;
    if (rm_low == 4) {
        byte_t sib;
        if (!amd64_trap_fetch_u8(ip, &sib))
            return false;
        unsigned base_low = sib & 7;
        unsigned index_low = (sib >> 3) & 7;
        modrm->scale = sib >> 6;
        // In 64-bit mode, SIB index 100 means "no index" only when REX.X is clear.
        if (index_low != 4 || rex.x) {
            modrm->has_index = true;
            modrm->index = index_low | (rex.x ? 8 : 0);
        }
        if (mod == 0 && base_low == 5) {
            modrm->has_base = false;
        } else {
            modrm->has_base = true;
            modrm->base = base_low | (rex.b ? 8 : 0);
        }
    } else if (mod == 0 && rm_low == 5) {
        modrm->rip_relative = true;
    } else {
        modrm->has_base = true;
        modrm->base = modrm->rm;
    }

    if (mod == 1) {
        int8_t disp8;
        if (!amd64_trap_fetch_i8(ip, &disp8))
            return false;
        modrm->disp = disp8;
    } else if (mod == 2 || (mod == 0 && (rm_low == 5 || (rm_low == 4 && !modrm->has_base)))) {
        int32_t disp32;
        if (!amd64_trap_fetch_i32(ip, &disp32))
            return false;
        modrm->disp = disp32;
    }
    return true;
}

static qword_t amd64_trap_effective_addr(struct cpu_state *cpu, const struct amd64_trap_modrm *modrm, bool fs_prefix, guest_addr_t rip_after_modrm) {
    qword_t addr = (qword_t) (sqword_t) modrm->disp;
    if (modrm->rip_relative)
        addr += rip_after_modrm;
    if (modrm->has_base)
        addr += cpu->amd64_regs[modrm->base];
    if (modrm->has_index)
        addr += cpu->amd64_regs[modrm->index] << modrm->scale;
    if (fs_prefix)
        addr += cpu->tls_ptr;
    return addr;
}

static bool amd64_trap_read_rm(struct cpu_state *cpu, const struct amd64_trap_modrm *modrm, bool fs_prefix, guest_addr_t rip_after_modrm, unsigned size, qword_t *value) {
    if (modrm->is_reg) {
        *value = amd64_trap_reg_get(cpu, modrm->rm, size);
        return true;
    }
    return amd64_trap_mem_read(amd64_trap_effective_addr(cpu, modrm, fs_prefix, rip_after_modrm), size, value);
}

static bool amd64_trap_write_rm(struct cpu_state *cpu, const struct amd64_trap_modrm *modrm, bool fs_prefix, guest_addr_t rip_after_modrm, unsigned size, qword_t value) {
    if (modrm->is_reg) {
        amd64_trap_reg_set(cpu, modrm->rm, size, value);
        return true;
    }
    return amd64_trap_mem_write(amd64_trap_effective_addr(cpu, modrm, fs_prefix, rip_after_modrm), size, value);
}

static bool amd64_trap_read_xmm_rm(struct cpu_state *cpu, const struct amd64_trap_modrm *modrm, bool fs_prefix,
        guest_addr_t rip_after_modrm, union xmm_reg *value) {
    if (modrm->is_reg) {
        *value = cpu->xmm[modrm->rm & 0xf];
        return true;
    }

    guest_addr_t addr;
    if (!amd64_trap_guest_addr_ok(amd64_trap_effective_addr(cpu, modrm, fs_prefix, rip_after_modrm), sizeof(*value), &addr))
        return false;
    return user_read(addr, value, sizeof(*value)) == 0;
}

static bool amd64_try_emulate_sse2_packed_integer(guest_addr_t ip, struct cpu_state *cpu) {
    struct amd64_trap_rex_prefix rex = {};
    bool operand_size_prefix = false;
    bool fs_prefix = false;
    byte_t opcode;
    guest_addr_t decode_ip = ip;

    while (true) {
        if (!amd64_trap_fetch_u8(&decode_ip, &opcode))
            return false;
        if (opcode == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (opcode == 0x2e || opcode == 0x3e) {
            continue;
        }
        if (opcode == 0x67) {
            continue;
        }
        if (opcode == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (opcode >= 0x40 && opcode <= 0x4f) {
            rex.present = true;
            rex.w = (opcode & 0x8) != 0;
            rex.r = (opcode & 0x4) != 0;
            rex.x = (opcode & 0x2) != 0;
            rex.b = (opcode & 0x1) != 0;
            continue;
        }
        break;
    }

    if (opcode != 0x0f)
        return false;
    if (!amd64_trap_fetch_u8(&decode_ip, &opcode))
        return false;

    // 66 0F D4/F4/F5 = paddq / pmuludq / pmaddwd (xmm); no-prefix 0F F5 =
    // pmaddwd (mmx). These are emulated here in the #UD handler rather than the
    // JIT bridge: the bridge cannot do the xmm paddq/pmuludq forms, and bridging
    // MMX pmaddwd trips a JIT block-chaining bug (the instruction *after* it
    // faults though its next_ip is correct). The #UD path advances rip one
    // instruction at a time, so it is immune -- the same reason paddq/pmuludq
    // already live here.
    struct amd64_trap_modrm modrm;
    if (operand_size_prefix) {
        if (opcode != 0xd4 && opcode != 0xf4 && opcode != 0xf5)
            return false;
        if (!amd64_trap_decode_modrm(&decode_ip, rex, &modrm))
            return false;
        union xmm_reg src;
        if (!amd64_trap_read_xmm_rm(cpu, &modrm, fs_prefix, decode_ip, &src))
            return false;
        union xmm_reg *dst = &cpu->xmm[modrm.reg & 0xf];
        switch (opcode) {
        case 0xd4: // PADDQ xmm, xmm/m128
            dst->qw[0] += src.qw[0];
            dst->qw[1] += src.qw[1];
            break;
        case 0xf4: // PMULUDQ xmm, xmm/m128
            dst->qw[0] = (uint64_t) dst->u32[0] * src.u32[0];
            dst->qw[1] = (uint64_t) dst->u32[2] * src.u32[2];
            break;
        case 0xf5: // PMADDWD xmm, xmm/m128
            vec_madd_d128(NULL, &src, dst);
            break;
        }
    } else {
        if (opcode != 0xf5) // no-prefix: only MMX pmaddwd is handled here
            return false;
        if (!amd64_trap_decode_modrm(&decode_ip, rex, &modrm))
            return false;
        if (modrm.reg >= 8 || (modrm.is_reg && modrm.rm >= 8))
            return false; // mm[] has 8 entries; reject an invalid MMX encoding (-> #UD)
        union mm_reg src_mm;
        if (modrm.is_reg) {
            src_mm = cpu->mm[modrm.rm & 7];
        } else {
            qword_t v;
            if (!amd64_trap_read_rm(cpu, &modrm, fs_prefix, decode_ip, 64, &v))
                return false;
            src_mm.qw = v;
        }
        vec_madd_d64(NULL, &src_mm, &cpu->mm[modrm.reg & 7]);
    }

    cpu->amd64_rip = decode_ip;
    amd64_trap_sync_legacy_regs(cpu);
    return true;
}

static bool amd64_try_emulate_add(guest_addr_t ip, struct cpu_state *cpu) {
    struct amd64_trap_rex_prefix rex = {};
    bool operand_size_prefix = false;
    bool fs_prefix = false;
    byte_t opcode;
    guest_addr_t decode_ip = ip;

    while (true) {
        if (!amd64_trap_fetch_u8(&decode_ip, &opcode))
            return false;
        if (opcode == 0x66) {
            operand_size_prefix = true;
            continue;
        }
        if (opcode == 0x2e || opcode == 0x3e) {
            continue;
        }
        if (opcode == 0x67) {
            continue;
        }
        if (opcode == 0x64) {
            fs_prefix = true;
            continue;
        }
        if (opcode >= 0x40 && opcode <= 0x4f) {
            rex.present = true;
            rex.w = (opcode & 0x8) != 0;
            rex.r = (opcode & 0x4) != 0;
            rex.x = (opcode & 0x2) != 0;
            rex.b = (opcode & 0x1) != 0;
            continue;
        }
        break;
    }

    if (opcode != 0x01)
        return false;

    unsigned op_size = rex.w ? 64 : (operand_size_prefix ? 16 : 32);
    struct amd64_trap_modrm modrm;
    if (!amd64_trap_decode_modrm(&decode_ip, rex, &modrm))
        return false;

    qword_t lhs, rhs, result;
    if (!amd64_trap_read_rm(cpu, &modrm, fs_prefix, decode_ip, op_size, &lhs))
        return false;
    rhs = amd64_trap_reg_get(cpu, modrm.reg, op_size);
    result = amd64_trap_trunc(lhs + rhs, op_size);
    if (!amd64_trap_write_rm(cpu, &modrm, fs_prefix, decode_ip, op_size, result))
        return false;

    amd64_trap_set_add_flags(cpu, lhs, rhs, result, op_size);
    cpu->amd64_rip = decode_ip;
    amd64_trap_sync_legacy_regs(cpu);
    return true;
}

void handle_illegal_instruction_interrupt(struct cpu_state *cpu) {
    guest_addr_t ip = current_fault_ip(cpu);
    if (current->abi == GUEST_ABI_AMD64) {
        int nop_len = amd64_nop_instruction_len(ip);
        if (nop_len > 0) {
            cpu->amd64_rip = ip + nop_len;
            amd64_trap_sync_legacy_regs(cpu);
            return;
        }
        if (amd64_try_emulate_add(ip, cpu))
            return;
        if (amd64_try_emulate_sse2_packed_integer(ip, cpu))
            return;
    }

    printk("ERROR: %d(%s) [%s] illegal instruction at %#llx: ",
           current->pid, current->comm, guest_abi_desc(current->abi).name, (unsigned long long) ip);
    for (int i = 0; i < 8; i++) {
        uint8_t b;
        if (user_get(ip + i, b))
            break;
        printk("%02x ", b);
    }
    printk("\n");
    dump_opcode_window(ip);
    if (current->abi == GUEST_ABI_AMD64) {
        dump_amd64_regs(cpu);
        dump_amd64_cc1_trace(cpu);
        dump_amd64_cc1_jit_trace(cpu);
        if (amd64_verbose_fault_trace_enabled()) {
            dump_amd64_loader_state(cpu);
            dump_amd64_store_trace(cpu);
        }
    }
    record_guest_fault_event("illegal-instruction", cpu, current_fault_ip(cpu), false);
    dump_stack(8);
    struct siginfo_ info = {
        .code = ILL_ILLOPC_,
        .fault.addr = current_fault_ip(cpu),
    };
    deliver_signal(current, SIGILL_, info);
}

static void handle_bus_interrupt(struct cpu_state *cpu) {
    // A host bad-access (SIGBUS) taken by the JIT on a file-backed guest mapping
    // whose backing shrank (ftruncate under a live mmap), reverse-mapped to the
    // guest fault address by jit_translate_host_fault(). Unlike a page fault the
    // guest page is validly mapped in iSH's page table, so there is nothing for
    // mem_ptr_fault to resolve -- deliver a guest SIGBUS directly, matching
    // Linux (accessing a truncated page of a file mapping => SIGBUS/BUS_ADRERR).
    printk("ERROR: %d(%s) [%s] SIGBUS on %#llx at %#llx (file-backed mmap truncated)\n",
           current->pid, current->comm, guest_abi_desc(current->abi).name,
           (unsigned long long) cpu->segfault_addr,
           (unsigned long long) current_fault_ip(cpu));
    record_guest_fault_event("bus-error", cpu, cpu->segfault_addr, cpu->segfault_was_write);
    struct siginfo_ info = {
        .code = BUS_ADRERR_,
        .fault.addr = cpu->segfault_addr,
    };
    deliver_signal(current, SIGBUS_, info);
}

static void handle_arithmetic_interrupt(struct cpu_state *cpu) {
    printk("ERROR: %d(%s) [%s] arithmetic fault at 0x%x\n",
           current->pid, current->comm, guest_abi_desc(current->abi).name, cpu->eip);
    dump_stack(8);
    struct siginfo_ info = {
        .code = FPE_INTDIV_,
        .fault.addr = current_fault_ip(cpu),
    };
    deliver_signal(current, SIGFPE_, info);
}

static void handle_privileged_instruction_interrupt(struct cpu_state *cpu) {
    cpu->trapno = INT_GPF;
    handle_general_protection_interrupt(cpu);
}

void handle_timer_interrupt(__attribute__((unused)) struct cpu_state *cpu) {
    // For now we just return.
    return;
}

void handle_interrupt(int interrupt) {
    struct cpu_state *cpu = &current->cpu;

    switch (interrupt) {
        case INT_SYSCALL:
            handle_syscall_interrupt(cpu);
            break;
        case INT_AMD64_SYSCALL:
            handle_amd64_syscall_interrupt(cpu);
            break;
        case INT_ARM64_SVC:
            handle_arm64_syscall_interrupt(cpu);
            break;
        case INT_RISCV64_ECALL:
            handle_riscv64_syscall_interrupt(cpu);
            break;
        case INT_PF:
            handle_page_fault_interrupt(cpu);
            break;
        case INT_GPF:
            handle_general_protection_interrupt(cpu);
            break;
        case INT_BUS:
            handle_bus_interrupt(cpu);
            break;
        case INT_UNDEFINED:
            handle_illegal_instruction_interrupt(cpu);
            break;
        case INT_DIV:
            handle_arithmetic_interrupt(cpu);
            break;
        case INT_PRIV:
            handle_privileged_instruction_interrupt(cpu);
            break;
        case INT_BREAKPOINT:
            // Debug exceptions are forced synchronous traps on Linux
            // (force_sig_fault): if the guest has SIGTRAP blocked (e.g.
            // musl blocks everything around abort/thread teardown before a
            // V8 IMMEDIATE_CRASH brk executes), delivery must force-default
            // and kill rather than leave it pending while the brk
            // re-executes forever. send_signal is the kill()-style path
            // without that semantic.
            deliver_signal(current, SIGTRAP_, (struct siginfo_) {
                .sig = SIGTRAP_,
                .code = TRAP_BRKPT_,
                .fault.addr = current_fault_ip(cpu),
            });
            break;
        case INT_DEBUG:
            deliver_signal(current, SIGTRAP_, (struct siginfo_) {
                .sig = SIGTRAP_,
                .code = TRAP_TRACE_,
                .fault.addr = current_fault_ip(cpu),
            });
            break;
        case INT_TIMER:
            handle_timer_interrupt(cpu); // Just a stub for now
            break;
        default:
            printk("WARNING: %d(%s) unhandled interrupt %d\n", current->pid, current->comm, interrupt);
            sys_exit(interrupt);
            break;
    }
    // Host-side wakeups (for example thread pokes or interrupted waits) often
    // arrive with no guest-visible pending signal work. Avoid serializing all
    // runnable threads on sighand->lock in that common case.
    sigset_t_ pending = __atomic_load_n(&current->pending, __ATOMIC_ACQUIRE);
    sigset_t_ group_pending = __atomic_load_n(&current->sighand->pending, __ATOMIC_ACQUIRE);
    sigset_t_ blocked = __atomic_load_n(&current->blocked, __ATOMIC_ACQUIRE);
    bool has_saved_mask = __atomic_load_n(&current->has_saved_mask, __ATOMIC_ACQUIRE);
    if (has_saved_mask || ((pending | group_pending) & ~blocked) != 0)
        receive_signals();
    // A job-control group-stop parks the task until SIGCONT (or, for a traced
    // task, until the tracer resumes it). Shared with native_checkpoint --
    // kernel/signal.c -- because a native program needs exactly the same
    // handling and the second copy this used to be lacked all of the ptrace
    // half of it.
    group_stop_wait();
}


void dump_maps(void) {
    extern void proc_maps_dump(struct task *task, struct proc_data *buf);
    struct proc_data buf = {};
    proc_maps_dump(current, &buf);
    // go a line at a time because it can be fucking enormous
    char *orig_data = buf.data;
    while (buf.size > 0) {
        size_t chunk_size = buf.size;
        if (chunk_size > 1024)
            chunk_size = 1024;
        printk("%.*s", chunk_size, buf.data);
        buf.data += chunk_size;
        buf.size -= chunk_size;
    }
    free(orig_data);
}

void dump_mem(guest_addr_t start, uint_t len) {
    const int width = 8;
    if (len == 0) {
        printk("ERROR: No memory to dump\n");
        return;
    }

    for (guest_addr_t addr = start; addr < start + len; addr += sizeof(dword_t)) {
        unsigned from_left = (addr - start) / sizeof(dword_t) % width;
        if (from_left == 0)
            printk("%08llx: ", (unsigned long long) addr);

        dword_t word;
        if (user_get(addr, word)) {
            printk("ERROR: Unable to read memory at address %08llx\n", (unsigned long long) addr);
            break;
        }
        printk("%08x ", word);

        if (from_left == width - 1)
            printk("\n");
    }
}

void dump_stack(int lines) {
    guest_addr_t sp;
    switch (current->abi) {
        case GUEST_ABI_AMD64:
            sp = current->cpu.amd64_regs[amd64_rsp];
            printk("stack at %#llx, bp at %#llx, ip at %#llx\n",
                   (unsigned long long) sp,
                   (unsigned long long) current->cpu.amd64_regs[amd64_rbp],
                   (unsigned long long) current->cpu.amd64_rip);
            break;
        case GUEST_ABI_ARM64:
            sp = current->cpu.arm64_sp;
            printk("stack at %#llx, fp at %#llx, pc at %#llx\n",
                   (unsigned long long) sp,
                   (unsigned long long) current->cpu.arm64_regs[arm64_x29],
                   (unsigned long long) current->cpu.arm64_pc);
            break;
        case GUEST_ABI_RISCV64:
            sp = current->cpu.riscv64_regs[riscv64_sp];
            printk("stack at %#llx, fp at %#llx, pc at %#llx\n",
                   (unsigned long long) sp,
                   (unsigned long long) current->cpu.riscv64_regs[riscv64_s0],
                   (unsigned long long) current->cpu.riscv64_pc);
            break;
        default:
            sp = current->cpu.esp;
            printk("stack at %x, base at %x, ip at %x\n",
                   current->cpu.esp, current->cpu.ebp, current->cpu.eip);
            break;
    }
    if (lines <= 0) {
        printk("ERROR: Invalid number of lines specified for stack dump\n");
        return;
    }
    // Ensure lines is within a reasonable limit to prevent excessive output
    const int max_lines = 20;
    lines = (lines > max_lines) ? max_lines : lines;

    dump_mem(sp, lines * sizeof(dword_t) * 8);
}

// TODO find a home for this
#ifdef LOG_OVERRIDE
int log_override = 0;
#endif
