// checkpoint.c -- save a running guest to a file and bring it back.
//
// WHAT THIS IS FOR
//
// iOS terminates this app routinely: jetsam, memory pressure, a user swiping it
// away. Today that loses the session unconditionally. The promise here is
// narrow and specific -- same device, same root, same build, back where you
// were -- and it is deliberately NOT "checkpoint any process", which is where
// this class of feature usually dies. CRIU has been at the general problem for
// a decade with a real kernel's cooperation and is still partial.
//
// WHY IT IS TRACTABLE HERE AT ALL, which is the interesting part: AOK owns the
// scheduler. There is no host kernel to negotiate with about when a guest
// thread is quiet. Every guest task passes through the top of
// task_run_current's loop, and at that point it holds no address-space lock, is
// not inside a syscall, and its struct cpu_state IS the whole of its
// execution state. Stopping the machine is a flag; the state is a struct.
//
// WHERE THE CHECKPOINT IS TAKEN, and it is not arbitrary. A guest writes to
// /proc/ish/checkpoint; that write does NOT save anything. It sets a flag, and
// the save happens at the next pass round task_run_current's loop -- after the
// syscall's return value has been stored in the guest's register file and the
// program counter has moved past it. So the image describes a task about to
// execute the instruction AFTER the write, and a restore continues rather than
// re-running. The write returns 0 in both lives, and the program tells them
// apart by reading /proc/ish/checkpoint back.
//
// WHAT v1 REFUSES, out loud, rather than approximating:
//
//   - More than one live task. The mechanism to stop several is the same flag,
//     but a task blocked INSIDE a syscall -- a read on a pipe, a wait for a
//     child -- is not at a boundary, and recording "restart this syscall" per
//     family is the next phase's work, not this one's.
//   - A native program. A native program is a C function on a host thread
//     (kernel/native.h); there is no serialising a host C stack. The rule the
//     project already has is that a native program either knows how to dump
//     its own state or the checkpoint refuses while it is running -- and it
//     refuses here, by name, so the limit is reportable rather than silent.
//   - Any descriptor with no restore rule: a pipe, a socket, an unlinked file,
//     an epoll set. Regular files, directories and the console come back;
//     everything else is named in the refusal.
//   - A different build. struct cpu_state is written as bytes, so the image
//     carries the build's own fingerprint and a mismatch is refused rather
//     than reinterpreted.
//
// WHAT IT DOES NOT REFUSE BUT DOES NOT PRESERVE: a mapping's NAME. Every
// mapping comes back as anonymous memory holding the bytes that were in it,
// which is semantically exact for a private mapping (a file-backed private
// page that has been written is already a private copy, and one that has not
// is identical to the file) and wrong only for /proc/<pid>/maps, which will
// call the text segment anonymous. A MAP_SHARED file mapping is not exact and
// is refused.
//
// THE FORMAT is a header, then one task record, then its mappings, then its
// descriptors. No compression: kernel/zswap.c already compresses guest frames
// and is the right place to do it, but wiring the pool into a file written
// once and read once is phase 1's job, and an uncompressed image is the thing
// to measure it against.

#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kernel/ipc_ns.h"
#include "kernel/calls.h"
#include "kernel/checkpoint.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/mm.h"
#include "kernel/personality.h"
#include "kernel/resource.h"
#include "kernel/signal.h"
#include "kernel/init.h"
#include "kernel/native.h"
#include "kernel/task.h"
#include "kernel/seccomp.h"
#include "kernel/uts.h"
#include "fs/fd.h"
#include "fs/path.h"
#include "fs/tty.h"
#include "fs/sock_ckpt.h"
#include "fs/real.h"
#include "fs/fifo.h"
#include "kernel/anonfd_ckpt.h"
#include "kernel/timer_ckpt.h"
#include "fs/devices.h"
#include "emu/memory.h"
#include "util/sync.h"

#define CKPT_MAGIC "AOKCKPT"
#define CKPT_VERSION 21  // 21: the capability bounding set; 20: NX -- 64-bit guests return from signals through a [sigpage], and the personality; 19: seccomp mode and filters, dumpable; 18: a queued signal says whether it is a POSIX timer's own; 17: no_new_privs; 16: the executable behind /proc/<pid>/exe, capabilities, supplementary groups; 15: the root it was saved from; 14: timers, queued signals, the deadline a frozen wait carries; timerfd as a deadline; 13: the guest's clocks, task start times, timerfd guest clock; 12: a terminal record names its terminal; 11: socket options and unix node attributes; 10: threads and shared objects; 9: socket pairs; 8: anon fds + epoll section; 7: pty slave owner; 6: tmpfs contents; 4: ckpt_task.native_standin_child
                         // 5: ckpt_map.kind, reservations saved as reservations
// How long the freezer waits for a task to reach a syscall boundary.
//
// Generous on purpose. Every wait in the guest is broken by the poke, so a
// task normally parks in microseconds; the cases that take longer are a task
// inside a host syscall that the SIGUSR1 has to interrupt, and one running a
// long stretch of guest code between checkpoints. Five seconds is long enough
// that neither is a flake on a busy machine -- two was not, and the
// first thing it failed under was this project's own test suite running
// beside it -- and short enough that a genuinely stuck task is reported
// rather than waited for.
#define CKPT_FREEZE_TIMEOUT_MS 5000

// Kinds of descriptor this version knows how to bring back. Anything else is a
// refusal naming the fd number and the filesystem it came from, because "the
// checkpoint failed" is useless and "fd 7 is a pipe" is actionable.
enum ckpt_fd_kind {
    CKPT_FD_FILE = 1,     // regular file: re-open by path, seek to offset
    CKPT_FD_DIR,          // directory: re-open by path
    CKPT_FD_TTY,          // the console: re-attach to this run's tty
    CKPT_FD_STDIO,        // 0/1/2 as the app handed them over: re-attach too
    CKPT_FD_CHR,          // /dev/null and friends: re-open the device by path
    CKPT_FD_PIPE,         // one end of a pipe, with whatever is still in it
    CKPT_FD_REF,          // the SAME struct fd as one already described
    CKPT_FD_SOCKET,       // a socket: rebuilt from its description, not copied
    // A pseudo-terminal INTERNAL to the image: one guest process holds the
    // master, another is on the slave. tmux, screen, script, expect and sshd
    // all look like this, and none of them is the UI's terminal -- which is
    // why they cannot go through CKPT_FD_TTY, whose whole model is "re-attach
    // to the terminal this run is using". A tmux pane restored that way came
    // back on a fresh window of its own while tmux's master pointed at
    // nothing, so tmux tore the window down and the server exited.
    //
    // The pair travels as its pty NUMBER (in `offset`): the master re-opens
    // /dev/ptmx, which allocates a new number, and the restore remembers
    // old -> new so the slave can find its way to the same pty.
    CKPT_FD_PTY_MASTER,
    CKPT_FD_PTY_SLAVE,
    // Descriptors with no file behind them (kernel/anonfd_ckpt.h). Each is
    // rebuilt from a description that follows its record -- `offset` bytes of
    // it -- except an epoll set's registrations, which follow the last task:
    // they name descriptors that may belong to other processes.
    CKPT_FD_EPOLL,
    CKPT_FD_EVENTFD,
    CKPT_FD_SIGNALFD,
    CKPT_FD_TIMERFD,
    CKPT_FD_INOTIFY,
    CKPT_FD_PIDFD,
    CKPT_FD_MEMFD,
    // A named FIFO: reopened by its path -- the node itself comes back with
    // its filesystem -- with whatever was buffered in it following the record.
    CKPT_FD_FIFO,
};

// Which KIND of terminal a process's standard streams were on. The two are
// not interchangeable: the console is the system's, lives at a fixed path and
// is simply re-opened, while a pseudo-terminal is a WINDOW -- created by the
// UI, destroyed with it -- and comes back only by making a new one and handing
// it to the UI to adopt. Collapsing the second into the first is what a
// restored session looked like before this existed: alive, correct, and
// talking to a terminal nobody was looking at, while the app started a fresh
// shell in the window the user could see.
enum ckpt_tty_kind {
    CKPT_TTY_NONE = 0,
    CKPT_TTY_CONSOLE,
    CKPT_TTY_PTS,
};

// ISH_CHECKPOINT_DEBUG=1 traces every record on the way out and on the way
// back. On the HOST's stderr, not the guest's: at restore time the guest has
// no descriptors yet, and at save time the thing being diagnosed is usually
// which descriptor the guest is holding.
static bool ckpt_debug(void) {
    // Read once. This is consulted from the syscall path, where a getenv per
    // call would be a measurable cost for a diagnostic that is off.
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("ISH_CHECKPOINT_DEBUG");
        cached = e != NULL && e[0] != '\0' && e[0] != '0';
    }
    return cached != 0;
}
#define CKPT_TRACE(...) do { \
    if (ckpt_debug()) { fprintf(stderr, "checkpoint: " __VA_ARGS__); } \
} while (0)

// The flags a descriptor is REOPENED with. fd->flags keeps everything the
// original open() was given, and handing that back verbatim replayed the
// open's side effects: O_TRUNC emptied the file -- `cmd > out` running across
// a suspend came back with everything written before it replaced by NULs up
// to the restored offset -- and O_CREAT|O_EXCL made an existing file EEXIST,
// which the degrade then turned into /dev/null. A reopen finds what is there;
// it never creates, truncates or makes an anonymous file.
static int ckpt_reopen_flags(uint32_t flags) {
    return (int) flags & ~(O_CREAT_ | O_EXCL_ | O_TRUNC_ | O_TMPFILE_);
}

static const char *ckpt_kind_name(uint32_t kind) {
    switch (kind) {
        case CKPT_FD_FILE: return "file";
        case CKPT_FD_DIR: return "dir";
        case CKPT_FD_TTY: return "tty";
        case CKPT_FD_CHR: return "chr";
        case CKPT_FD_PIPE: return "pipe";
        case CKPT_FD_REF: return "ref";
        case CKPT_FD_STDIO: return "stdio";
        case CKPT_FD_SOCKET: return "sock";
        case CKPT_FD_PTY_MASTER: return "ptmx";
        case CKPT_FD_PTY_SLAVE: return "pts";
        case CKPT_FD_EPOLL: return "epoll";
        case CKPT_FD_EVENTFD: return "evfd";
        case CKPT_FD_SIGNALFD: return "sigfd";
        case CKPT_FD_TIMERFD: return "tmrfd";
        case CKPT_FD_INOTIFY: return "inotf";
        case CKPT_FD_PIDFD: return "pidfd";
        case CKPT_FD_MEMFD: return "memfd";
        case CKPT_FD_FIFO: return "fifo";
        default: return "?";
    }
}

struct ckpt_header {
    char magic[8];
    uint32_t version;
    uint32_t abi;
    uint32_t cpu_state_size;   // struct cpu_state is written as bytes
    uint32_t page_size;
    uint64_t build_fingerprint;
    uint32_t n_tasks;
    uint64_t total_pages;
    // WHICH console, and which device it has to be. The CLI's is /dev/tty1
    // (4:1) and the app's is /dev/console (5:1) -- a session that came back on
    // the other one would be talking to a terminal nobody is looking at.
    // create_stdio checks the major/minor and falls back to an adhoc node if
    // they do not match, so both halves travel.
    char console[64];
    uint32_t console_major, console_minor;
    // The UTS namespace: the guest's own state, and nothing on the resume path
    // puts it back. The app seeds the hostname while provisioning /etc/hostname
    // on a fresh boot, which a resume skips entirely -- so a restored shell
    // kept the name it had cached and every shell started afterwards disagreed
    // with it, two shells on one rootfs apparently on different machines.
    char hostname[UTS_NAME_LENGTH];
    char domainname[UTS_NAME_LENGTH];
    // How many tmpfs mounts are described between this header and the first
    // task record. See the tmpfs contents section below.
    uint32_t n_tmpfs;
    // How many epoll registrations follow the last task record.
    uint32_t n_epoll_regs;
    uint32_t reserved2;
    // The guest's clocks at the freeze, and the host's wall clock with them
    // (struct guest_clock_reading). Without them a restored machine's clocks
    // restarted at zero, and every absolute CLOCK_MONOTONIC deadline in the
    // image -- Python's time.sleep is one -- waited an extra "uptime at the
    // save". The restore continues MONOTONIC from these and advances BOOTTIME
    // by the wall-clock time the machine spent stopped (guest_clock_resume).
    int64_t clock_monotonic_ns, clock_boottime_ns, clock_raw_ns;
    int64_t clock_realtime_ns;
    int64_t clock_boot_time;
    // The root the machine was running on (checkpoint_root_identity), and its
    // name for the refusal. A session is its processes' view of ONE
    // filesystem: every descriptor reopens by path, every mapping names a file
    // in it. Resumed on another root, those paths still resolved -- in the
    // wrong files. Saved on Devuan and resumed on Alpine, the "Devuan" shell
    // read Alpine's /etc/os-release, and a descriptor open for writing would
    // have written into Alpine. 0 means the root had no identity to record.
    uint64_t root_identity;
    char root_name[64];
};

struct ckpt_task {
    uint32_t pid, ppid, pgid, sid;
    uint32_t n_maps, n_fds;
    uint32_t abi;
    // A zombie: no address space, no descriptors, nothing but an exit status
    // its parent has not collected yet. Recorded because dropping it turns the
    // parent's wait() into a hang on the far side of a restore.
    uint32_t zombie, exit_code;
    // What a child sends its parent when it dies. Set by clone() from the
    // flags; zero on a freshly built task, which is exactly what a restored
    // child was getting -- it exited cleanly, sent nothing, and its parent's
    // wait() never returned. The rest travel with it for the same reason:
    // nothing else puts them back.
    int32_t exit_signal, pdeath_signal, nice, sched_policy;
    uint64_t robust_list;
    uint32_t did_exec;
    // The terminal this process's standard streams were on: enum ckpt_tty_kind,
    // and for a pty the number and path it had. The path is kept for the trace
    // and for the refusal message; nothing re-opens it, because the pty it
    // names is gone.
    uint32_t tty_kind;
    uint32_t tty_num;
    char tty_path[64];
    // The terminal's SESSION and FOREGROUND process group. Not decoration:
    // a read from a terminal by a process outside the foreground group is
    // EIO when the shell has SIGTTIN ignored, which every interactive shell
    // does -- so a restored session whose terminal came back with the wrong
    // foreground group read EIO on its first prompt and exited. Letting the
    // first process to open the new terminal claim it gave the LOGIN's group,
    // never the shell's.
    int32_t tty_session, tty_fg_group;
    // And the line discipline, byte for byte. A shell puts its terminal into
    // raw mode for its own line editing and puts it back when it exits; a
    // terminal rebuilt with the driver's defaults has ECHO on under a shell
    // that is still echoing for itself, so every keystroke came back twice.
    // The window size travels with it -- the UI re-syncs its own size a moment
    // later, but until it does the guest should not think the terminal changed
    // shape.
    struct termios_ tty_termios;
    struct winsize_ tty_winsize;
    // A NATIVE task. There is no address space to photograph and no register
    // file that means anything -- it is a C function on a host thread -- so
    // what travels is the program's name, the argv it was given, and the state
    // it produced about itself. n_maps is 0 and no cpu_state follows; the
    // three blobs below do, in this order, each NUL-terminated.
    uint32_t native;
    uint32_t native_name_len, native_argv_len, native_state_len, native_env_len;
    // The child a native exec stand-in was waiting on, or 0 (struct task's
    // native_standin_child). Such a task comes back as that wait, never by
    // running its program -- and so the command -- a second time.
    uint32_t native_standin_child;
    uint32_t uid, gid, euid, egid, suid, sgid, fsuid, fsgid;
    uint32_t umask;
    char comm[16];
    uint64_t blocked, pending;
    uint64_t altstack, altstack_size;
    uint64_t clear_tid;
    uint64_t brk, start_brk, vdso, stack_start;
    uint64_t argv_start, argv_end, env_start, env_end, auxv_start, auxv_end;
    uint32_t n_sigactions;
    uint32_t cwd_len, root_len;
    // The address space's SHAPE, which the ELF loader sets per guest
    // architecture and which nothing else puts back. Without page_limit a
    // restored arm64 guest gets a fresh mm's default -- a 32-bit one -- and
    // every mapping above 4 GiB fails to map with ENOMEM, which is what the
    // first restore attempt did.
    uint64_t page_limit, mmap_floor, mmap_ceiling;
    uint64_t stack_top, stack_limit_pages;
    // THREADS, and anything else that shares with another task. tgid is the
    // thread group. Each owner names the EARLIER task in the image that holds
    // the same address space, descriptor table, fs info or signal handlers --
    // 0 when this task's own follow in this record. A task that shares its
    // address space writes no maps (n_maps is 0) and one that shares its table
    // writes no descriptors (n_fds is 0): the owner's record carries them.
    //
    // Before these, every task was a process. A three-thread rsyslogd came
    // back as three processes, each with a private copy of what had been one
    // address space -- the image held it three times -- so a futex wake, a
    // queue, anything one thread handed another, stopped crossing between
    // them, and nothing said so.
    uint32_t tgid;
    uint32_t mm_owner, files_owner, fs_owner, sighand_owner;
    // A thread group's leader that EXITED while other threads of its process
    // run on. Linux keeps it as a zombie; AOK keeps it in the pid table, off
    // its group's thread list and holding nothing (kernel/exit.c), because
    // the group's exit is reported as the leader's -- to the leader's parent,
    // with the leader's exit signal. Recorded like a zombie, with no maps and
    // no descriptors; `zombie` is 0. Without it the image had threads whose
    // process it did not have.
    uint32_t departed;
    // When it started, in uptime ticks (/proc/<pid>/stat's starttime). The
    // restored uptime goes on from the image's, so this still means what it
    // did; the restore's own clock reading would make every process look as
    // though it had started at the restore.
    uint64_t start_time_ticks;
    // The syscall this task was parked in, rewound to run again, and what it
    // had left. A sleep or a poll-family wait the freeze interrupted carries
    // its deadline into the call that re-executes it (struct task's
    // sleep_restart_deadline and poll_restart_deadline); here it travels on
    // the guest clock Linux would count it on (kernel/timer_ckpt.h), because a
    // host deadline means nothing to the next run. Without it the call started
    // its whole timeout again after the restore: `sleep 5`, frozen 3 s in,
    // slept 5 s more. And whether a handler that runs before the call does
    // cancels the restart, which is Linux's answer and was lost with the task.
    uint32_t sleep_restart_valid, sleep_restart_clock;   // the sleep's guest clockid
    int64_t sleep_restart_value_ns;
    uint32_t poll_restart_valid, restart_pending;        // bit 0 NOHAND, bit 1 SYS
    int64_t poll_restart_value_ns;
    // Signals sent and not yet taken, each a struct ckpt_sigqueue after the
    // descriptors: this task's own, and -- in the record of the task that
    // holds its signal handlers for the others (sighand_owner 0) -- the
    // process's. The pending sets are rebuilt from them. `pending` alone came
    // back before these, as a bit with no signal behind it: never delivered,
    // since delivery takes from the queue, and ending every wait at once,
    // since the waits ask the bit. A blocked SIGALRM was lost, and unblocked,
    // it spun. (`pending` still carries a native program's, as before.)
    uint32_t n_sigqueue, n_group_sigqueue;
    // Its process's timers follow the signals: a struct group_timers_ckpt and
    // its POSIX timers. In the record of the process's first task in the
    // image that is running -- not a departed leader, whose group lives on in
    // its threads -- and never a native program's, which is re-launched and
    // arms its own.
    uint32_t group_timers;
    uint32_t reserved3;
    // The executable behind /proc/<pid>/exe (mm->exefile), as a path after the
    // root's, in the record that owns the address space. It came back empty:
    // readlink /proc/self/exe failed in every restored process, and ktop, which
    // reads the ELF header through that link, showed "?" as the architecture of
    // all of them.
    uint32_t exe_len;
    // The rest of the credentials. Only the uids and gids travelled, so every
    // restored task kept what the RESTORING task had: root's full capability
    // set and its supplementary groups, for a uid-1000 shell as much as for
    // init -- a restore was a privilege escalation. The groups follow the exe
    // path, ngroups uint32s.
    uint32_t cap_effective[2], cap_permitted[2], cap_inheritable[2], cap_ambient[2];
    // A capability dropped from it stays dropped: restored with the restoring
    // task's full set instead, it would come back at the next exec.
    uint32_t cap_bounding[2];
    uint32_t keepcaps;
    uint32_t ngroups;
    // PR_SET_NO_NEW_PRIVS (struct task's no_new_privs). A sandbox sets it and
    // then runs what it does not trust; restored without it, the process could
    // gain privilege from a set-id binary again, and there is no way for it
    // to find out.
    uint32_t no_new_privs;
    // seccomp (kernel/seccomp.h): the mode, and the filters as programs,
    // oldest first. Restored without them a sandboxed process would come back
    // unconfined, which it has no way to notice. seccomp_nprogs records follow
    // the supplementary groups, each a struct ckpt_seccomp_prog and its
    // instructions.
    uint32_t seccomp_mode;
    uint32_t seccomp_nprogs;
    // The process is not dumpable (struct tgroup's undumpable): a restore must
    // not open it up to its user's ptrace and /proc.
    uint32_t undumpable;
    // personality(2), the process's: READ_IMPLIES_EXEC decides whether what
    // it maps from now on is executable, so a pre-NX program restored without
    // it would fault on code that ran before the save. (An image from before
    // version 20 is refused outright: its 64-bit processes have no [sigpage],
    // and their signal handlers would return onto a stack that no longer
    // executes.)
    uint32_t personality;
};

struct ckpt_seccomp_prog {
    uint32_t log;
    uint32_t len;   // classic BPF instructions, 8 bytes each, follow
};
// More filters than any path could run through (each costs at least five of
// Linux's 32768-instruction budget): a sanity bound for a damaged image.
#define CKPT_MAX_SECCOMP_PROGS 8192

struct ckpt_map {
    uint64_t start;    // guest address
    uint64_t pages;
    uint32_t flags;    // P_*; for CKPT_MAP_RESERVED, struct mem_lazy_map's
    uint32_t kind;     // enum ckpt_map_kind. Was `reserved`, always 0, in v4.
};

// What follows a map record. A lazy anonymous reservation (emu/memory.h,
// struct mem_lazy_map) is address space the guest has mapped with no page-table
// entries yet, so a walk of the entries does not see it. Version 4 walked only
// the entries, and a restored process lost every reservation: a JVM's PROT_NONE
// heap came back as a hole that the next mmap could land in (SEGV_MAPERR where
// the heap had given SEGV_ACCERR), and the untouched tail of a large RW mapping
// faulted on the first write. That got worse when splits began leaving the
// remainders of a reservation reserved rather than materialised. Writing them
// out as zero pages instead would be correct and cost the whole reservation in
// the image -- 537 MB for one committed page in a 512 MB heap -- so a
// reservation travels as a range, with no bytes, and is reserved again.
enum ckpt_map_kind {
    CKPT_MAP_PAGES = 0,       // `pages` pages of bytes follow
    CKPT_MAP_RESERVED = 1,    // nothing follows
};

struct ckpt_fd {
    uint32_t fd;
    uint32_t cloexec;
    uint32_t flags;
    uint32_t kind;
    uint64_t offset;
    uint32_t path_len;
    // WHICH struct fd this is, not just which number it sits at.
    //
    // Two descriptors that are the same object have to come back as the same
    // object: a shell and the child it forked share one struct fd for a
    // redirected file, and giving each its own on restore gives them
    // independent offsets -- the child's reads stop advancing the parent's
    // position, which is the bug `while read; done < file | ...` is made of.
    // The first record for an id describes it; every later one is a
    // CKPT_FD_REF naming it.
    uint32_t id;
    // CKPT_FD_PIPE: the inode both ends share (fs/pipe.c), so a pair held by
    // two different processes is rebuilt as ONE host pipe; and which end this
    // is. `offset` carries the number of bytes that were still in it, which
    // follow the record for a read end.
    union {
        struct {
            uint64_t pipe_inode;
            uint32_t pipe_write_end;
            uint32_t reserved;
        };
        // CKPT_FD_PTY_MASTER: who owned its slave, and the slave's mode --
        // what login or sshd set with fchown/fchmod once they knew whose
        // terminal it was. The pair is rebuilt by opening /dev/ptmx again, and
        // without this the slave comes back owned by root: the user's shell
        // can no longer open its own terminal, and neither can anything it
        // runs. pty_owner_known is 0 when the master had no slave to ask.
        struct {
            uint32_t pty_uid, pty_gid, pty_perms, pty_owner_known;
        };
    };
};

// ------------------------------------------------------------------ status

static lock_t ckpt_lock = LOCK_INITIALIZER;
static struct checkpoint_status ckpt_status;
static char ckpt_pending_path[PATH_MAX];
static bool ckpt_pending;
static bool ckpt_pending_halt;
// Who asked: the task that wrote to /proc/ish/checkpoint. See
// checkpoint_run_pending for why it matters which task takes the request.
static pid_t_ ckpt_pending_pid;
static char ckpt_session_path[PATH_MAX];

// A root is its data directory's host inode. Not its name: the app renames
// roots, and a renamed root is still the same files. Not its path either,
// which on a device carries the app container's UUID. The inode survives a
// rename, and a COPY never has it -- an import, an exported root brought back,
// a snapshot, a `cp -R` of the directory -- each is another filesystem that
// merely starts out equal, and a session resumed on it would carry on writing
// into files it never saw. fakefs tells a copied root from its original the
// same way (meta.db's db_inode, fs/fake-db.c).
uint64_t checkpoint_root_identity(const char *root_data_dir) {
    struct stat st;
    if (root_data_dir == NULL || stat(root_data_dir, &st) != 0)
        return 0;
    return (uint64_t) st.st_ino;
}

// The same for the root this machine is running on, read from its mount's own
// descriptor -- that data directory, opened (realfs_mount). And a name for it:
// the directory holding "data" for a fakefs root (roots/<name>/data), the
// directory itself otherwise.
static uint64_t ckpt_running_root(char *name, size_t name_size) {
    name[0] = '\0';
    struct mount *root = mount_find("");
    if (root == NULL)
        return 0;
    uint64_t id = 0;
    struct stat st;
    if (root->root_fd >= 0 && fstat(root->root_fd, &st) == 0)
        id = (uint64_t) st.st_ino;
    const char *src = root->source != NULL ? root->source : "";
    size_t len = strlen(src);
    while (len > 1 && src[len - 1] == '/')
        len--;
    if (len > 5 && strncmp(src + len - 5, "/data", 5) == 0)
        len -= 5;
    const char *base = src + len;
    while (base > src && base[-1] != '/')
        base--;
    snprintf(name, name_size, "%.*s", (int) (src + len - base), base);
    mount_release(root);
    return id;
}

int checkpoint_peek(const char *host_path, struct checkpoint_image_info *out) {
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(host_path, "rb");
    if (f != NULL)
        setvbuf(f, NULL, _IOFBF, 1 << 20);   // see checkpoint_save's note
    if (f == NULL)
        return errno_map();
    // fread directly rather than this file's rd(): peek sits above it, and a
    // header short read is simply "not an image" rather than an I/O policy.
    struct ckpt_header h;
    size_t got = fread(&h, 1, sizeof(h), f);
    fclose(f);
    if (got != sizeof(h))
        return _EINVAL;
    if (memcmp(h.magic, CKPT_MAGIC, sizeof(h.magic)) != 0)
        return _EINVAL;   // not one of ours at all
    out->version = h.version;
    out->abi = h.abi;
    out->tasks = h.n_tasks;
    out->pages = h.total_pages;
    // The header's hostname is not required to be terminated; the picker is
    // going to hand this straight to a string API.
    size_t n = sizeof(h.hostname) < sizeof(out->hostname) - 1
             ? sizeof(h.hostname) : sizeof(out->hostname) - 1;
    memcpy(out->hostname, h.hostname, n);
    out->hostname[n] = '\0';
    out->loadable = h.version == CKPT_VERSION && h.page_size == PAGE_SIZE;
    // Only a header of this version has these fields where this build reads
    // them; in any other it is whatever followed a shorter header.
    if (h.version == CKPT_VERSION) {
        out->root = h.root_identity;
        h.root_name[sizeof(h.root_name) - 1] = '\0';
        snprintf(out->root_name, sizeof(out->root_name), "%s", h.root_name);
    }
    return 0;
}

void checkpoint_get_status(struct checkpoint_status *out) {
    lock(&ckpt_lock, 0);
    *out = ckpt_status;
    unlock(&ckpt_lock);
}

static void ckpt_refuse(const char *fmt, ...) {
    va_list ap;
    lock(&ckpt_lock, 0);
    va_start(ap, fmt);
    vsnprintf(ckpt_status.last_refusal, sizeof(ckpt_status.last_refusal), fmt, ap);
    va_end(ap);
    // Traced as well as recorded. Under ISH_CHECKPOINT_DEBUG a refusal was the
    // one thing the checkpoint did NOT say: last_refusal is readable only from
    // inside the guest or from the app's own UI, and a device whose save runs
    // as the app is backgrounded has neither -- the reason went nowhere a
    // console capture could see it, and a refused save looked like a save that
    // had simply stopped.
    CKPT_TRACE("refused: %s\n", ckpt_status.last_refusal);
    unlock(&ckpt_lock);
}

// The build's own fingerprint. struct cpu_state travels as raw bytes -- it is
// hundreds of fields across four guest architectures and transcribing it would
// be a second copy to keep in step -- so an image from another build must be
// refused rather than reinterpreted. Its size plus the compile timestamp is
// enough: the same binary always agrees, and a rebuild never does.
static uint64_t ckpt_fingerprint(void) {
    static const char stamp[] = __DATE__ __TIME__;
    uint64_t h = 1469598103934665603ULL;   // FNV-1a
    for (const char *p = stamp; *p; p++) {
        h ^= (unsigned char) *p;
        h *= 1099511628211ULL;
    }
    h ^= sizeof(struct cpu_state);
    h *= 1099511628211ULL;
    h ^= sizeof(struct ckpt_task);
    return h;
}

// ------------------------------------------------------------------ freezer

// >0 while a freeze is in progress. One global rather than a per-task read,
// because kernel/calls.c consults it on EVERY syscall return: the common
// answer is "no" and it must cost a relaxed load and a branch.
static _Atomic int ckpt_freeze_active;
// Raised while a RESTORE holds its tasks, as distinct from a save's freeze.
//
// Both raise ckpt_freeze_active -- a restored task parks in the same place a
// checkpointed one does, which is the point. But the two want opposite things
// from a native program: a save needs it to describe itself, while a restore
// is putting state IN and has nothing to ask. Worse, the program being parked
// during a restore is one that has just been re-launched and may be a few
// instructions into its own startup, with none of the structures a dump reads
// built yet. That crashed on a device: a re-launched zsh reached its first
// syscall inside parseopts, parked, and was asked to serialise itself --
// aok_run_state_script read sigtrapped[SIGDEBUG] with sigtrapped still NULL,
// which is the 0x80 in the report.
static _Atomic int ckpt_restoring;
// The parking lot. A leaf lock -- nothing is ever taken under it.
static pthread_mutex_t ckpt_park_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ckpt_park_cond = PTHREAD_COND_INITIALIZER;

// A task that will never park, because it is already leaving.
//
// A zombie has no thread left to reach the parking place, and a task inside
// do_exit is not going to get there either. Waiting for one is waiting for
// something that cannot happen: the freeze times out, the checkpoint refuses,
// and it looks like a bug in the freezer. It is the ordinary case -- a
// `( sleep 1; ... ) &` whose child finishes while the image is being taken.
//
// They are also not written to the image. A zombie IS real state -- its parent
// may be about to wait for it -- and losing one turns that wait into a hang,
// so it is recorded as a zombie and recreated rather than dropped. A task
// mid-exit is not recorded at all: it has already run its last instruction.
static bool ckpt_task_is_leaving(struct task *t) {
    return t->zombie || t->exiting ||
        atomic_load_explicit(&t->exit_finished, memory_order_acquire);
}

// Set while this thread is inside a program's ckpt_dump. See
// checkpoint_native_park, which sets it.
static __thread bool ckpt_dumping;

bool checkpoint_freeze_pending(void) {
    if (atomic_load_explicit(&ckpt_freeze_active, memory_order_relaxed) == 0)
        return false;
    // A program describing itself FOR the freeze is doing the freeze's work,
    // and every wait that asks this question would otherwise end early on its
    // behalf -- and the shim re-issues an interrupted call, so the description
    // spins and never finishes, and the task never parks.
    if (ckpt_dumping)
        return false;
    return current != NULL &&
        atomic_load_explicit(&current->ckpt_freeze_wanted, memory_order_acquire);
}

void checkpoint_park_if_frozen(void) {
    if (atomic_load_explicit(&ckpt_freeze_active, memory_order_relaxed) == 0)
        return;
    if (current == NULL ||
            !atomic_load_explicit(&current->ckpt_freeze_wanted, memory_order_acquire))
        return;

    // No missed wakeup: the flag is re-checked under the mutex the thawer
    // signals with, the same shape as emu/memory.c's quiesce parking lot.
    pthread_mutex_lock(&ckpt_park_lock);
    atomic_store_explicit(&current->ckpt_frozen, true, memory_order_release);
    pthread_cond_broadcast(&ckpt_park_cond);
    while (atomic_load_explicit(&current->ckpt_freeze_wanted, memory_order_acquire))
        pthread_cond_wait(&ckpt_park_cond, &ckpt_park_lock);
    atomic_store_explicit(&current->ckpt_frozen, false, memory_order_release);
    pthread_mutex_unlock(&ckpt_park_lock);

    // Leave the freeze's wake behind. task_wake_for_freeze marks
    // wait_interrupted on every task, and ckpt_freeze_all marks it again, every
    // 2ms, on whichever task it finds not yet parked -- including one whose
    // wait already returned and is on its way here. Nothing consumes the mark
    // but the next wait_for, which returns EINTR on it before looking at
    // anything else. So the call re-executed after the thaw failed with an
    // EINTR no signal and no freeze stood behind: of 43 tasks parked for one
    // checkpoint, 31 still carried the mark when they left this lot, and
    // semop, msgrcv, flock and rt_sigsuspend each failed with EINTR in one run
    // or another, on arm64 as well as amd64.
    //
    // Safe to drop, because no wait is in progress here -- a task parks at
    // the top of its loop, a native program at a syscall checkpoint -- and
    // nothing real is lost with it: a signal sent during the freeze is still
    // in the pending set, which every wait checks as well.
    __atomic_store_n(&current->wait_interrupted, false, __ATOMIC_RELEASE);
}

// Set while this thread is inside a program's ckpt_dump.
//
// The dump is the program describing itself, and describing itself means
// making syscalls -- zsh's emitters write to a descriptor. Every syscall a
// native program makes goes through native_checkpoint(), which comes straight
// back here, which would call the dump again: the guard below is what stops
// that from being an infinite recursion off the end of the thread's stack,
// which took the whole app down rather than the shell.
//
// __thread rather than a task field, because it is a property of THIS call
// stack and nothing else can see it. Declared above checkpoint_freeze_pending,
// which reads it too.

void checkpoint_native_park(void) {
    // Already describing itself: neither dump again nor park. Parking here
    // would stop the program halfway through producing the very state the
    // freeze is waiting for.
    if (ckpt_dumping)
        return;

    if (atomic_load_explicit(&ckpt_freeze_active, memory_order_relaxed) == 0)
        return;
    if (current == NULL ||
            !atomic_load_explicit(&current->ckpt_freeze_wanted, memory_order_acquire))
        return;

    // DESCRIBE YOURSELF FIRST, on this thread, because this is the only thread
    // the program's state exists on -- everything a native shell holds is
    // __thread (tools/dash-tls-rewrite.py and its bash/zsh predecessors). The
    // writer runs on the checkpointing task's thread and could not reach any
    // of it.
    //
    // A program with no ckpt_dump leaves this NULL, and ckpt_check_scope has
    // already refused on its behalf -- so reaching here with nothing is the
    // freeze that was allowed to start, not a silent loss.
    // Never during a restore: see ckpt_restoring. The task still parks below,
    // which is all the restore actually wants from it.
    const struct native_program *prog = native_program_running(current);
    if (atomic_load_explicit(&ckpt_restoring, memory_order_acquire) == 0 &&
            prog != NULL && prog->ckpt_dump != NULL &&
            // A stand-in comes back as its wait, so its program's state would
            // never be read -- and after a restore this thread is not running
            // that program at all, so there is no state here to describe.
            current->native_standin_child == 0 &&
            current->ckpt_native_state == NULL) {
        ckpt_dumping = true;
        current->ckpt_native_state = prog->ckpt_dump();
        ckpt_dumping = false;
        CKPT_TRACE("native park: pid %d described itself in %zu bytes\n",
                   current->pid, current->ckpt_native_state != NULL
                   ? strlen(current->ckpt_native_state) : 0);
    }

    checkpoint_park_if_frozen();
}

// Ask every task but this one to reach a boundary and stop there.
//
// Returns 0 with every task parked, or _EBUSY with `blame` naming the one that
// would not stop. Thaws on failure, so a refusal leaves the guest exactly as
// it was.
// Where a task that would not park actually IS, on the host. The frames come
// from kernel/task.c, which can see the Mach headers: including them here made
// PAGE_SIZE a runtime variable and turned a later static array into a VLA.
static void ckpt_trace_host_backtrace(struct task *t) {
    uintptr_t frames[32];
    unsigned n = task_host_backtrace(t, frames, 32);
    for (unsigned i = 0; i < n; i++) {
        Dl_info info;
        if (dladdr((void *) frames[i], &info) && info.dli_fname != NULL) {
            const char *img = strrchr(info.dli_fname, '/');
            CKPT_TRACE("  host frame %2u: %s+%#lx %s\n", i,
                       img != NULL ? img + 1 : info.dli_fname,
                       (unsigned long) (frames[i] - (uintptr_t) info.dli_fbase),
                       info.dli_sname != NULL ? info.dli_sname : "?");
        } else {
            CKPT_TRACE("  host frame %2u: %#lx\n", i, (unsigned long) frames[i]);
        }
    }
}

static int ckpt_freeze_all(unsigned timeout_ms, char *blame, size_t blame_size) {
    struct task_snapshot snap = {0};
    if (task_snapshot_collect(&snap, false) < 0)
        return _EAGAIN;

    atomic_fetch_add_explicit(&ckpt_freeze_active, 1, memory_order_acq_rel);
    for (unsigned i = 0; i < snap.count; i++) {
        struct task *t = snap.tasks[i];
        if (t == current)
            continue;
        atomic_store_explicit(&t->ckpt_freeze_wanted, true, memory_order_release);
    }
    // Woken only AFTER every flag is set. A task woken while a sibling's flag
    // was still clear could run on, block again in something the freezer has
    // already passed, and never be asked a second time.
    for (unsigned i = 0; i < snap.count; i++) {
        struct task *t = snap.tasks[i];
        if (t != current)
            task_wake_for_freeze(t);
    }

    int err = 0;
    struct task *stuck = NULL;
    for (unsigned spins = 0; ; spins++) {
        stuck = NULL;
        for (unsigned i = 0; i < snap.count; i++) {
            struct task *t = snap.tasks[i];
            if (t == current)
                continue;
            if (ckpt_task_is_leaving(t))
                continue;   // see ckpt_task_is_leaving
            if (!atomic_load_explicit(&t->ckpt_frozen, memory_order_acquire)) {
                stuck = t;
                break;
            }
        }
        if (stuck == NULL)
            break;
        if (spins * 2 >= timeout_ms) {
            err = _EBUSY;
            break;
        }
        // Poked again on every pass. One wake can be lost -- a task that was
        // between waits when the first arrived takes the next one instead --
        // and a freeze that gives up because of a single dropped poke would
        // be a flake rather than a limit.
        task_wake_for_freeze(stuck);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    if (err != 0 && stuck != NULL && blame != NULL) {
        // WHICH syscall, when there is a guest register file to read it from.
        // The pid alone is not enough to act on: every untaught blocking path
        // looks identical from here, and the fix for each is in a different
        // file (kernel/calls.c task_blocked_syscall has the story).
        bool native = stuck->native_exec != NULL || stuck->native_cmdline != NULL;
        const char *abi = NULL;
        long nr = native ? -1 : task_blocked_syscall(stuck, &abi);
        if (nr >= 0)
            snprintf(blame, blame_size, "pid %d (%s) did not reach a syscall "
                     "boundary within %ums (blocked in %s syscall %ld)",
                     stuck->pid, stuck->comm, timeout_ms, abi, nr);
        else
            snprintf(blame, blame_size, "pid %d (%s) did not reach a syscall "
                     "boundary within %ums", stuck->pid, stuck->comm, timeout_ms);
        if (ckpt_debug()) {
            CKPT_TRACE("host backtrace of pid %d (%s), the task that would not park:\n",
                       stuck->pid, stuck->comm);
            ckpt_trace_host_backtrace(stuck);
        }
    }
    if (err != 0) {
        for (unsigned i = 0; i < snap.count; i++)
            atomic_store_explicit(&snap.tasks[i]->ckpt_freeze_wanted, false,
                                  memory_order_release);
        pthread_mutex_lock(&ckpt_park_lock);
        pthread_cond_broadcast(&ckpt_park_cond);
        pthread_mutex_unlock(&ckpt_park_lock);
        atomic_fetch_sub_explicit(&ckpt_freeze_active, 1, memory_order_acq_rel);
    }
    task_snapshot_release(&snap);
    return err;
}

static void ckpt_thaw_all(void) {
    struct task_snapshot snap = {0};
    if (task_snapshot_collect(&snap, false) == 0) {
        for (unsigned i = 0; i < snap.count; i++) {
            atomic_store_explicit(&snap.tasks[i]->ckpt_freeze_wanted, false,
                                  memory_order_release);
            // And drop what each native program said about itself.
            //
            // checkpoint_native_park fills this in per task, on that task's own
            // thread, and it is guarded by `== NULL` so a program describes
            // itself ONCE. Nothing cleared it: the guest-initiated path frees
            // only `current`'s copy, and the app's external save frees none at
            // all. So every native that parked kept its first description
            // forever, and the SECOND save of a session wrote the state from
            // the FIRST one -- a restored shell then came back holding a
            // snapshot of itself from a suspend ago.
            //
            // Reported as: resume a session, open more windows, suspend again,
            // and the next resume has nothing. A shell restored from a stale,
            // mismatched state exits on startup, and a workspace window whose
            // session ends closes itself, so the windows vanish rather than
            // arriving wrong.
            //
            // The guest path already had the rule right in its own comment --
            // "it describes a moment that has passed, and leaving it would have
            // the next checkpoint write a stale state" -- it simply could not
            // reach the other tasks. Here it can: this is where every parked
            // task is released, on every path, success or refusal.
            //
            // Before the broadcast below, so the owning threads are still
            // parked and none of them can be reading it.
            free(snap.tasks[i]->ckpt_native_state);
            snap.tasks[i]->ckpt_native_state = NULL;
        }
        task_snapshot_release(&snap);
    }
    pthread_mutex_lock(&ckpt_park_lock);
    pthread_cond_broadcast(&ckpt_park_cond);
    pthread_mutex_unlock(&ckpt_park_lock);
    atomic_fetch_sub_explicit(&ckpt_freeze_active, 1, memory_order_acq_rel);
}

// ------------------------------------------------------------------ writing

struct ckpt_writer {
    FILE *f;
    int err;
};

static void wr(struct ckpt_writer *w, const void *p, size_t n) {
    if (w->err != 0 || n == 0)
        return;
    if (fwrite(p, 1, n, w->f) != n)
        w->err = errno_map();
}

static int rd(FILE *f, void *p, size_t n) {
    if (n == 0)
        return 0;
    if (fread(p, 1, n, f) != n)
        return feof(f) ? _EINVAL : errno_map();
    return 0;
}

// Can this guest be checkpointed at all? Answered before anything is written,
// so a refusal costs nothing and names its reason.
static int ckpt_check_scope(void) {
    struct task_snapshot snap = {0};
    if (task_snapshot_collect(&snap, false) < 0) {
        ckpt_refuse("could not enumerate tasks");
        return _EAGAIN;
    }
    int err = 0;
    if (snap.count == 0) {
        task_snapshot_release(&snap);
        ckpt_refuse("there is no guest running");
        return _ESRCH;
    }
    // A native program used to refuse the whole checkpoint here, because its
    // C stack on a host thread cannot be serialised and only zsh knows how to
    // describe itself. That was the wrong trade, and it is worth being plain
    // about why: the alternative to a degraded restore is NOT a perfect one,
    // it is no restore at all. iOS kills the app either way. Refusing bought
    // "nothing came back looking wrong" at the price of nothing coming back.
    //
    // Nothing technical required it. The freezer already parks a native
    // program whether or not it has a ckpt_dump (checkpoint_native_park), the
    // image already carries its name, argv, environment and descriptors, and
    // ckpt_dispatch_native already starts a program with no state -- it only
    // attaches the state pipe when there IS one. So the pieces for saving
    // these were all present; the refusal was policy sitting on top of them.
    //
    // What is genuinely lost is the program's own place: a re-launched
    // program starts from its command line again, not from where it was. For
    // a shell at a prompt that is exactly right. For one part way through a
    // script it means the script runs again from the top, which is why this
    // is REPORTED (ckpt_status.natives_note) rather than done quietly.
    task_snapshot_release(&snap);
    return err;
}

// Native programs in the save being written that had no state to give, so the
// restore will start them again from their command line. Accumulated as the
// task records are written and published into ckpt_status when the image
// lands. Guarded by ckpt_lock like the rest of the status.
static unsigned long ckpt_natives_restarted;
static char ckpt_natives_note[192];

// What the restore under way could not put back as it was, read through
// checkpoint_get_restore_note once it succeeds. The guest runs on regardless, and
// on a device this is the only place such a degradation can be read -- the
// printks that used to be the whole report go to fd 555.
static char ckpt_restore_note[256];

static void ckpt_note_restore(uint32_t pid, uint32_t fd, const char *why) {
    size_t len = strlen(ckpt_restore_note);
    if (len + 1 >= sizeof(ckpt_restore_note))
        return;
    snprintf(ckpt_restore_note + len, sizeof(ckpt_restore_note) - len,
             "%spid %u fd %u: %s", len != 0 ? "; " : "", pid, fd, why);
}

void checkpoint_get_restore_note(char *out, size_t size) {
    if (out == NULL || size == 0)
        return;
    lock(&ckpt_lock, 0);
    snprintf(out, size, "%s", ckpt_status.restored ? ckpt_restore_note : "");
    unlock(&ckpt_lock);
}

static void ckpt_note_restarted_native(const char *name) {
    ckpt_natives_restarted++;
    // Names, deduplicated: four shells all called bash should read "bash",
    // not "bash, bash, bash, bash".
    size_t len = strlen(ckpt_natives_note);
    if (len != 0) {
        if (strstr(ckpt_natives_note, name) != NULL)
            return;
        if (len + 2 < sizeof(ckpt_natives_note))
            len += (size_t) snprintf(ckpt_natives_note + len,
                                     sizeof(ckpt_natives_note) - len, ", ");
    }
    snprintf(ckpt_natives_note + len, sizeof(ckpt_natives_note) - len, "%s", name);
}

// What is still in a pipe, taken out and PUT BACK.
//
// A checkpoint is a copy: the guest carries on afterwards and must not notice,
// so bytes read out here are written straight back in. Safe only because the
// machine is frozen -- nobody else is at either end -- and bounded by the pipe
// buffer, so the write can never block on a pipe we have just emptied.
//
// FIONREAD first rather than reading until EAGAIN: it says exactly how much is
// there, so there is no need to make the descriptor non-blocking and no window
// in which it is.
static int ckpt_pipe_drain(struct fd *fd, char **out, uint64_t *len) {
    *out = NULL;
    *len = 0;
    int avail = 0;
    if (ioctl(fd->real_fd, FIONREAD, &avail) < 0 || avail <= 0)
        return 0;
    char *buf = malloc((size_t) avail);
    if (buf == NULL)
        return _ENOMEM;
    ssize_t got = read(fd->real_fd, buf, (size_t) avail);
    if (got <= 0) {
        free(buf);
        return 0;
    }
    ssize_t put = write(fd->real_fd, buf, (size_t) got);
    if (put != got) {
        // Cannot happen on a pipe we have just emptied, and if it somehow does
        // the guest has lost data -- say so rather than carry on quietly.
        printk("checkpoint: pipe %llu lost %zd bytes putting them back\n",
               (unsigned long long) fd->stat.inode, got - (put < 0 ? 0 : put));
    }
    *out = buf;
    *len = (uint64_t) got;
    return 0;
}

// One descriptor, gathered under files->lock and described afterwards.
struct ckpt_saved_fd {
    struct fd *fd;
    unsigned num;
    unsigned cloexec;
    int kind;
    uint64_t offset;
    uint32_t id;
    bool first;              // this record describes the object, not a ref to it
    char *pipe_bytes;        // CKPT_FD_PIPE read end: what was still in it
    uint64_t pipe_len;
    struct sock_ckpt_desc sock;   // CKPT_FD_SOCKET: how to build it again
    char path[MAX_PATH + 1];
};

// Every distinct struct fd in the image, in the order first seen. The index is
// the id a record carries; a second sighting of the same pointer -- in this
// process or another -- becomes a CKPT_FD_REF to it.
struct ckpt_fd_ids {
    struct fd **fds;
    uint32_t count, cap;
};

// Returns the id, and sets *first to whether this is the first sighting.
static uint32_t ckpt_fd_id(struct ckpt_fd_ids *ids, struct fd *fd, bool *first) {
    for (uint32_t i = 0; i < ids->count; i++) {
        if (ids->fds[i] == fd) {
            *first = false;
            return i;
        }
    }
    if (ids->count == ids->cap) {
        uint32_t cap = ids->cap ? ids->cap * 2 : 32;
        struct fd **f = realloc(ids->fds, cap * sizeof(*f));
        if (f == NULL) {
            *first = true;
            return UINT32_MAX;   // caller treats it as "describe it again"
        }
        ids->fds = f;
        ids->cap = cap;
    }
    ids->fds[ids->count] = fd;
    *first = true;
    return ids->count++;
}

// Where a descriptor is positioned, asked rather than read off struct fd.
//
// fd->offset is not the answer for a realfs or fakefs file: the position lives
// in the HOST descriptor, and fd->offset is only maintained by the filesystems
// that have nowhere else to keep it. Reading it gave every restored file an
// offset of 0, so a shell that had read one line of /etc/services came back
// about to read that same line again -- which is exactly the thing this
// feature exists to get right.
static uint64_t ckpt_fd_offset(struct fd *fd) {
    if (fd->ops != NULL && fd->ops->lseek != NULL) {
        off_t_ pos = fd->ops->lseek(fd, 0, LSEEK_CUR);
        if (pos >= 0)
            return (uint64_t) pos;
    }
    return fd->offset;
}

// One descriptor, classified. Returns the kind, or 0 with a refusal recorded.
// A terminal record's identity: its driver major and its number, as one value
// in the record's offset field. 0 is "not known" -- a descriptor with no tty
// behind it.
static uint64_t ckpt_tty_identity(int type, int num) {
    return ((uint64_t) (uint32_t) type << 32) | (uint32_t) num;
}

static int ckpt_classify_fd(int num, struct fd *fd, char *path, size_t path_size) {
    // Descriptors with no file behind them, identified by what they ARE rather
    // than by what they look like. Before this they reached the standard-
    // stream rule below: struct fd is zero-initialised, so every one of them
    // had real_fd 0. A restored dbus-daemon got /dev/null where its epoll set
    // had been, epoll_pwait failed at once for ever, and the daemon spun at a
    // full core without serving the bus -- and every login waits on the bus.
    if (epoll_fd_is(fd))
        return CKPT_FD_EPOLL;
    if (eventfd_fd_is(fd))
        return CKPT_FD_EVENTFD;
    if (signalfd_fd_is(fd))
        return CKPT_FD_SIGNALFD;
    if (timerfd_fd_is(fd))
        return CKPT_FD_TIMERFD;
    if (inotify_fd_is(fd))
        return CKPT_FD_INOTIFY;
    if (pidfd_fd_is(fd))
        return CKPT_FD_PIDFD;
    if (memfd_fd_is(fd))
        return CKPT_FD_MEMFD;

    const char *family = fd->ops != NULL && fd->ops->name != NULL
            ? fd->ops->name : "unknown";

    path[0] = '\0';
    // A terminal comes back by being RE-OPENED rather than restored --
    // sockrestart's model, and the only honest one for a terminal belonging to
    // a process that no longer exists. Its PATH travels so it is re-opened as
    // the same one: the CLI's console is /dev/tty1 and the app's is
    // /dev/console, and a session that came back on the wrong one would be
    // talking to a terminal nobody is looking at.
    if ((fd->ops != NULL && fd->ops->name != NULL &&
                strcmp(fd->ops->name, "devpts") == 0) || fd_tty(fd) != NULL) {
        if (generic_getpath(fd, path) < 0 || path[0] != '/')
            path[0] = '\0';
        // A pty whose MASTER a guest process holds is the image's own, not the
        // terminal this run is looking at. tty->type is the driver's major.
        struct tty *terminal = fd_tty(fd);
        if (terminal != NULL) {
            if (terminal->type == TTY_PSEUDO_MASTER_MAJOR)
                return CKPT_FD_PTY_MASTER;
            if (terminal->type == TTY_PSEUDO_SLAVE_MAJOR &&
                    pty_master_is_open(terminal->num))
                return CKPT_FD_PTY_SLAVE;
        }
        return CKPT_FD_TTY;
    }
    // The standard streams as the entry point handed them over. On the CLI
    // with output piped these are host descriptors wrapped in a struct fd
    // (kernel/init.c's create_piped_stdio) -- a pipe or a socket whose other
    // end is a process on the Mac. There is nothing to serialise and nothing
    // that would mean anything on the way back, so they are re-attached like
    // the tty: to whatever the next run is handed.
    //
    // Identified by what the descriptor IS, not by the guest number it sits
    // at, and that is not pedantry: a shell moves its saved stdin to fd 10 for
    // the duration of a redirection (dash's to_upper_fd), so the very first
    // checkpoint taken from inside `while read; done < file` refused on "fd 10
    // is a special file". It is the same stream, wherever the guest is holding
    // it.
    //
    // Nor by the HOST number it wraps, which was the test until a restored
    // session had to be saved again: the restore wraps a copy of the new run's
    // stream, above 2 (open_host_stdio_copy), so the resumed shell's stdin was
    // "a special file on realfs with no restore rule" and every save after a
    // resume refused. A number in 0..2 was never proof either: once the guest
    // has closed its last reference to one of the host's streams, the next
    // host open gets that number, and a guest pipe could pass. fd->host_stdio
    // is set where the descriptor is made, and says which stream it is.
    if (fd->ops == &realfs_fdops && fd->host_stdio != 0)
        return CKPT_FD_STDIO;
    // An ordinary character device -- /dev/null, /dev/zero, /dev/urandom.
    // These have a stable path and no state, so they come back by being
    // opened again, and they are NOT the terminal case above: treating
    // /dev/null as a console gave a backgrounded `sleep &` a tty for stdin,
    // which is the opposite of what putting it on /dev/null was for.
    if (S_ISCHR(fd->type) && generic_getpath(fd, path) >= 0 && path[0] == '/')
        return CKPT_FD_CHR;
    // A pipe. AOK's pipes are HOST pipes with a struct fd over each end
    // (fs/pipe.c), so what has to travel is the pairing, the direction and
    // whatever bytes are still in flight -- not the object, which cannot
    // outlive the process that owns it.
    // A NAMED pipe -- a FIFO node on a filesystem, whose buffer lives with the
    // node -- as opposed to a pipe(2) pair, which is a host pipe. This one had
    // no rule and, with real_fd 0, reached the standard-stream rule instead:
    // sysvinit's /run/initctl came back as /dev/null, so every telinit and
    // shutdown request after a restore went nowhere.
    if (S_ISFIFO(fd->type) && fd->ops != &realfs_fdops &&
            generic_getpath(fd, path) >= 0 && path[0] == '/')
        return CKPT_FD_FIFO;
    if (S_ISFIFO(fd->type) && fd->ops == &realfs_fdops && fd->real_fd >= 0 &&
            fd->stat.inode != 0)
        return CKPT_FD_PIPE;
    // A socket. The host object belongs to this process and cannot outlive it
    // -- on iOS it does not even outlive a suspension -- so what travels is a
    // description complete enough to BUILD one again. That is sockrestart's
    // model, and the only one available. fs/sock_ckpt.h carries the rules,
    // including what becomes of a connection that cannot be resumed.
    if (S_ISSOCK(fd->type) && fd->ops != NULL && fd->ops->name != NULL &&
            strcmp(fd->ops->name, "socket") == 0)
        return CKPT_FD_SOCKET;
    if (!S_ISREG(fd->type) && !S_ISDIR(fd->type)) {
        ckpt_refuse("fd %d is a %s on %s with no restore rule",
                    num, S_ISFIFO(fd->type) ? "named pipe" :
                         S_ISSOCK(fd->type) ? "socket" : "special file",
                    family);
        return 0;
    }
    int err = generic_getpath(fd, path);
    if (err < 0 || path[0] != '/') {
        ckpt_refuse("fd %d on %s has no path to re-open", num, family);
        return 0;
    }
    (void) path_size;
    return S_ISDIR(fd->type) ? CKPT_FD_DIR : CKPT_FD_FILE;
}

// Walk the address space and hand each contiguous run of like-flagged mapped
// pages to `emit`. Contiguity is by FLAGS as well as by address: restoring a
// run means one pt_map_nothing, and pt_map_nothing takes one flag word.
static int ckpt_for_each_map(struct mem *mem,
        int (*emit)(void *ctx, page_t start, pages_t pages, unsigned flags),
        void *ctx) {
    // mem_next_page, not page++, and that distinction is the difference
    // between finishing and not. An arm64 guest's page_limit is the top of a
    // 64-bit address space -- billions of pages, almost all of them holes --
    // and a linear scan of it does not return. mem_next_page is the SPARSE
    // walk /proc/<pid>/maps uses for the same reason: from the last page of a
    // page-table leaf it jumps to the next leaf that exists.
    page_t page = 0;
    while (page < mem->page_limit) {
        while (page < mem->page_limit && mem_pt(mem, page) == NULL)
            mem_next_page(mem, &page);
        if (page >= mem->page_limit)
            break;
        page_t start = page;
        unsigned flags = mem_pt(mem, page)->flags;
        while (page < mem->page_limit) {
            struct pt_entry *e = mem_pt(mem, page);
            if (e == NULL || e->flags != flags)
                break;
            page++;
            // A run has to stop at the end of a leaf as well: past it the
            // next page may be billions away, and the pages between are not
            // part of this mapping.
            if (page < mem->page_limit && mem_pt(mem, page) == NULL)
                break;
        }
        int err = emit(ctx, start, page - start, flags);
        if (err < 0)
            return err;
    }
    return 0;
}

// And each reservation, which has no entries for the walk above to find.
// Caller holds the mem lock.
static int ckpt_for_each_reservation(struct mem *mem,
        int (*emit)(void *ctx, page_t start, pages_t pages, unsigned flags),
        void *ctx) {
    for (unsigned i = 0; i < mem->lazy_count; i++) {
        struct mem_lazy_map l = mem->lazy[i];
        if (l.start >= l.end)
            continue;
        // Without mlockall's mark: the image does not carry an entry's lock
        // either, so a restored process holds no locks at all rather than
        // locks on only the pages that were still reserved.
        int err = emit(ctx, l.start, l.end - l.start, l.flags & ~MEM_LAZY_LOCKED);
        if (err < 0)
            return err;
    }
    return 0;
}

struct ckpt_count_ctx { uint32_t maps; uint64_t pages; };
static int ckpt_count_map(void *vctx, page_t UNUSED(start), pages_t pages,
        unsigned UNUSED(flags)) {
    struct ckpt_count_ctx *c = vctx;
    c->maps++;
    c->pages += pages;
    return 0;
}

static int ckpt_count_reservation(void *vctx, page_t UNUSED(start), pages_t UNUSED(pages),
        unsigned UNUSED(flags)) {
    struct ckpt_count_ctx *c = vctx;
    c->maps++;      // a record, but no pages in the image
    return 0;
}

struct ckpt_emit_ctx { struct ckpt_writer *w; struct mem *mem; };

static int ckpt_emit_reservation(void *vctx, page_t start, pages_t pages, unsigned flags) {
    struct ckpt_emit_ctx *c = vctx;
    struct ckpt_map m = {
        .start = (uint64_t) start << PAGE_BITS,
        .pages = pages,
        .flags = flags,
        .kind = CKPT_MAP_RESERVED,
    };
    CKPT_TRACE("  save reservation %#llx +%llu pages flags %#x\n",
               (unsigned long long) m.start, (unsigned long long) pages, flags);
    wr(c->w, &m, sizeof(m));
    return c->w->err;
}

static int ckpt_emit_map(void *vctx, page_t start, pages_t pages, unsigned flags) {
    struct ckpt_emit_ctx *c = vctx;
    struct ckpt_map m = {
        .start = (uint64_t) start << PAGE_BITS,
        .pages = pages,
        .flags = flags,
        .kind = CKPT_MAP_PAGES,
    };
    wr(c->w, &m, sizeof(m));
    for (pages_t i = 0; i < pages && c->w->err == 0; i++) {
        guest_addr_t addr = ((guest_addr_t) (start + i)) << PAGE_BITS;
        // MEM_READ, so a page the pager has evicted is faulted back in rather
        // than written out as a hole. That is the whole point of taking the
        // checkpoint through the ordinary read path: swap is not a second
        // place the image has to look.
        const char *p = mem_ptr(c->mem, addr, MEM_READ);
        if (p == NULL) {
            // A PROT_NONE guard page is mapped and unreadable, and that is
            // normal rather than an error -- a stack guard, or the gap
            // pthreads leaves. Write zeroes; the flags travel separately and
            // put the protection back.
            static const char zero[PAGE_SIZE];
            wr(c->w, zero, PAGE_SIZE);
        } else {
            wr(c->w, p, PAGE_SIZE);
        }
    }
    return c->w->err;
}

// One task, written in full: its record, its register file, its signal
// dispositions and limits, its address space, its descriptors.
//
// `current` is REPOINTED at the task for the duration. That is an established
// move in this tree (kernel/init.c does it in three places) and it is what
// lets every helper below -- generic_getpath, mem_ptr, the filesystem's lseek
// -- be the ordinary one rather than a second copy that takes an explicit
// task. Safe because every other task is frozen; unsafe the moment that stops
// being true.
// One task, written in full: its record, its register file, its signal
// dispositions and limits, its address space, its descriptors.
//
// `current` is REPOINTED at the task for the duration. That is an established
// move in this tree (kernel/init.c does it in three places) and it is what
// lets every helper below -- generic_getpath, mem_ptr, the filesystem's lseek
// -- be the ordinary one rather than a second copy that takes an explicit
// task. Safe because every other task is frozen; unsafe the moment that stops
// being true.
//
// Three shapes, sharing the descriptor half: an ordinary task, a NATIVE task
// (no address space to photograph, a self-description instead), and a zombie
// (a status and nothing else).
// ---- tmpfs contents ------------------------------------------------------
//
// A tmpfs is RAM with a path on it, and nothing outside this image remembers
// it. A restore that leaves it out brings the session back onto an empty
// /run, /tmp and /dev/shm, which is not a cosmetic gap: the descriptor that
// first exposed it was a stdin on /run/pacct_source, ENOENT the moment the
// restore tried to reopen it, and every AF_UNIX name sock_ckpt_rebuild has to
// bind lives in the same place. Handing a session back its processes but not
// the files those processes wrote is not a restore.
//
// The section sits between the header and the first task record, because the
// task records NAME these paths: the tree has to be back before any
// descriptor is reopened against it.
//
// Only mounts whose fs is exactly &tmpfs travel. devtmpfs, cgroupfs and
// cgroup2fs share this file's code but not its nature -- their contents are
// generated by the kernel at mount time, and putting a stale copy back would
// fight whatever this boot just built. A bind is skipped because its origin
// carries the data, and a detached or lazily-unmounted mount has no path to
// restore onto.
#define CKPT_TMPFS_MAX_FILE  (64ull << 20)
#define CKPT_TMPFS_MAX_TOTAL (256ull << 20)
#define CKPT_TMPFS_MAX_DEPTH 64
#define CKPT_TMPFS_MAX_MOUNTS 16

// The mount itself, then its contents. How it was MADE travels too: the
// restore has to put the tmpfs back before it fills it, and a tmpfs mounted
// with different options is a different filesystem.
struct ckpt_tmpfs_mount {
    uint32_t point_len;      // path, source and info bytes follow, in that
    uint32_t source_len;     // order, none of them terminated
    uint32_t info_len;
    int32_t flags;
    uint32_t n_entries;
    // Whether the save could actually READ this mount. Zero means it could
    // not, and the restore must then leave the mount point alone entirely:
    // mounting an empty tmpfs over it would HIDE whatever the rootfs has
    // underneath, which is strictly worse than not restoring it. An empty
    // tmpfs the guest really had is walked_ok with n_entries 0, and does get
    // mounted, because that is the state the session was in.
    uint32_t walked_ok;
};

struct ckpt_tmpfs_entry {
    uint32_t path_len;       // relative to the mount point, no leading slash
    uint32_t mode;           // st_mode, type bits included
    uint32_t uid, gid;
    uint64_t rdev;
    uint64_t data_len;       // regular file: contents. symlink: target. else 0
    int64_t atime, mtime;
    uint32_t atime_nsec, mtime_nsec;
};

struct ckpt_tmpfs_ctx {
    struct ckpt_writer *w;
    size_t point_len;        // the mount-point prefix every path here carries
    uint32_t n_entries;
    uint64_t budget;         // content bytes left for the whole image
    bool walked_ok;          // the mount point itself opened and listed
};

// One entry: fixed record, then the relative path, then the payload. The
// payload is the contents for a regular file and the target for a symlink;
// everything else is fully described by its mode.
static void ckpt_tmpfs_emit(struct ckpt_tmpfs_ctx *c, const char *abs,
        const struct statbuf *st, const char *data, uint64_t data_len) {
    const char *rel = abs + c->point_len;
    while (*rel == '/')
        rel++;
    struct ckpt_tmpfs_entry e = {
        .path_len = (uint32_t) strlen(rel),
        .mode = st->mode,
        .uid = st->uid,
        .gid = st->gid,
        .rdev = st->rdev,
        .data_len = data_len,
        .atime = st->atime, .atime_nsec = st->atime_nsec,
        .mtime = st->mtime, .mtime_nsec = st->mtime_nsec,
    };
    wr(c->w, &e, sizeof(e));
    wr(c->w, rel, e.path_len);
    if (data_len > 0)
        wr(c->w, data, (size_t) data_len);
    c->n_entries++;
}

// A regular file's contents. A file too big to carry is left out and SAID so,
// rather than truncated: half a file restored as if it were whole is worse
// than one that is honestly missing.
static int ckpt_tmpfs_slurp(struct ckpt_tmpfs_ctx *c, const char *abs,
        uint64_t size, char **out, uint64_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (size == 0)
        return 0;
    if (size > CKPT_TMPFS_MAX_FILE || size > c->budget) {
        printk("WARNING: checkpoint: %s is %llu bytes; leaving it out of the "
               "image\n", abs, (unsigned long long) size);
        return _E2BIG;
    }
    struct fd *fd = generic_open(abs, O_RDONLY_, 0);
    if (IS_ERR(fd))
        return (int) PTR_ERR(fd);
    char *buf = malloc((size_t) size);
    if (buf == NULL) {
        fd_close(fd);
        return _ENOMEM;
    }
    uint64_t got = 0;
    int err = 0;
    while (got < size) {
        ssize_t n = fd->ops->read(fd, buf + got, (size_t) (size - got));
        if (n < 0) { err = (int) n; break; }
        if (n == 0) break;      // shorter than it said: carry what is there
        got += (uint64_t) n;
    }
    fd_close(fd);
    if (err < 0) {
        free(buf);
        return err;
    }
    *out = buf;
    *out_len = got;
    return 0;
}

// Pre-order, so a directory is always emitted before anything inside it and
// the restore can create the tree in the order it reads it.
static int ckpt_tmpfs_walk(struct ckpt_tmpfs_ctx *c, char *path, size_t len,
        unsigned depth) {
    if (depth > CKPT_TMPFS_MAX_DEPTH) {
        printk("WARNING: checkpoint: %s is nested deeper than %u; stopping "
               "there\n", path, CKPT_TMPFS_MAX_DEPTH);
        return 0;
    }
    struct fd *dir = generic_open(path, O_RDONLY_, 0);
    if (IS_ERR(dir)) {
        // A subdirectory can vanish under a walk of a live filesystem, and
        // stepping over one is right. The MOUNT POINT not opening is a
        // different thing -- the whole tmpfs is then missing from the image --
        // so it is said out loud. It does NOT fail the save: a session is
        // worth more than a /run, and the same argument that made a bad
        // descriptor degrade to /dev/null rather than refuse the restore
        // applies here. What is not acceptable is doing it quietly, which is
        // how three empty mounts reached a device restore looking like a
        // success.
        printk("WARNING: checkpoint: %s could not be read (%d); %s\n", path,
               -(int) PTR_ERR(dir), depth == 0
               ? "that filesystem is not in the image, and a restore will "
                 "leave its mount point alone rather than cover it"
               : "it is not in the image");
        return 0;
    }
    if (dir->ops->readdir == NULL) {
        fd_close(dir);
        return 0;
    }
    if (depth == 0)
        c->walked_ok = true;
    if (dir->ops->readdir_begin != NULL)
        dir->ops->readdir_begin(dir);

    int err = 0;
    for (;;) {
        struct dir_entry ent;
        int res = dir->ops->readdir(dir, &ent);
        if (res <= 0)
            break;
        if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0)
            continue;
        size_t nlen = strlen(ent.name);
        if (len + 1 + nlen >= MAX_PATH)
            continue;
        path[len] = '/';
        memcpy(path + len + 1, ent.name, nlen + 1);
        size_t clen = len + 1 + nlen;

        struct statbuf st;
        if (generic_statat(AT_PWD, path, &st, AT_SYMLINK_NOFOLLOW_) >= 0) {
            if (S_ISDIR(st.mode)) {
                ckpt_tmpfs_emit(c, path, &st, NULL, 0);
                err = ckpt_tmpfs_walk(c, path, clen, depth + 1);
            } else if (S_ISLNK(st.mode)) {
                char target[MAX_PATH];
                ssize_t n = generic_readlinkat(AT_PWD, path, target,
                                               sizeof(target));
                if (n > 0)
                    ckpt_tmpfs_emit(c, path, &st, target, (uint64_t) n);
            } else if (S_ISREG(st.mode)) {
                char *data = NULL;
                uint64_t dlen = 0;
                if (ckpt_tmpfs_slurp(c, path, st.size, &data, &dlen) == 0) {
                    ckpt_tmpfs_emit(c, path, &st, data, dlen);
                    c->budget -= dlen;
                }
                free(data);
            } else if (!S_ISSOCK(st.mode)) {
                // A fifo or a device node: the node, not what is queued in it.
                // A SOCKET is deliberately absent -- sock_ckpt_rebuild binds
                // its name itself, and a node already sitting there would make
                // that bind EADDRINUSE. Its directory still travels, which is
                // the half that was missing.
                ckpt_tmpfs_emit(c, path, &st, NULL, 0);
            }
        }
        path[len] = '\0';
        if (err < 0)
            break;
        if (c->w->err != 0) {
            err = c->w->err;
            break;
        }
    }
    if (dir->ops->readdir_end != NULL)
        dir->ops->readdir_end(dir);
    fd_close(dir);
    path[len] = '\0';
    return err;
}

// Every tmpfs in the guest, written as its own mount section. Returns the
// number of sections written, or negative on error.
// Every exit from ckpt_tmpfs_save goes through here; the table is on the heap.
#define CKPT_TMPFS_DONE(_err) \
    do { int _e = (_err); free(found); current = entered_with; return _e; } while (0)

static int ckpt_tmpfs_save(struct ckpt_writer *w, struct task *as,
        uint32_t *n_mounts_out) {
    *n_mounts_out = 0;
    uint64_t budget = CKPT_TMPFS_MAX_TOTAL;

    // The walk resolves paths, and path resolution reads `current` -- its
    // root, its pwd, its credentials. An EXTERNAL save has none: the app
    // backgrounding is not a guest task, and checkpoint_save_external sets
    // current to NULL on purpose so ckpt_freeze_all does not mistake a stale
    // pointer for a task already at a boundary. Every open here then failed
    // and every tmpfs was written as zero entries -- the empty /run this
    // section exists to prevent, reported as a success. Borrowed for the walk
    // only, and only after the freeze, so nothing else sees it.
    struct task *entered_with = current;
    if (current == NULL)
        current = as;

    // Copied out under the lock rather than walked under it: the walk opens
    // files and reads them, and holding mounts_lock across that invites a
    // lock order nobody else here takes. On the heap because the table is
    // ~70KB, which is not a thing to put on a thread stack.
    struct ckpt_tmpfs_found {
        char point[MAX_PATH + 1];
        char source[256];
        char info[256];
        int flags;
    } *found = calloc(CKPT_TMPFS_MAX_MOUNTS, sizeof(*found));
    if (found == NULL) {
        current = entered_with;
        return _ENOMEM;
    }
    unsigned n_points = 0;
    lock(&mounts_lock, 0);
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (mount->fs != &tmpfs || mount->bind_origin != NULL ||
                mount->detached || mount->lazy_umount)
            continue;
        if (mount->point == NULL || mount->point_len > MAX_PATH)
            continue;
        if (n_points >= CKPT_TMPFS_MAX_MOUNTS)
            break;
        snprintf(found[n_points].point, sizeof(found[0].point), "%s",
                 mount->point);
        snprintf(found[n_points].source, sizeof(found[0].source), "%s",
                 mount->source != NULL ? mount->source : "tmpfs");
        snprintf(found[n_points].info, sizeof(found[0].info), "%s",
                 mount->info != NULL ? mount->info : "");
        found[n_points].flags = mount->flags;
        n_points++;
    }
    unlock(&mounts_lock);

    // Shallowest first. The mount list is in whatever order the guest mounted
    // things, and /run/user/1000 came before /run -- so the restore tried to
    // build a mount point inside a tmpfs that had not been mounted yet, let
    // alone filled. Sorting by path length puts every parent ahead of its
    // children, which is all this ordering has to guarantee.
    for (unsigned i = 1; i < n_points; i++) {
        for (unsigned j = i; j > 0 &&
                strlen(found[j].point) < strlen(found[j - 1].point); j--) {
            struct ckpt_tmpfs_found tmp = found[j];
            found[j] = found[j - 1];
            found[j - 1] = tmp;
        }
    }

    for (unsigned i = 0; i < n_points; i++) {
        char path[MAX_PATH + 1];
        size_t len = strlen(found[i].point);
        // "/" as a mount point would make every child "//name"; the walk
        // wants a prefix with no trailing slash.
        while (len > 1 && found[i].point[len - 1] == '/')
            len--;
        memcpy(path, found[i].point, len);
        path[len] = '\0';

        size_t slen = strlen(found[i].source), ilen = strlen(found[i].info);
        struct ckpt_tmpfs_mount mh = {
            .point_len = (uint32_t) len,
            .source_len = (uint32_t) slen,
            .info_len = (uint32_t) ilen,
            .flags = found[i].flags,
        };
        long at = ftell(w->f);
        wr(w, &mh, sizeof(mh));
        wr(w, path, len);
        wr(w, found[i].source, slen);
        wr(w, found[i].info, ilen);
        if (w->err != 0)
            CKPT_TMPFS_DONE(w->err);

        struct ckpt_tmpfs_ctx c = {
            .w = w,
            .point_len = (len == 1 && path[0] == '/') ? 0 : len,
            .budget = budget,
        };
        int err = ckpt_tmpfs_walk(&c, path, len, 0);
        if (err < 0)
            CKPT_TMPFS_DONE(err);
        budget = c.budget;

        // The entry count is only known once the tree is walked, and the
        // mount header has to come first. Same trick the image header uses.
        long end = ftell(w->f);
        mh.n_entries = c.n_entries;
        mh.walked_ok = c.walked_ok ? 1 : 0;
        if (at < 0 || end < 0 || fseek(w->f, at, SEEK_SET) != 0)
            CKPT_TMPFS_DONE(errno_map());
        wr(w, &mh, sizeof(mh));
        if (fseek(w->f, end, SEEK_SET) != 0)
            CKPT_TMPFS_DONE(errno_map());
        if (w->err != 0)
            CKPT_TMPFS_DONE(w->err);
        (*n_mounts_out)++;
    }
    CKPT_TMPFS_DONE(0);
}
#undef CKPT_TMPFS_DONE

// One node, put back where it was. A failure here is reported and stepped
// over: a restore that drops a single file is worth far more than one that
// refuses the whole session over it.
static void ckpt_tmpfs_put(const char *abs, const struct ckpt_tmpfs_entry *e,
        const char *data) {
    mode_t_ perm = e->mode & 07777;
    int err = 0;
    if (S_ISDIR(e->mode)) {
        err = generic_mkdirat(AT_PWD, abs, perm);
    } else if (S_ISLNK(e->mode)) {
        char target[MAX_PATH + 1];
        memcpy(target, data, (size_t) e->data_len);
        target[e->data_len] = '\0';
        err = generic_symlinkat(target, AT_PWD, abs);
    } else if (S_ISREG(e->mode)) {
        struct fd *fd = generic_open(abs, O_WRONLY_ | O_CREAT_ | O_TRUNC_,
                                     perm);
        if (IS_ERR(fd)) {
            err = (int) PTR_ERR(fd);
        } else {
            uint64_t put = 0;
            while (put < e->data_len) {
                ssize_t n = fd->ops->write(fd, (void *) (data + put),
                                           (size_t) (e->data_len - put));
                if (n <= 0) { err = n < 0 ? (int) n : _EIO; break; }
                put += (uint64_t) n;
            }
            fd_close(fd);
        }
    } else {
        err = generic_mknodat(AT_PWD, abs, e->mode, (dev_t_) e->rdev);
    }
    // Already there: whatever this boot made is close enough to keep, and the
    // ownership and times below still land on it.
    if (err == _EEXIST)
        err = 0;
    if (err < 0) {
        printk("WARNING: checkpoint: could not restore %s (%d)\n", abs, -err);
        return;
    }
    // Ownership before times, because a chown is what resets them.
    generic_setattrat(AT_PWD, abs, make_attr(uid, e->uid), false);
    generic_setattrat(AT_PWD, abs, make_attr(gid, e->gid), false);
    struct timespec atime = { .tv_sec = e->atime, .tv_nsec = e->atime_nsec };
    struct timespec mtime = { .tv_sec = e->mtime, .tv_nsec = e->mtime_nsec };
    generic_utime(AT_PWD, abs, atime, mtime, false);
}

static bool ckpt_tmpfs_mounted_at(const char *point) {
    bool found = false;
    lock(&mounts_lock, 0);
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (mount->fs == &tmpfs && !mount->detached && !mount->lazy_umount &&
                mount->point != NULL && strcmp(mount->point, point) == 0) {
            found = true;
            break;
        }
    }
    unlock(&mounts_lock);
    return found;
}

// Put the tmpfs back before filling it. This is not tidiness: a resume runs
// none of the guest's init, so nothing else mounts /run, and filling the
// directory UNDERNEATH it would write the session's pid files, lock files and
// sockets into the rootfs -- where they would survive the next boot and tell
// every daemon it is already running. A tmpfs has to come back a tmpfs.
// The tmpfses this restore mounted itself, so a restore that fails later can
// take them down again (ckpt_restore_unwind). Ones that were already there are
// not listed: they were this boot's, not the image's.
static char *ckpt_restore_mounted[CKPT_TMPFS_MAX_MOUNTS];
static unsigned ckpt_restore_mounted_n;

static bool ckpt_tmpfs_remount(const char *point, const char *source,
        const char *info, int flags) {
    if (ckpt_tmpfs_mounted_at(point))
        return true;
    // The mount point itself may be missing on a rootfs that never had it --
    // and so may its PARENTS. /run/user/1000 is the one that proved it: a
    // resume runs no init, so nothing had made /run/user, and a single mkdir
    // of the leaf came back ENOENT and left that tmpfs unmounted and empty.
    int err = 0;
    char build[MAX_PATH + 1];
    size_t n = 0;
    for (const char *p = point; ; p++) {
        if (*p == '/' || *p == '\0') {
            if (n > 1) {
                build[n] = '\0';
                err = generic_mkdirat(AT_PWD, build, 0755);
                if (err < 0 && err != _EEXIST)
                    printk("WARNING: checkpoint: could not create %s (%d)\n",
                           build, -err);
            }
        }
        if (*p == '\0')
            break;
        if (n < MAX_PATH)
            build[n++] = *p;
    }
    err = do_mount(&tmpfs, source, point, info, flags);
    if (err < 0) {
        // Fill it anyway below: an empty /run is the failure the user sees,
        // and a warned-about persistent one is the lesser of the two. Said
        // out loud because the leftovers WILL outlive this boot.
        printk("WARNING: checkpoint: could not mount tmpfs on %s (%d); its "
               "contents will be written to the rootfs instead\n", point, -err);
        return false;
    }
    if (ckpt_restore_mounted_n < CKPT_TMPFS_MAX_MOUNTS)
        ckpt_restore_mounted[ckpt_restore_mounted_n++] = strdup(point);
    return true;
}

// Read the tmpfs sections back and rebuild them, before any task record is
// read and long before any descriptor is reopened against them.
static int ckpt_tmpfs_restore(FILE *f, uint32_t n_mounts) {
    if (n_mounts > CKPT_TMPFS_MAX_MOUNTS)
        return _EINVAL;
    for (uint32_t m = 0; m < n_mounts; m++) {
        struct ckpt_tmpfs_mount mh;
        int err = rd(f, &mh, sizeof(mh));
        if (err < 0)
            return err;
        if (mh.point_len == 0 || mh.point_len > MAX_PATH ||
                mh.source_len > 255 || mh.info_len > 255)
            return _EINVAL;
        char point[MAX_PATH + 1], source[256], info[256];
        if ((err = rd(f, point, mh.point_len)) < 0)
            return err;
        point[mh.point_len] = '\0';
        if ((err = rd(f, source, mh.source_len)) < 0)
            return err;
        source[mh.source_len] = '\0';
        if ((err = rd(f, info, mh.info_len)) < 0)
            return err;
        info[mh.info_len] = '\0';
        // "/" prefixed onto a relative name would give "//name".
        const char *prefix = strcmp(point, "/") == 0 ? "" : point;
        if (mh.walked_ok) {
            ckpt_tmpfs_remount(point, source, info, mh.flags);
        } else {
            printk("WARNING: checkpoint: %s was not readable when the image "
                   "was written; leaving the mount point as this boot made "
                   "it\n", point);
        }

        unsigned restored = 0;
        for (uint32_t i = 0; i < mh.n_entries; i++) {
            struct ckpt_tmpfs_entry e;
            if ((err = rd(f, &e, sizeof(e))) < 0)
                return err;
            if (e.path_len == 0 || e.path_len > MAX_PATH ||
                    e.data_len > CKPT_TMPFS_MAX_FILE)
                return _EINVAL;
            char rel[MAX_PATH + 1];
            if ((err = rd(f, rel, e.path_len)) < 0)
                return err;
            rel[e.path_len] = '\0';
            if (S_ISLNK(e.mode) && e.data_len > MAX_PATH)
                return _EINVAL;

            char *data = NULL;
            if (e.data_len > 0) {
                data = malloc((size_t) e.data_len);
                if (data == NULL)
                    return _ENOMEM;
                if ((err = rd(f, data, (size_t) e.data_len)) < 0) {
                    free(data);
                    return err;
                }
            }
            char abs[MAX_PATH + 1];
            snprintf(abs, sizeof(abs), "%s/%s", prefix, rel);
            if (mh.walked_ok)
                ckpt_tmpfs_put(abs, &e, data);
            free(data);
            restored++;
        }
        printk("checkpoint: restored %u entries under %s\n", restored, point);
    }
    return 0;
}


// ---- descriptors with no file behind them ---------------------------------
//
// See kernel/anonfd_ckpt.h. A memfd larger than this is refused by name
// rather than carried in part.
#define CKPT_MEMFD_MAX (64ull << 20)
#define CKPT_ANON_MAX (CKPT_MEMFD_MAX + (1u << 20))

struct ckpt_eventfd_desc { uint64_t val; uint32_t semaphore, pad; };

static char *ckpt_memdup(const void *p, size_t n) {
    char *c = malloc(n);
    if (c != NULL)
        memcpy(c, p, n);
    return c;
}

// Describe s->fd into s->pipe_bytes / s->pipe_len, which the writer puts after
// the record, and set `offset` to the length -- the same arrangement a pipe's
// leftover bytes use. Nothing to do for any other kind, or for an epoll set,
// whose registrations are written after the last task.
static int ckpt_describe_anon(struct ckpt_saved_fd *s) {
    char *blob = NULL;
    size_t len = 0;
    switch (s->kind) {
    case CKPT_FD_EVENTFD: {
        struct ckpt_eventfd_desc d = {
            .val = s->fd->eventfd.val,
            .semaphore = s->fd->eventfd.semaphore ? 1 : 0,
        };
        len = sizeof(d);
        blob = ckpt_memdup(&d, len);
        break;
    }
    case CKPT_FD_SIGNALFD: {
        uint64_t mask = signalfd_ckpt_mask(s->fd);
        len = sizeof(mask);
        blob = ckpt_memdup(&mask, len);
        break;
    }
    case CKPT_FD_TIMERFD: {
        struct timerfd_ckpt d;
        timerfd_ckpt_describe(s->fd, &d);
        len = sizeof(d);
        blob = ckpt_memdup(&d, len);
        break;
    }
    case CKPT_FD_PIDFD: {
        int32_t pid = pidfd_ckpt_pid(s->fd);
        len = sizeof(pid);
        blob = ckpt_memdup(&pid, len);
        break;
    }
    case CKPT_FD_INOTIFY:
        blob = inotify_ckpt_describe(s->fd, &len);
        break;
    case CKPT_FD_FIFO: {
        struct fifo_file *fifo = tmpfs_fd_fifo(s->fd);
        if (fifo == NULL)
            fifo = fakefs_fd_fifo(s->fd);
        if (fifo == NULL)
            return 0;
        blob = fifo_file_peek(fifo, &len);
        if (blob == NULL)
            return 0;   // nothing buffered
        break;
    }
    case CKPT_FD_MEMFD:
        blob = memfd_ckpt_describe(s->fd, &len, CKPT_MEMFD_MAX);
        if (blob == NULL) {
            ckpt_refuse("fd %u is a memfd that could not be carried (larger "
                        "than %llu MB, or unreadable)", s->num,
                        (unsigned long long) (CKPT_MEMFD_MAX >> 20));
            return _EFBIG;
        }
        break;
    default:
        return 0;
    }
    if (blob == NULL)
        return _ENOMEM;
    s->pipe_bytes = blob;
    s->pipe_len = len;
    s->offset = len;
    return 0;
}

// Every epoll registration, written after the last task: a registration names
// a descriptor by identity, and that descriptor may belong to any process.
struct ckpt_epoll_reg {
    uint32_t epoll_id, target_id;
    int32_t guest_fd;
    uint32_t types;
    uint64_t data;
};
struct ckpt_epoll_walk {
    struct ckpt_writer *w;
    struct ckpt_fd_ids *ids;
    uint32_t epoll_id, written, skipped;
};
static void ckpt_epoll_reg_each(void *ctx, struct fd *target, int32_t guest_fd,
                                int types, uint64_t data) {
    struct ckpt_epoll_walk *c = ctx;
    uint32_t tid = UINT32_MAX;
    for (uint32_t i = 0; i < c->ids->count; i++) {
        if (c->ids->fds[i] == target) {
            tid = i;
            break;
        }
    }
    // Registered on a descriptor no process still holds: Linux drops a
    // registration when its file closes, so there is nothing to bring back.
    if (tid == UINT32_MAX) {
        c->skipped++;
        return;
    }
    struct ckpt_epoll_reg r = {
        .epoll_id = c->epoll_id, .target_id = tid, .guest_fd = guest_fd,
        .types = (uint32_t) types, .data = data,
    };
    wr(c->w, &r, sizeof(r));
    c->written++;
}
static int ckpt_save_epoll_regs(struct ckpt_writer *w, struct ckpt_fd_ids *ids,
                                uint32_t *count) {
    struct ckpt_epoll_walk c = {.w = w, .ids = ids};
    for (uint32_t i = 0; i < ids->count; i++) {
        if (!epoll_fd_is(ids->fds[i]))
            continue;
        c.epoll_id = i;
        epoll_ckpt_each(ids->fds[i], ckpt_epoll_reg_each, &c);
    }
    CKPT_TRACE("epoll: %u registrations written, %u on nothing held\n",
               c.written, c.skipped);
    *count = c.written;
    return w->err;
}

// What a task shares, by the pid of the earlier task in the image that owns
// each object. See struct ckpt_task's tgid and owners.
struct ckpt_shares {
    uint32_t tgid;
    uint32_t mm, files, fs, sighand;
    // This record carries its process's timers (struct ckpt_task's
    // group_timers).
    bool group_timers;
};

enum ckpt_share_kind { CKPT_SHARE_MM, CKPT_SHARE_FILES, CKPT_SHARE_FS, CKPT_SHARE_SIGHAND };

static const void *ckpt_share_obj(struct task *t, enum ckpt_share_kind which) {
    switch (which) {
        case CKPT_SHARE_MM: return t->mm;
        case CKPT_SHARE_FILES: return t->files;
        case CKPT_SHARE_FS: return t->fs;
        case CKPT_SHARE_SIGHAND: return t->sighand;
    }
    return NULL;
}

// A zombie has nothing left to share, and a native task is re-launched rather
// than restored -- its objects are rebuilt by the program, never by the image
// -- so neither owns anything for another record, and neither is given an
// owner: each comes back with its own, as before.
static bool ckpt_can_share(struct task *t) {
    return !t->zombie && !t->exiting && native_program_running(t) == NULL;
}

// See struct ckpt_task's `departed`. Asked with the machine frozen.
static bool ckpt_task_departed(struct task *t) {
    return !t->zombie && t->exiting &&
        atomic_load_explicit(&t->exit_finished, memory_order_acquire) &&
        t->group != NULL && t->group->leader == t &&
        !list_empty(&t->group->threads);
}

static uint32_t ckpt_owner_of(struct task **tasks, unsigned index,
        enum ckpt_share_kind which) {
    struct task *t = tasks[index];
    const void *obj = ckpt_share_obj(t, which);
    if (obj == NULL || !ckpt_can_share(t))
        return 0;
    for (unsigned j = 0; j < index; j++)
        if (ckpt_can_share(tasks[j]) && ckpt_share_obj(tasks[j], which) == obj)
            return (uint32_t) tasks[j]->pid;
    return 0;
}

// The first task of its thread group in the image that is running. The
// group's timers go in its record, because a restore rebuilds the group with
// its first task and the timers need a group that is there -- and one that is
// still there while they are described: a running task is parked, so its group
// cannot be torn down under the save, where a departed leader's could be by
// the exit of its last thread. A native program is re-launched and arms its
// own, as it did the first time.
static bool ckpt_carries_group_timers(struct task **tasks, unsigned index) {
    struct task *t = tasks[index];
    if (!ckpt_can_share(t) || t->group == NULL)
        return false;
    for (unsigned j = 0; j < index; j++)
        if (tasks[j]->group == t->group && ckpt_can_share(tasks[j]))
            return false;
    return true;
}

static struct ckpt_shares ckpt_shares_of(struct task **tasks, unsigned index) {
    struct task *t = tasks[index];
    struct ckpt_shares sh = {
        .tgid = (uint32_t) t->tgid,
        .mm = ckpt_owner_of(tasks, index, CKPT_SHARE_MM),
        .files = ckpt_owner_of(tasks, index, CKPT_SHARE_FILES),
        .fs = ckpt_owner_of(tasks, index, CKPT_SHARE_FS),
        .sighand = ckpt_owner_of(tasks, index, CKPT_SHARE_SIGHAND),
        .group_timers = ckpt_carries_group_timers(tasks, index),
    };
    if (!ckpt_can_share(t))
        sh.tgid = (uint32_t) t->pid;
    return sh;
}

// One queued signal in the image: its siginfo, and whether a POSIX timer's
// expiry queued it as the timer's own (struct sigqueue's from_timer). That is
// not in the siginfo -- si_code SI_TIMER is what rt_sigqueueinfo can claim --
// and without it a restored timer's signal was just a signal: the timer's
// next expiry queued a second one rather than counting an overrun on it.
struct ckpt_sigqueue {
    struct siginfo_ info;
    uint32_t from_timer;
    uint32_t reserved;
};

// How many queued signals one queue may bring back: Linux's own default
// RLIMIT_SIGPENDING is in the tens of thousands, and an image claiming more
// than this is not one this build wrote.
#define CKPT_SIGQUEUE_MAX 65536

// A task's queued signals, and with `with_group` its process's, taken under the
// one sighand->lock, so that a signal arriving meanwhile -- a timer's thread
// still runs during the freeze -- is in one reading of both queues or in
// neither. malloc'd arrays, NULL when empty.
static int ckpt_signals_snapshot(struct task *task, bool with_group,
        struct ckpt_sigqueue **own, uint32_t *n_own,
        struct ckpt_sigqueue **group, uint32_t *n_group) {
    struct sighand *sighand = task->sighand;
    *own = *group = NULL;
    *n_own = *n_group = 0;
    if (sighand == NULL)
        return 0;
    lock(&sighand->lock, 0);
    unsigned long own_count = list_size(&task->queue);
    unsigned long group_count = with_group ? list_size(&sighand->queue) : 0;
    if (own_count > CKPT_SIGQUEUE_MAX || group_count > CKPT_SIGQUEUE_MAX) {
        unlock(&sighand->lock);
        ckpt_refuse("pid %d has %lu signals queued, more than an image carries",
                    task->pid, own_count > group_count ? own_count : group_count);
        return _EOPNOTSUPP;
    }
    struct ckpt_sigqueue *o = own_count != 0 ? malloc(own_count * sizeof(*o)) : NULL;
    struct ckpt_sigqueue *g = group_count != 0 ? malloc(group_count * sizeof(*g)) : NULL;
    if ((own_count != 0 && o == NULL) || (group_count != 0 && g == NULL)) {
        unlock(&sighand->lock);
        free(o);
        free(g);
        return _ENOMEM;
    }
    struct sigqueue *q;
    unsigned n = 0;
    list_for_each_entry(&task->queue, q, queue)
        o[n++] = (struct ckpt_sigqueue) {.info = q->info, .from_timer = q->from_timer};
    n = 0;
    if (with_group)
        list_for_each_entry(&sighand->queue, q, queue)
            g[n++] = (struct ckpt_sigqueue) {.info = q->info, .from_timer = q->from_timer};
    unlock(&sighand->lock);
    *own = o;
    *n_own = (uint32_t) own_count;
    *group = g;
    *n_group = (uint32_t) group_count;
    return 0;
}

// A deadline on the host's CLOCK_MONOTONIC, as the ns it has left: negative
// for one the freeze outlasted, and TIMER_CKPT_NEVER for one too far off to
// count -- a sleep asked for TIME_T_MAX, whose deadline the sleep itself only
// ever compares, never reads.
static int64_t ckpt_host_deadline_left_ns(struct timespec deadline) {
    struct timespec left = timespec_subtract(deadline, timespec_now(CLOCK_MONOTONIC));
    if (left.tv_sec >= INT64_MAX / 1000000000 - 1)
        return TIMER_CKPT_NEVER;
    return (int64_t) left.tv_sec * 1000000000 + left.tv_nsec;
}

// ...and back: the host deadline `left_ns` from now. "Never" stays never, far
// enough out that the sleep re-reading it cannot overflow either.
static struct timespec ckpt_host_deadline_after(int64_t left_ns) {
    struct timespec left = {0};
    if (left_ns == TIMER_CKPT_NEVER)
        left.tv_sec = INT64_MAX / 4;
    else if (left_ns > 0)
        left = (struct timespec) {.tv_sec = (time_t) (left_ns / 1000000000),
                                  .tv_nsec = (long) (left_ns % 1000000000)};
    return timespec_add(timespec_now(CLOCK_MONOTONIC), left);
}

static int ckpt_save_task(struct ckpt_writer *w, struct task *task,
        struct ckpt_header *h, uint64_t *pages_out, struct ckpt_fd_ids *ids,
        const struct ckpt_shares *sh) {
    struct task *saved_current = current;
    current = task;
    int ret = 0;

    const struct native_program *prog = native_program_running(task);
    struct ckpt_saved_fd *saved = NULL;
    unsigned nfds = 0;
    // Both released at `out`, which a descriptor that cannot be described
    // reaches before either is set: declared here, where the jump cannot
    // skip them, so the unwind never reads them uninitialised.
    struct mem *mem = NULL;
    char *native_env = NULL;
    struct posix_timer_ckpt *posix = NULL;
    struct ckpt_sigqueue *own_signals = NULL, *group_signals = NULL;

    bool departed = ckpt_task_departed(task);
    if (task->zombie || departed) {
        // Nothing but the status its parent has not collected. No address
        // space, no descriptors, no register file -- a zombie has already run
        // its last instruction, and what makes it worth recording is that
        // something is still going to wait() for it. A departed leader is the
        // same, before its process has finished.
        struct ckpt_task z = {
            .pid = task->pid,
            .ppid = task->parent != NULL ? task->parent->pid : 0,
            .pgid = task->group != NULL ? task->group->pgid : 0,
            .sid = task->group != NULL ? task->group->sid : 0,
            .abi = (uint32_t) task->abi,
            .zombie = departed ? 0 : 1,
            .departed = departed ? 1 : 0,
            .exit_code = task->exit_code,
            .exit_signal = task->exit_signal,
            .n_sigactions = NUM_SIGS,
            .tgid = (uint32_t) task->tgid,
            .start_time_ticks = task->start_time_ticks,
        };
        memcpy(z.comm, task->comm, sizeof(z.comm));
        CKPT_TRACE("save pid %d (ppid %d) %s: %s, exit code %#x\n",
                   z.pid, z.ppid, z.comm, departed ? "DEPARTED LEADER" : "ZOMBIE",
                   z.exit_code);
        wr(w, &z, sizeof(z));
        current = saved_current;
        return w->err;
    }

    // ---- the descriptors, gathered for either shape ----------------------
    //
    // Gathered ONCE, with a reference held, and the table lock dropped before
    // anything is asked of them. Two reasons, and the second was found the
    // hard way:
    //
    //  - Classifying twice (a pre-flight pass and then the write) meant
    //    describing a table that could have changed in between.
    //  - Asking a descriptor where it is positioned runs the filesystem's
    //    lseek, and on /proc/ish/checkpoint that regenerates the file --
    //    which walks every task's fd table, including this one. Holding
    //    files->lock across it deadlocked the guest against itself.
    struct fdtable *files = task->files;
    lock(&files->lock, 0);
    unsigned cap = files->size;
    saved = calloc(cap != 0 ? cap : 1, sizeof(*saved));
    if (saved == NULL) {
        unlock(&files->lock);
        current = saved_current;
        return _ENOMEM;
    }
    for (unsigned i = 0; i < files->size; i++) {
        struct fd *fd = files->files[i];
        if (fd == NULL)
            continue;
        saved[nfds].num = i;
        saved[nfds].fd = fd_retain(fd);
        saved[nfds].cloexec = bit_test(i, files->cloexec) ? 1 : 0;
        nfds++;
    }
    unlock(&files->lock);

    uint32_t tty_kind = CKPT_TTY_NONE;
    int tty_num = 0;
    int tty_session = 0, tty_fg_group = 0;
    struct termios_ tty_termios = {0};
    struct winsize_ tty_winsize = {0};
    char tty_path[64] = {0};
    for (unsigned i = 0; i < nfds; i++) {
        struct ckpt_saved_fd *s = &saved[i];
        s->id = ckpt_fd_id(ids, s->fd, &s->first);
        if (!s->first) {
            // Already described, here or in another process. What matters is
            // that it comes back as the SAME object.
            s->kind = CKPT_FD_REF;
        } else {
            s->kind = ckpt_classify_fd((int) s->num, s->fd, s->path,
                                       sizeof(s->path));
            if (s->kind == 0) {
                ret = _EOPNOTSUPP;
                goto out;
            }
            // A standard stream travels as WHICH one it is, 0, 1 or 2.
            s->offset = s->kind == CKPT_FD_STDIO ? (uint64_t) (s->fd->host_stdio - 1)
                                                 : ckpt_fd_offset(s->fd);
            // A pty travels as its NUMBER: that is what pairs a master with
            // its slave across a restore, where both get new ones.
            if (s->kind == CKPT_FD_PTY_MASTER || s->kind == CKPT_FD_PTY_SLAVE) {
                struct tty *terminal = fd_tty(s->fd);
                s->offset = terminal != NULL ? (uint64_t) terminal->num : 0;
            }
            // And a terminal travels as WHICH terminal it is -- driver major
            // and number -- not merely as a path: a descriptor opened through
            // /dev/tty records "/dev/tty", which names no terminal at all, and
            // one held by a process not on that terminal (the tmux server
            // holds every attached client's) has to be found again by what it
            // is. See ckpt_restore_tty_record.
            if (s->kind == CKPT_FD_TTY) {
                struct tty *terminal = fd_tty(s->fd);
                s->offset = terminal != NULL
                        ? ckpt_tty_identity(terminal->type, terminal->num) : 0;
            }
            if (s->kind == CKPT_FD_PIPE) {
                // Only the READ end carries the contents: the bytes are in the
                // pipe once, and taking them from both ends would double them.
                if (!(s->fd->flags & O_WRONLY_) &&
                        (ret = ckpt_pipe_drain(s->fd, &s->pipe_bytes,
                                               &s->pipe_len)) < 0)
                    goto out;
                s->offset = s->pipe_len;
            }
            if ((ret = ckpt_describe_anon(s)) < 0)
                goto out;
            // Asked while everything is frozen, which is the only moment the
            // answer is stable: it reads the host socket's own name and
            // whether it has a peer.
            if (s->kind == CKPT_FD_SOCKET &&
                    (ret = sock_ckpt_describe(s->fd, &s->sock)) < 0)
                goto out;
            // A connected local pair's queue travels with it, the way a pipe's
            // leftover bytes do: `offset` says how many follow the record.
            if (s->kind == CKPT_FD_SOCKET) {
                s->offset = 0;
                if (s->sock.state == SOCK_CKPT_PAIR) {
                    size_t qlen = 0;
                    s->pipe_bytes = sock_ckpt_queued(s->fd, &qlen);
                    s->pipe_len = s->pipe_bytes != NULL ? qlen : 0;
                    s->offset = s->pipe_len;
                }
            }
        }

        // WHICH terminal this process's standard streams were on, asked of
        // every descriptor rather than only the ones being described. A shell
        // and the login that forked it hold the SAME struct fd for their
        // terminal, so the second is a CKPT_FD_REF -- and looking only at
        // descriptions recorded every process but the first in a session as
        // having no terminal, which put them all back on the console. The
        // question here is not "what does this descriptor need in order to
        // come back", it is "what was this process looking at".
        // fd_tty, not s->fd->tty: the field is a union arm and is only a tty
        // pointer on a descriptor that IS one. A daemon with its standard
        // streams on /dev/null or a socket read as having a terminal and this
        // locked whatever the arm held (see fs/tty.h).
        struct tty *fd_terminal = fd_tty(s->fd);
        if (s->num <= 2 && fd_terminal != NULL) {
            // tty->type is the driver's major, which is what pty_open_fake
            // set; the device node's rdev is a second-hand copy of it.
            int major = fd_terminal->type;
            bool pts = major == TTY_PSEUDO_SLAVE_MAJOR;
            // A pty always wins over a console: it is the terminal a person is
            // looking at, and a process holding both is one that opened the
            // console for logging.
            if (pts ? tty_kind != CKPT_TTY_PTS : tty_kind == CKPT_TTY_NONE) {
                tty_kind = pts ? CKPT_TTY_PTS : CKPT_TTY_CONSOLE;
                tty_num = fd_terminal->num;
                lock(&fd_terminal->lock, 0);
                tty_session = fd_terminal->session;
                tty_fg_group = fd_terminal->fg_group;
                tty_termios = fd_terminal->termios;
                tty_winsize = fd_terminal->winsize;
                unlock(&fd_terminal->lock);
                char p[MAX_PATH + 1];
                if (generic_getpath(s->fd, p) >= 0 && p[0] == '/')
                    snprintf(tty_path, sizeof(tty_path), "%s", p);
                else
                    tty_path[0] = '\0';
                // The image's console, as a fallback for a record that names
                // no path of its own. A pty must never set it: the app has
                // BOTH -- a console supervisor and a session window -- and
                // whichever was described first used to win, which is how a
                // session on /dev/pts/1 came back on /dev/console.
                if (!pts && h->console[0] == '\0' && tty_path[0] == '/') {
                    snprintf(h->console, sizeof(h->console), "%s", tty_path);
                    h->console_major = (uint32_t) major;
                    h->console_minor = dev_minor(s->fd->stat.rdev);
                }
            }
        }
    }

    // ---- its process's timers, then its signals --------------------------
    //
    // In that order. Reading a timer waits out an expiry its thread is
    // delivering (timer_read), so whatever a timer was found to have fired is
    // queued by the time the queues are read; one that fires after it was
    // read comes back due again as well -- one more overrun on the signal it
    // queued, or a coalesced second SIGALRM. The other order loses the expiry:
    // the signal is not yet queued when the queues are read, and the timer is
    // past it when it is.
    struct group_timers_ckpt timers = {0};
    if (sh->group_timers) {
        posix = calloc(TIMERS_MAX, sizeof(*posix));
        if (posix == NULL) {
            ret = _ENOMEM;
            goto out;
        }
        group_timers_ckpt_describe(task->group, &timers, posix);
    }
    uint32_t n_own_signals = 0, n_group_signals = 0;
    // A native program is re-launched, with what it had pending as before.
    if (prog == NULL &&
            (ret = ckpt_signals_snapshot(task, sh->sighand == 0, &own_signals,
                                         &n_own_signals, &group_signals,
                                         &n_group_signals)) < 0)
        goto out;

    char cwd[MAX_PATH + 1] = "/", root[MAX_PATH + 1] = "/";
    lock(&task->fs->lock, 0);
    if (task->fs->pwd != NULL)
        generic_getpath(task->fs->pwd, cwd);
    if (task->fs->root != NULL)
        generic_getpath(task->fs->root, root);
    mode_t_ umask = task->fs->umask;
    unlock(&task->fs->lock);
    // Only where the address space is recorded. A native program's counts: its
    // mm names the /AOK/native entry it was exec'd through.
    char exe[MAX_PATH + 1] = "";
    if (sh->mm == 0 && task->mm != NULL && task->mm->exefile != NULL &&
            generic_getpath(task->mm->exefile, exe) < 0)
        exe[0] = '\0';

    struct ckpt_task rec = {
        .pid = task->pid,
        .ppid = task->parent != NULL ? task->parent->pid : 0,
        .pgid = task->group->pgid, .sid = task->group->sid,
        .n_fds = nfds,
        .abi = (uint32_t) task->abi,
        .uid = task->uid, .gid = task->gid,
        .euid = task->euid, .egid = task->egid,
        .suid = task->suid, .sgid = task->sgid,
        .fsuid = task->fsuid, .fsgid = task->fsgid,
        .umask = umask,
        .blocked = task->blocked, .pending = task->pending,
        .altstack = task->altstack, .altstack_size = task->altstack_size,
        .clear_tid = task->clear_tid,
        .exit_signal = task->exit_signal,
        .pdeath_signal = task->pdeath_signal,
        .nice = task->nice,
        .sched_policy = task->sched_policy,
        .robust_list = task->robust_list,
        .did_exec = task->did_exec ? 1 : 0,
        .tty_kind = tty_kind,
        .tty_num = (uint32_t) tty_num,
        .tty_session = tty_session,
        .tty_fg_group = tty_fg_group,
        .tty_termios = tty_termios,
        .tty_winsize = tty_winsize,
        .n_sigactions = NUM_SIGS,
        .cwd_len = (uint32_t) strlen(cwd),
        .root_len = (uint32_t) strlen(root),
        .exe_len = (uint32_t) strlen(exe),
        .cap_effective = {task->cap_effective[0], task->cap_effective[1]},
        .cap_permitted = {task->cap_permitted[0], task->cap_permitted[1]},
        .cap_inheritable = {task->cap_inheritable[0], task->cap_inheritable[1]},
        .cap_ambient = {task->cap_ambient[0], task->cap_ambient[1]},
        .cap_bounding = {task->cap_bounding[0], task->cap_bounding[1]},
        .keepcaps = task->keepcaps ? 1 : 0,
        .no_new_privs = task->no_new_privs ? 1 : 0,
        .seccomp_mode = (uint32_t) __atomic_load_n(&task->seccomp_mode, __ATOMIC_ACQUIRE),
        .seccomp_nprogs = seccomp_filter_count(task->seccomp_filter),
        .undumpable = atomic_load(&task->group->undumpable) ? 1 : 0,
        .personality = task->group->personality,
        .ngroups = task->ngroups,
        .tgid = sh->tgid,
        .mm_owner = sh->mm,
        .files_owner = sh->files,
        .fs_owner = sh->fs,
        .sighand_owner = sh->sighand,
        .start_time_ticks = task->start_time_ticks,
        .n_sigqueue = n_own_signals,
        .n_group_sigqueue = n_group_signals,
        .group_timers = sh->group_timers ? 1 : 0,
    };
    // The call the freezer rewound it over, and what that call has left. Not
    // for a native program, whose calls are not rewound but re-issued, and
    // which is re-launched anyway.
    if (prog == NULL) {
        if (task->sleep_restart_valid) {
            rec.sleep_restart_valid = 1;
            rec.sleep_restart_clock = task->sleep_restart_clock;
            rec.sleep_restart_value_ns = timer_ckpt_carry(
                    timer_ckpt_clock_for(task->sleep_restart_clock, false),
                    ckpt_host_deadline_left_ns(task->sleep_restart_deadline));
        }
        // poll's timeout is MONOTONIC's, as Linux's is.
        if (task->poll_restart_valid) {
            rec.poll_restart_valid = 1;
            rec.poll_restart_value_ns = timer_ckpt_carry(
                    TIMER_CKPT_MONOTONIC,
                    ckpt_host_deadline_left_ns(task->poll_restart_deadline));
        }
        rec.restart_pending = (task->restart_nohand_pending ? 1u : 0u) |
                              (task->restart_sys_pending ? 2u : 0u);
    }
    // The owner's record carries the table; this one points at it.
    if (sh->files != 0)
        rec.n_fds = 0;
    memcpy(rec.comm, task->comm, sizeof(rec.comm));
    memcpy(rec.tty_path, tty_path, sizeof(rec.tty_path));

    struct ckpt_count_ctx counts = {0};
    const char *native_state = NULL;
    const char *native_argv = NULL;
    size_t native_env_len = 0;

    if (prog != NULL) {
        // A NATIVE task: no address space worth photographing and no register
        // file that means anything. What travels is the program's name, the
        // argv it was given, and the state it produced about itself when the
        // freeze parked it (checkpoint_native_park runs on its own thread,
        // which is the only place its state exists).
        native_state = task->ckpt_native_state != NULL ? task->ckpt_native_state : "";
        rec.native_standin_child = task->native_standin_child;
        // A stand-in is not re-launched at all, so it is not a restart.
        if (native_state[0] == '\0' && rec.native_standin_child == 0)
            ckpt_note_restarted_native(prog->name);
        native_argv = task->native_cmdline != NULL ? task->native_cmdline : "";
        rec.native = 1;
        rec.native_name_len = (uint32_t) strlen(prog->name);
        rec.native_argv_len = (uint32_t) (task->native_cmdline != NULL
                ? task->native_cmdline_len : 0);
        rec.native_state_len = (uint32_t) strlen(native_state);
        // The environment, as one NUL-separated block. A shell's exported
        // parameters come back with the state script, but the program reads
        // `environ` before it sources anything -- and a restored program with
        // no PATH at all would not find the first thing it was asked to run.
        for (char **e = task->native_env; e != NULL && *e != NULL; e++)
            native_env_len += strlen(*e) + 1;
        if (native_env_len != 0) {
            native_env = malloc(native_env_len);
            if (native_env == NULL) { ret = _ENOMEM; goto out; }
            size_t at = 0;
            for (char **e = task->native_env; *e != NULL; e++) {
                size_t n = strlen(*e) + 1;
                memcpy(native_env + at, *e, n);
                at += n;
            }
        }
        rec.native_env_len = (uint32_t) native_env_len;
        CKPT_TRACE("save pid %d (ppid %d) NATIVE %s: %u fds, %u bytes of state\n",
                   rec.pid, rec.ppid, prog->name, rec.n_fds,
                   rec.native_state_len);
        // The tail, because a state that does not finish is the failure this
        // has to distinguish from one that is simply wrong: the last line is
        // the program's own sentinel.
        CKPT_TRACE("  state ends: %s\n", rec.native_state_len > 90
                   ? native_state + rec.native_state_len - 90 : native_state);
    } else if (sh->mm != 0) {
        // The owner's record carries the address space.
        CKPT_TRACE("save pid %d (tgid %u, ppid %d) %s: shares the address space "
                   "of pid %u%s, pc %#llx\n", rec.pid, rec.tgid, rec.ppid, rec.comm,
                   sh->mm, sh->files != 0 ? " and its descriptors" : "",
                   (unsigned long long) (task->abi == GUEST_ABI_ARM64
                           ? task->cpu.arm64_pc : task->cpu.amd64_rip));
    } else {
        struct mm *mm = task->mm;
        mem = task->mem;
        read_lock(&mem->lock);
        ckpt_for_each_map(mem, ckpt_count_map, &counts);
        ckpt_for_each_reservation(mem, ckpt_count_reservation, &counts);
        rec.n_maps = counts.maps;
        rec.brk = mm->brk; rec.start_brk = mm->start_brk;
        rec.vdso = mm->vdso; rec.stack_start = mm->stack_start;
        rec.argv_start = mm->argv_start; rec.argv_end = mm->argv_end;
        rec.env_start = mm->env_start; rec.env_end = mm->env_end;
        rec.auxv_start = mm->auxv_start; rec.auxv_end = mm->auxv_end;
        rec.page_limit = mem->page_limit;
        rec.mmap_floor = mem->mmap_floor;
        rec.mmap_ceiling = mem->mmap_ceiling;
        rec.stack_top = mem->stack_top;
        rec.stack_limit_pages = mem->stack_limit_pages;
        CKPT_TRACE("save pid %d (ppid %d pgid %d sid %d) %s: %u maps, %u fds, "
                   "%llu pages, pc %#llx, tty %s%s\n", rec.pid, rec.ppid, rec.pgid,
                   rec.sid, rec.comm, rec.n_maps, rec.n_fds,
                   (unsigned long long) counts.pages,
                   (unsigned long long) (task->abi == GUEST_ABI_ARM64
                           ? task->cpu.arm64_pc : task->cpu.amd64_rip),
                   rec.tty_kind == CKPT_TTY_PTS ? "pts" :
                   rec.tty_kind == CKPT_TTY_CONSOLE ? "console" : "none",
                   rec.tty_kind == CKPT_TTY_PTS ? rec.tty_path : "");
    }

    wr(w, &rec, sizeof(rec));
    wr(w, cwd, rec.cwd_len);
    wr(w, root, rec.root_len);
    wr(w, exe, rec.exe_len);
    if (rec.ngroups > 0)
        wr(w, task->groups, rec.ngroups * sizeof(*task->groups));
    if (rec.seccomp_nprogs > 0) {
        // The task is frozen, so no sibling's TSYNC can change the chain
        // between the count above and this.
        struct seccomp_ckpt_prog *progs = calloc(rec.seccomp_nprogs, sizeof(*progs));
        unsigned n = progs == NULL ? 0 :
                seccomp_ckpt_export(task->seccomp_filter, progs, rec.seccomp_nprogs);
        if (progs == NULL || n != rec.seccomp_nprogs) {
            // The record already promised them; an image without them would
            // restore the process unconfined. Refuse the whole save instead.
            ckpt_refuse("pid %d: could not record its seccomp filters", task->pid);
            w->err = _ENOMEM;
        } else {
            for (unsigned i = 0; i < n; i++) {
                struct ckpt_seccomp_prog hdr = {progs[i].log ? 1 : 0, progs[i].len};
                wr(w, &hdr, sizeof(hdr));
                wr(w, progs[i].insns, (size_t) progs[i].len * 8);
            }
        }
        free(progs);
    }

    if (prog != NULL) {
        wr(w, prog->name, rec.native_name_len);
        wr(w, native_argv, rec.native_argv_len);
        wr(w, native_state, rec.native_state_len);
        wr(w, native_env, rec.native_env_len);
    } else {
        // The register file, as bytes. See ckpt_fingerprint for why that is
        // safe and what stops it from being unsafe.
        wr(w, &task->cpu, sizeof(struct cpu_state));

        lock(&task->sighand->lock, 0);
        wr(w, task->sighand->action, sizeof(struct sigaction_) * NUM_SIGS);
        unlock(&task->sighand->lock);

        lock(&task->group->lock, 0);
        wr(w, task->group->limits, sizeof(task->group->limits));
        unlock(&task->group->lock);

        if (mem != NULL) {
            struct ckpt_emit_ctx emit = { .w = w, .mem = mem };
            ckpt_for_each_map(mem, ckpt_emit_map, &emit);
            ckpt_for_each_reservation(mem, ckpt_emit_reservation, &emit);
            read_unlock(&mem->lock);
            mem = NULL;
            *pages_out += counts.pages;
        }
    }

    for (unsigned i = 0; i < rec.n_fds && w->err == 0; i++) {
        struct ckpt_saved_fd *s = &saved[i];
        struct ckpt_fd cf = {
            .fd = s->num,
            .cloexec = s->cloexec,
            .flags = s->fd->flags,
            .kind = (uint32_t) s->kind,
            // For a standard stream the offset is meaningless and the host
            // descriptor it mirrors is what matters, so the field carries
            // that instead; for a pipe it is how many bytes follow.
            .offset = s->offset,
            .path_len = (uint32_t) strlen(s->path),
            .id = s->id,
            .pipe_inode = s->kind == CKPT_FD_PIPE ? s->fd->stat.inode : 0,
            .pipe_write_end = s->kind == CKPT_FD_PIPE &&
                    (s->fd->flags & O_WRONLY_) ? 1 : 0,
        };
        if (s->kind == CKPT_FD_PTY_MASTER) {
            uid_t_ ouid, ogid;
            mode_t_ operms;
            if (pty_slave_owner_of(fd_tty(s->fd), &ouid, &ogid, &operms)) {
                cf.pty_uid = ouid;
                cf.pty_gid = ogid;
                cf.pty_perms = operms;
                cf.pty_owner_known = 1;
            }
        }
        CKPT_TRACE("  save fd %u %-5s id %u real_fd %d flags %#x off %llu %s\n",
                   cf.fd, ckpt_kind_name(cf.kind), cf.id, s->fd->real_fd,
                   cf.flags, (unsigned long long) cf.offset, s->path);
        wr(w, &cf, sizeof(cf));
        wr(w, s->path, cf.path_len);
        if (s->pipe_len != 0)
            wr(w, s->pipe_bytes, s->pipe_len);
        if (s->kind == CKPT_FD_SOCKET)
            wr(w, &s->sock, sizeof(s->sock));
    }

    // After the descriptors: the signals, then the timers.
    wr(w, own_signals, (size_t) n_own_signals * sizeof(*own_signals));
    wr(w, group_signals, (size_t) n_group_signals * sizeof(*group_signals));
    if (sh->group_timers) {
        wr(w, &timers, sizeof(timers));
        wr(w, posix, (size_t) timers.n_posix * sizeof(*posix));
    }
    if (n_own_signals != 0 || n_group_signals != 0 || rec.sleep_restart_valid ||
            rec.poll_restart_valid)
        CKPT_TRACE("  pid %d: %u signals queued, %u for the process%s%s\n",
                   task->pid, n_own_signals, n_group_signals,
                   rec.sleep_restart_valid ? ", a sleep's deadline" : "",
                   rec.poll_restart_valid ? ", a poll's deadline" : "");
    if (sh->group_timers)
        CKPT_TRACE("  pid %d: itimer %s, %u POSIX timers\n", task->pid,
                   timers.real.armed ? "armed" : "not armed", timers.n_posix);
    ret = w->err;

out:
    free(posix);
    free(own_signals);
    free(group_signals);
    free(native_env);
    if (mem != NULL)
        read_unlock(&mem->lock);
    if (saved != NULL) {
        for (unsigned i = 0; i < nfds; i++) {
            fd_close(saved[i].fd);
            free(saved[i].pipe_bytes);
        }
        free(saved);
    }
    current = saved_current;
    return ret;
}

// Order the tasks so a parent is always written before its children.
//
// Not tidiness: the restore creates each task as a CHILD of one that already
// exists, because that is the only way the parent/child lists and the wait
// machinery come out right. A child written first would have nothing to be
// created under.
static bool ckpt_task_listed(struct task **tasks, unsigned count, struct task *t) {
    for (unsigned i = 0; i < count; i++)
        if (tasks[i] == t)
            return true;
    return false;
}

// And siblings in the order their parent lists them. The restore links each
// task at the end of its parent's children list as it builds it (task.c), so
// the image's order IS the restored list's, and that list is what a wait
// walks: oldest first, so a parent's wait(-1) reaps its oldest zombie first,
// and must after a restore too. The collection comes in pid-table order,
// newest first, and the placement below used to take a ready task wherever it
// found one: four zombies forked 4 5 6 7 came back reaped 7 4 6 5
// (checkpoint_threads.sh, mode order).
//
// So first a walk of the tree: every task with no parent in the image (init,
// a self-parent, one reparented while this was collected), then breadth-first
// each one's children in its list's order, then anything the walk did not
// reach. Parents come before their children in it; the placement keeps that
// order among the tasks it moves past, so it only ever defers a thread to
// after its leader, and a process's children stay in their list's order.
static void ckpt_order_by_tree(struct task **tasks, unsigned count) {
    if (count == 0)
        return;
    struct task **order = malloc(sizeof(*order) * count);
    bool *taken = calloc(count, sizeof(*taken));
    if (order == NULL || taken == NULL) {
        // Still a valid image: parents before children, siblings in the
        // collection's order.
        free(order);
        free(taken);
        return;
    }
    unsigned n = 0;
    complex_lockt(&pids_lock, 0);
    for (unsigned i = 0; i < count; i++) {
        struct task *t = tasks[i];
        if (t->parent == NULL || t->parent == t ||
                !ckpt_task_listed(tasks, count, t->parent)) {
            order[n++] = t;
            taken[i] = true;
        }
    }
    for (unsigned q = 0; q < n; q++) {
        struct task *child;
        list_for_each_entry(&order[q]->children, child, siblings) {
            for (unsigned i = 0; i < count; i++) {
                if (tasks[i] == child && !taken[i]) {
                    order[n++] = child;
                    taken[i] = true;
                    break;
                }
            }
        }
    }
    unlock(&pids_lock);
    for (unsigned i = 0; i < count; i++)
        if (!taken[i])
            order[n++] = tasks[i];
    memcpy(tasks, order, sizeof(*tasks) * count);
    free(order);
    free(taken);
}

// And a thread after its group's leader, which the restore builds the group
// around. The leader is usually the thread's ancestor anyway, but not always:
// once a leader has exited, AOK hands its children to the first live thread
// of the group (find_new_parent), which can leave a thread its OWN parent --
// never ready by the parent rule alone, so a self-parent counts as none.
// The tail loop below used to take such a task "anyway"; it is ordered now.
static void ckpt_order_tasks(struct task **tasks, unsigned count) {
    ckpt_order_by_tree(tasks, count);
    unsigned placed = 0;
    while (placed < count) {
        unsigned progress = 0;
        for (unsigned i = placed; i < count; i++) {
            struct task *t = tasks[i];
            // No usable parent -- itself, or one not in the image -- means
            // init's child, which is what the restore makes it: ready once the
            // first task is down. Never before it: the image's first task must
            // be pid 1, the entry point's own.
            bool parent_ready = t->parent == NULL ||
                    (placed > 0 && (t->parent == t ||
                                    !ckpt_task_listed(tasks, count, t->parent)));
            for (unsigned j = 0; j < placed && !parent_ready; j++)
                if (tasks[j] == t->parent)
                    parent_ready = true;
            struct task *leader = t->group != NULL ? t->group->leader : NULL;
            bool leader_ready = leader == NULL || leader == t ||
                    !ckpt_task_listed(tasks, count, leader) ||
                    ckpt_task_listed(tasks, placed, leader);
            if (!parent_ready || !leader_ready)
                continue;
            // Moved in front of the ones it passed, which keep their order:
            // a swap sent the first of them to where this one was.
            memmove(&tasks[placed + 1], &tasks[placed], sizeof(*tasks) * (i - placed));
            tasks[placed] = t;
            placed++;
            progress++;
        }
        // A task whose parent is not in the snapshot at all -- reparented to
        // init while this was being collected. Take it anyway rather than
        // spinning; the restore reparents it to pid 1, which is where it was
        // going.
        if (progress == 0)
            break;
    }
}

// The externally-triggered form. Same body; the difference is only that there
// is no `current` to leave running, so every task is frozen.
int checkpoint_save_external(const char *host_path) {
    // The caller is NOT a guest task, and it has to say so.
    //
    // `current` is per-thread and the app's UI thread has one: kernel/init.c's
    // become_first_process leaves it pointing at init, and every session start
    // leaves it pointing at that session's own first process. ckpt_freeze_all
    // deliberately does not freeze `current` -- the task asking for a
    // checkpoint is already at a boundary -- so an external save inherited
    // that stale pointer and skipped a task that was running.
    //
    // What that produced is the worst kind of quiet: the skipped task was
    // blocked inside wait(), never woken, never rewound, and its cpu_state was
    // photographed mid-syscall with the program counter already past the
    // instruction. The restored process came back as though its wait had
    // returned, tidied up and exited -- so the session the image was taken to
    // preserve was a pair of zombies a millisecond after the resume, with
    // nothing anywhere reporting a failure. See [[current-is-not-always-your-task]].
    struct task *saved = current;
    current = NULL;
    int err = checkpoint_save(host_path);
    current = saved;
    return err;
}

int checkpoint_save(const char *host_path) {
    int err = ckpt_check_scope();
    if (err < 0)
        return err;

    // Per-image, not cumulative: this describes the save about to be written.
    ckpt_natives_restarted = 0;
    ckpt_natives_note[0] = '\0';

    // WHERE THE TIME WENT. A suspend reported as taking "a minute or more" on
    // a device could not be compared against anything, because the save said
    // only whether it worked. The same image takes a fifth of a second on a
    // Mac, so the minute is not the bytes -- and without a breakdown the only
    // way to find out which phase owns it was to guess, repeatedly.
    //
    // Four phases, each with a different cause if it is the slow one: the
    // FREEZE waits for every task to reach a syscall boundary; the WRITE
    // walks each task's memory, and faults evicted pages back in through
    // mem_ptr(MEM_READ), so a heavily swapped guest pays the pager here; the
    // FSYNC is the storage; the rename is nothing.
    struct timespec t_start = timespec_now(CLOCK_MONOTONIC);
    struct timespec t_frozen = t_start, t_written = t_start, t_synced = t_start;

    // STOP THE MACHINE. Everything below describes tasks that are not running,
    // which is the whole difference between a checkpoint and a photograph of a
    // moving object.
    char blame[192] = "";
    err = ckpt_freeze_all(CKPT_FREEZE_TIMEOUT_MS, blame, sizeof(blame));
    if (err < 0) {
        ckpt_refuse("%s", blame);
        return err;
    }
    t_frozen = timespec_now(CLOCK_MONOTONIC);
    // The guest's clocks at the instant the machine stopped: the moment the
    // image describes, and the last one any guest task saw. Everything a
    // restore counts as time spent stopped is measured from here.
    struct guest_clock_reading clocks;
    guest_clock_read(&clocks);

    // Zombies and departed leaders included: the ordinary collection skips
    // both, and every zombie in a session used to be lost at a save.
    struct task_snapshot snap = {0};
    if (task_snapshot_collect_all(&snap) < 0) {
        ckpt_thaw_all();
        ckpt_refuse("could not enumerate tasks");
        return _EAGAIN;
    }
    // A task inside do_exit has already run its last instruction and has no
    // state left worth carrying. Dropped here rather than in the writer, so
    // the header's task count is the number actually written. Kept: a
    // process's zombie -- not a thread's, which only a tracer waits for, and
    // tracing is not carried -- and a departed leader.
    //
    // Decided under pids_lock, which a departed leader's thread list needs;
    // the references are dropped after it, since dropping the last one can
    // free the task.
    bool *keep = calloc(snap.count != 0 ? snap.count : 1, sizeof(*keep));
    if (keep == NULL) {
        task_snapshot_release(&snap);
        ckpt_thaw_all();
        ckpt_refuse("could not enumerate tasks");
        return _ENOMEM;
    }
    complex_lockt(&pids_lock, 0);
    for (unsigned i = 0; i < snap.count; i++) {
        struct task *t = snap.tasks[i];
        keep[i] = t->zombie ? t->group != NULL && t->group->leader == t
                            : ckpt_task_departed(t) || !ckpt_task_is_leaving(t);
    }
    unlock(&pids_lock);
    unsigned live = 0;
    for (unsigned i = 0; i < snap.count; i++) {
        if (keep[i])
            snap.tasks[live++] = snap.tasks[i];
        else
            task_ref_cnt_mod(snap.tasks[i], -1);
    }
    free(keep);
    snap.count = live;
    ckpt_order_tasks(snap.tasks, snap.count);

    // Written to a temporary beside the target and renamed into place only when
    // it is whole.
    //
    // The header goes first, so a file truncated by a kill mid-write still
    // passes checkpoint_peek -- and the session picker then offers it as a real
    // choice, which fails part way through restoring. Worse, opening the target
    // directly meant a save that REFUSED had already destroyed the previous
    // good session by truncating it. With one suspend.img that was bad; with
    // slots it is somebody's other session.
    char tmp_path[PATH_MAX];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.new", host_path) >= (int) sizeof(tmp_path)) {
        task_snapshot_release(&snap);
        ckpt_thaw_all();
        return _ENAMETOOLONG;
    }
    FILE *f = fopen(tmp_path, "wb");
    if (f != NULL)
        // One write(2) per PAGE otherwise. ckpt_emit_map calls wr() once per
        // 4 KB page, and stdio's default buffer is the file's st_blksize --
        // also 4 KB -- so every page went straight to the kernel as its own
        // write: about 250,000 of them for a 1 GB image. A megabyte of buffer
        // makes that one write per 256 pages.
        //
        // It costs nothing when writes are cheap (measured 610 MB/s either way
        // on a Mac) and it matters when they are not. Under Xcode every write
        // goes through libLogRedirect.dylib's interposer, which is what a
        // suspend that looked hung was doing in its thread dump -- not
        // deadlocked, just paying that toll a quarter of a million times.
        setvbuf(f, NULL, _IOFBF, 1 << 20);
    if (f == NULL) {
        err = errno_map();
        task_snapshot_release(&snap);
        ckpt_thaw_all();
        return err;
    }
    struct ckpt_writer w = { .f = f };

    struct ckpt_header h = {
        .version = CKPT_VERSION,
        // The first task's, not `current`'s: an external caller (the app's
        // backgrounding path) is not a guest task at all.
        .abi = (uint32_t) snap.tasks[0]->abi,
        .cpu_state_size = (uint32_t) sizeof(struct cpu_state),
        .page_size = PAGE_SIZE,
        .build_fingerprint = ckpt_fingerprint(),
        .n_tasks = snap.count,
        .clock_monotonic_ns = clocks.monotonic_ns,
        .clock_boottime_ns = clocks.boottime_ns,
        .clock_raw_ns = clocks.raw_ns,
        .clock_realtime_ns = clocks.realtime_ns,
        .clock_boot_time = clocks.boot_time,
    };
    memcpy(h.magic, CKPT_MAGIC, sizeof(h.magic));
    // The UTS namespace. init's, which is the one every AOK task is in unless
    // it unshared -- and the one the resume path has to put back, because it
    // returns before the boot code that seeds it.
    lock(&init_uts_ns.lock, 0);
    snprintf(h.hostname, sizeof(h.hostname), "%s", init_uts_ns.hostname);
    snprintf(h.domainname, sizeof(h.domainname), "%s", init_uts_ns.domainname);
    unlock(&init_uts_ns.lock);
    h.root_identity = ckpt_running_root(h.root_name, sizeof(h.root_name));
    // Written now and rewritten at the end: the page count and the stdio kind
    // are only known once every task has been walked, and the header has to
    // come first in the file.
    long header_at = ftell(f);
    wr(&w, &h, sizeof(h));

    // Before the tasks, because their descriptors name these paths and the
    // reopen on the far side needs the tree to already be there.
    if (err == 0 && w.err == 0) {
        int terr = ckpt_tmpfs_save(&w, snap.tasks[0], &h.n_tmpfs);
        if (terr < 0) {
            ckpt_refuse("could not write the tmpfs contents (%d)", -terr);
            err = terr;
        }
    }

    uint64_t pages = 0;
    // One id space for the whole image, so a descriptor two processes share is
    // described once and referenced from the other.
    struct ckpt_fd_ids ids = {0};
    for (unsigned i = 0; i < snap.count && err == 0; i++) {
        struct ckpt_shares sh = ckpt_shares_of(snap.tasks, i);
        err = ckpt_save_task(&w, snap.tasks[i], &h, &pages, &ids, &sh);
    }
    if (err == 0 && w.err == 0)
        err = ckpt_save_epoll_regs(&w, &ids, &h.n_epoll_regs);
    unsigned nfds_total = ids.count;
    free(ids.fds);
    task_snapshot_release(&snap);

    t_written = timespec_now(CLOCK_MONOTONIC);
    // How long the image is SUPPOSED to be, taken before the seek back to
    // rewrite the header. Checked against the file once it is closed.
    off_t expect_size = -1;
    if (err == 0 && w.err == 0) {
        expect_size = ftello(f);
        h.total_pages = pages;
        if (fseek(f, header_at, SEEK_SET) == 0)
            wr(&w, &h, sizeof(h));
        else
            w.err = errno_map();
    }
    if (err == 0)
        err = w.err;
    if (fclose(f) != 0 && err == 0)
        err = errno_map();

    // The image is only as good as what actually reached the disk, and until
    // now nothing confirmed that it had. Every error path above reports a
    // failure honestly, but a save that comes back 0 was simply BELIEVED --
    // and a believed save that is short produces an image that cannot be
    // restored, which the next launch reports as "there was no session",
    // blaming the restore for what the suspend did.
    //
    // Cheap: one stat against a length we already know. It cannot catch a
    // corrupt image, but it catches a truncated one, which is what an
    // interrupted or short write leaves behind.
    if (err == 0 && expect_size >= 0) {
        struct stat img_st;
        if (stat(tmp_path, &img_st) != 0) {
            err = errno_map();
        } else if (img_st.st_size != expect_size) {
            ckpt_refuse("the session image is %lld bytes, not the %lld written "
                        "-- refusing to keep a truncated one",
                        (long long) img_st.st_size, (long long) expect_size);
            err = _EIO;
        }
    }

    ckpt_thaw_all();

    if (err != 0) {
        unlink(tmp_path);   // whatever was already at host_path is untouched
        return err;
    }

    // Durable before it is visible, and both steps AFTER the thaw: the guest
    // does not have to be stopped for an fsync, and on a session-sized image
    // that is the longest part of the whole save.
    int img_fd = open(tmp_path, O_RDONLY);
    if (img_fd >= 0) {
        fsync(img_fd);
        close(img_fd);
    }
    t_synced = timespec_now(CLOCK_MONOTONIC);
    if (rename(tmp_path, host_path) != 0) {
        err = errno_map();
        unlink(tmp_path);
        return err;
    }
    // The rename itself has to survive a power cut, which means fsyncing the
    // directory that now names the file.
    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s", host_path);
    char *slash = strrchr(dir_path, '/');
    if (slash != NULL) {
        if (slash == dir_path)
            dir_path[1] = '\0';
        else
            *slash = '\0';
        int dir_fd = open(dir_path, O_RDONLY);
        if (dir_fd >= 0) {
            fsync(dir_fd);
            close(dir_fd);
        }
    }

    // One line saying where the time went. Always, not under a debug flag: a
    // suspend is user-visible and a slow one is a bug report, and the whole
    // reason this exists is that "it takes a minute" could not be turned into
    // a question without it.
    {
        struct timespec t_end = timespec_now(CLOCK_MONOTONIC);
        #define CKPT_MS(a, b) ((double) ((b).tv_sec - (a).tv_sec) * 1000.0 + \
                               (double) ((b).tv_nsec - (a).tv_nsec) / 1000000.0)
        printk("checkpoint: saved %u tasks, %llu pages, %lld bytes in %.0f ms "
               "(freeze %.0f, write %.0f, fsync %.0f)\n",
               h.n_tasks, (unsigned long long) pages,
               (long long) expect_size, CKPT_MS(t_start, t_end),
               CKPT_MS(t_start, t_frozen), CKPT_MS(t_frozen, t_written),
               CKPT_MS(t_written, t_synced));
        #undef CKPT_MS
    }

    lock(&ckpt_lock, 0);
    ckpt_status.saves++;
    ckpt_status.last_err = 0;
    ckpt_status.last_refusal[0] = '\0';
    snprintf(ckpt_status.last_path, sizeof(ckpt_status.last_path), "%s", host_path);
    ckpt_status.pages = (unsigned long) pages;
    ckpt_status.tasks = h.n_tasks;
    ckpt_status.natives_restarted = ckpt_natives_restarted;
    snprintf(ckpt_status.natives_note, sizeof(ckpt_status.natives_note),
             "%s", ckpt_natives_note);
    ckpt_status.fds = nfds_total;
    ckpt_status.bytes = (unsigned long long) pages * PAGE_SIZE;
    unlock(&ckpt_lock);
    return 0;
}

// ------------------------------------------------------------------ reading

// Everything about one task, read back onto `task`, which must already exist
// and be current.
// What the restore has built so far, shared by every task in the image.
struct ckpt_restore_state {
    // The native program the task being restored right now is, if it is one.
    // Read by the caller once ckpt_restore_task returns, so it can dispatch
    // the program rather than start a guest thread.
    char *native_name, *native_argv, *native_state, *native_env;
    uint32_t native_argv_len, native_env_len;
    uint32_t native_standin_child;
    // The console the checkpointed guest was on, and the device it expects to
    // find there. create_stdio verifies the major/minor and falls back to an
    // adhoc node if they do not match, so both halves travel.

    // id -> the struct fd built for it. A CKPT_FD_REF installs this one again
    // rather than making a second object, which is what keeps a forked child's
    // file offset the same object as its parent's.
    struct fd **by_id;
    uint32_t id_count, id_cap;
    // pipe inode -> the two ends built for it, so a pair whose ends are held
    // by two different processes becomes ONE host pipe.
    struct { uint64_t inode; struct fd *rd, *wr; } *pipes;
    uint32_t pipe_count, pipe_cap;
    // The host's own standard streams as THIS run was handed them, one
    // descriptor per stream for the whole image (ckpt_host_stdio).
    struct fd *host_stdio[3];
    // One standard-stream set per TERMINAL, made on first sight.
    //
    // Not one per image, which is what this was: six gettys on six virtual
    // consoles all came back reading /dev/console, taking turns at each
    // other's keystrokes. A terminal is identified by what it is -- a console
    // by its path, a pseudo-terminal by the SESSION it belongs to, because the
    // pty itself is new and its old number means nothing.
    struct ckpt_stdio_set {
        uint32_t kind;      // enum ckpt_tty_kind
        uint32_t sid;       // CKPT_TTY_PTS: whose session this terminal is
        char path[64];      // CKPT_TTY_CONSOLE: which device
        struct fd *stdio[3];
        struct tty *tty;    // CKPT_TTY_PTS: the terminal itself
        int tty_num;
        void *terminal;     // the tty's driver data, for the UI to adopt
        int leader_pid;
        // The terminal as the IMAGE knew it -- the kind and number its first
        // process recorded -- which is how a record naming it by identity
        // finds it again (kind is the recorded one, before the CLI's
        // no-window downgrade to the console).
        uint32_t old_kind;
        int old_num;
        // stdio[] is one description, and the first terminal record mapped
        // onto it decides which one of the image's it stands for: its id, and
        // its flags. Every other description on this terminal gets a
        // descriptor of its own (ckpt_tty_description).
        bool claimed;
        uint32_t primary_id;
    } *sets;
    uint32_t set_count, set_cap;
    // old pty number -> the number its master got when it was re-opened.
    //
    // A pty pair internal to the image (tmux and its pane) comes back as a
    // NEW pair, so the slave has to be told where its master went. Populated
    // when a CKPT_FD_PTY_MASTER is restored and read by every slave after it.
    // A pane's own processes come after it, because tasks arrive parents-first
    // and the master holder is the pane's parent. A slave held by anyone else
    // may come first -- another tmux server, handed a client's terminal -- and
    // waits in pending_ttys until every task exists. One that still finds no
    // entry then falls back to the terminal its holder was given -- the old
    // behaviour, and the honest answer when the master was not in the image.
    struct { int old_num, new_num; } *ptys;
    uint32_t pty_count, pty_cap;
    // pidfds made unbound, to be bound to their process once every task is
    // built (ckpt_restore_pidfds).
    struct { struct fd *fd; int32_t pid; } *pidfds;
    uint32_t pidfd_count, pidfd_cap;
    // Named FIFOs: the buffers already put back (one FIFO can be open through
    // several descriptors, each of which carried its bytes), and write-only
    // ends opened read-write until every task exists (ckpt_restore_fifos).
    struct fifo_file **fifos_primed;
    uint32_t fifos_primed_count, fifos_primed_cap;
    struct { struct fd *fd; uint32_t flags; } *fifo_writers;
    uint32_t fifo_writer_count, fifo_writer_cap;
    // Connected AF_LOCAL pairs, made when the first end's record is read and
    // handed out end by end (SOCK_CKPT_PAIR). An end still here at the end of
    // the restore had no process holding it, and is closed -- its partner then
    // sees the peer gone, which is what it would have seen.
    struct { uint64_t cookie; struct fd *end[2]; bool claimed[2]; } *sockpairs;
    uint32_t sockpair_count, sockpair_cap;
    // Terminal descriptors whose terminal had not been rebuilt yet when their
    // record was read: a daemon earlier in the image than the session whose
    // terminal it holds (the tmux server, older than a window a client later
    // attached from). Resolved once every task exists; a CKPT_FD_REF to one of
    // them waits with it. `head` marks the entry that carries the description.
    //
    // A pty SLAVE whose master had not been rebuilt yet waits here too
    // (`pty_slave`), for the same reason: the tmux server holds the slave of a
    // pane in ANOTHER tmux server when a client attached from that pane, and
    // the server holding it is often the older of the two.
    struct ckpt_pending_tty {
        uint32_t id;
        int num;            // the pty number the image knew
        uint32_t sid;       // the holder's session, to break a tie
        int flags;
        struct task *task;
        uint32_t fd;
        bool cloexec;
        bool head;
        bool pty_slave;     // a CKPT_FD_PTY_SLAVE, not a terminal record
        uint32_t set;       // pty_slave: the holder's stdio set, the fallback
    } *pending_ttys;
    uint32_t pending_tty_count, pending_tty_cap;
    // Each process's timers, read with its first task and armed only once
    // every task has started (checkpoint_restore). Armed any earlier, one that is
    // due at once would signal a task with no thread of its own yet, and a
    // restore failing after that would leave them firing into a process that
    // task_never_ran_destroy had taken apart -- it does not free a group's
    // timers, which only exit does.
    struct ckpt_group_timers {
        struct tgroup *group;
        uint32_t pid;
        struct group_timers_ckpt d;
        struct posix_timer_ckpt *posix;
    } *timers;
    uint32_t timers_count, timers_cap;
};

// Remember where a restored pty master ended up, and look it up again.
static int ckpt_pty_map_put(struct ckpt_restore_state *st, int old_num, int new_num) {
    if (st->pty_count == st->pty_cap) {
        uint32_t cap = st->pty_cap ? st->pty_cap * 2 : 8;
        void *n = realloc(st->ptys, cap * sizeof(*st->ptys));
        if (n == NULL)
            return _ENOMEM;
        st->ptys = n;
        st->pty_cap = cap;
    }
    st->ptys[st->pty_count].old_num = old_num;
    st->ptys[st->pty_count].new_num = new_num;
    st->pty_count++;
    return 0;
}

static int ckpt_pty_map_get(struct ckpt_restore_state *st, int old_num) {
    for (uint32_t i = 0; i < st->pty_count; i++)
        if (st->ptys[i].old_num == old_num)
            return st->ptys[i].new_num;
    return -1;
}


// Record `fd` under `id`, taking a reference of the table's own.
//
// The table holding a reference is what makes the ownership rule one sentence:
// EVERY pointer this structure keeps -- stdio, by_id, both ends of each pipe --
// is one reference, released once at the end of the restore. Installing a
// descriptor in a process's table is a separate retain. Getting this wrong the
// other way round closed the standard streams an extra time each, which shut
// the app's real stdout and made a restored guest look silently hung.
static int ckpt_id_put(struct ckpt_restore_state *st, uint32_t id, struct fd *fd) {
    if (id == UINT32_MAX)
        return 0;   // the save could not allocate an id; nothing refers to it
    if (id >= st->id_cap) {
        uint32_t cap = st->id_cap ? st->id_cap * 2 : 32;
        while (id >= cap)
            cap *= 2;
        struct fd **n = realloc(st->by_id, cap * sizeof(*n));
        if (n == NULL)
            return _ENOMEM;
        memset(n + st->id_cap, 0, (cap - st->id_cap) * sizeof(*n));
        st->by_id = n;
        st->id_cap = cap;
    }
    if (id >= st->id_count)
        st->id_count = id + 1;
    st->by_id[id] = fd_retain(fd);
    return 0;
}

// The host's standard stream `n` (0, 1 or 2) for a CKPT_FD_STDIO record: made
// on first mention and then shared, the way a fork shares it, by every
// descriptor in the image that names that stream -- whichever process holds
// it and whatever terminal that process was on. NULL if it could not be made.
static struct fd *ckpt_host_stdio(struct ckpt_restore_state *st, uint64_t n) {
    if (n > 2)
        return NULL;
    if (st->host_stdio[n] == NULL)
        st->host_stdio[n] = open_host_stdio_copy((int) n);
    return st->host_stdio[n];
}

// ---- restored sessions ---------------------------------------------------

struct tty *(*checkpoint_open_session_tty)(void);

// Sessions that came back on a fresh pseudo-terminal, waiting for the UI to
// show them. Not part of ckpt_restore_state: the restore is over long before
// the first terminal view controller asks, and there may be several launches
// worth of view controllers.
static struct checkpoint_restored_session ckpt_sessions[8];
static unsigned ckpt_session_count, ckpt_session_taken;

// Every process exit, when ISH_CHECKPOINT_DEBUG is on. A restored guest that
// comes back and then quietly falls over is the failure mode this feature has
// most of: the image loads, every task starts, and a second later the ones
// that mattered are zombies with nothing anywhere saying why. The exit code is
// the first fact worth having.
void checkpoint_trace_exit(int pid, const char *comm, int status) {
    CKPT_TRACE("pid %d (%s) exited, status %#x\n", pid, comm, status);
}

// The first syscalls each RESTORED task makes, when ISH_CHECKPOINT_DEBUG is on.
// Bounded, because the point is the handful of calls between "the image was
// loaded" and "the process that mattered is a zombie" -- after that it is a
// running guest and this is just noise.
#define CKPT_SYSCALL_TRACE_LIMIT 20
void checkpoint_trace_syscall(unsigned long nr) {
    // The task test first and the environment second: this is on the syscall
    // path, and for everything that did not come back from an image the whole
    // cost is one load of a bool that is false.
    if (current == NULL || !current->ckpt_restored ||
            current->ckpt_syscalls_traced >= CKPT_SYSCALL_TRACE_LIMIT)
        return;
    if (!ckpt_debug())
        return;
    current->ckpt_syscalls_traced++;
    fprintf(stderr, "checkpoint: pid %d (%s) syscall %lu\n",
            current->pid, current->comm, nr);
}

int checkpoint_take_restored_session_for_pid(int leader_pid,
                                            struct checkpoint_restored_session *out) {
    int got = 0;
    lock(&ckpt_lock, 0);
    // Exact match first: the window that was showing this shell gets this
    // shell back, not whichever session happens to be next in the queue. Two
    // terminals coming back swapped is a small wrong that reads as a big one,
    // because one of them may be the Session Shell (the admin surface) while
    // the other is an ordinary login.
    for (unsigned i = ckpt_session_taken; i < ckpt_session_count; i++) {
        if (leader_pid > 0 && ckpt_sessions[i].leader_pid != leader_pid)
            continue;
        *out = ckpt_sessions[i];
        // Keep the queue contiguous: swap the one just taken to the front of
        // the untaken range, so the plain take() below stays correct.
        ckpt_sessions[i] = ckpt_sessions[ckpt_session_taken];
        ckpt_session_taken++;
        got = 1;
        break;
    }
    unsigned count = ckpt_session_count, taken = ckpt_session_taken;
    unlock(&ckpt_lock);
    CKPT_TRACE("UI asked for restored session pid %d: %s (%u of %u taken)\n",
               leader_pid, got ? "handed it over" : "no match", taken, count);
    if (got)
        return 1;
    // No session with that leader -- it may not have been saved, or this is a
    // window that had no session at all. Fall back to the queue order rather
    // than leaving the window empty.
    return checkpoint_take_restored_session(out);
}

int checkpoint_restored_session_pending(int leader_pid) {
    int pending = 0;
    lock(&ckpt_lock, 0);
    for (unsigned i = ckpt_session_taken; i < ckpt_session_count; i++) {
        if (leader_pid > 0 && ckpt_sessions[i].leader_pid == leader_pid) {
            pending = 1;
            break;
        }
    }
    unlock(&ckpt_lock);
    return pending;
}

int checkpoint_take_restored_session(struct checkpoint_restored_session *out) {
    int got = 0;
    lock(&ckpt_lock, 0);
    if (ckpt_session_taken < ckpt_session_count) {
        *out = ckpt_sessions[ckpt_session_taken++];
        got = 1;
    }
    unsigned count = ckpt_session_count, taken = ckpt_session_taken;
    unlock(&ckpt_lock);
    CKPT_TRACE("UI asked for a restored session: %s (%u of %u taken)\n",
               got ? "handed one over" : "none left", taken, count);
    return got;
}

// Whose terminal this is, which group is in the FOREGROUND of it, its line
// discipline and its size -- as the image's first process on it recorded them.
//
// Opening it made the first restored process to arrive its owner -- the login,
// never the shell it forked -- and a shell outside the foreground group reads
// EIO and exits, which is what every restored session did before this: back as
// a zombie within a millisecond. The line discipline is guarded on a plausible
// record rather than applied blindly: an image from before this travelled
// carries zeroes, and a terminal with no ECHO, no ICANON and no ISIG is one
// nothing can be typed into.
static void ckpt_restore_terminal(struct tty *tty, const struct ckpt_task *rec) {
    lock(&tty->lock, 0);
    if (rec->tty_session != 0)
        tty->session = rec->tty_session;
    if (rec->tty_fg_group != 0)
        tty->fg_group = rec->tty_fg_group;
    if (rec->tty_termios.lflags != 0 || rec->tty_termios.iflags != 0)
        tty->termios = rec->tty_termios;
    if (rec->tty_winsize.col != 0 && rec->tty_winsize.row != 0)
        tty->winsize = rec->tty_winsize;
    unlock(&tty->lock);
}

// The standard streams for one restored task, made on first sight of the
// terminal it was on and shared by everything else on that same terminal.
//
// A pty that cannot be made falls back to the console rather than to nothing:
// a restored process with no terminal at all reads EOF and exits, which is
// worse than one on a terminal nobody is watching. The CLI has no factory at
// all and takes that path deliberately -- its terminal IS the console.
static struct ckpt_stdio_set *ckpt_stdio_set_for(struct ckpt_restore_state *st,
        const struct ckpt_header *h, const struct ckpt_task *rec,
        struct fdtable *files) {
    uint32_t kind = rec->tty_kind;
    const char *path = rec->tty_path[0] == '/' ? rec->tty_path :
                       h->console[0] != '\0' ? h->console : "/dev/tty1";
    if (kind == CKPT_TTY_PTS && checkpoint_open_session_tty == NULL)
        kind = CKPT_TTY_CONSOLE, path = h->console[0] != '\0' ? h->console
                                                              : "/dev/tty1";

    for (uint32_t i = 0; i < st->set_count; i++) {
        struct ckpt_stdio_set *set = &st->sets[i];
        if (set->kind != kind)
            continue;
        if (kind == CKPT_TTY_PTS ? set->sid == rec->sid
                                 : strcmp(set->path, path) == 0)
            return set;
    }

    if (st->set_count == st->set_cap) {
        uint32_t cap = st->set_cap ? st->set_cap * 2 : 8;
        void *n = realloc(st->sets, cap * sizeof(*st->sets));
        if (n == NULL)
            return NULL;
        st->sets = n;
        st->set_cap = cap;
    }
    struct ckpt_stdio_set *set = &st->sets[st->set_count];
    memset(set, 0, sizeof(*set));
    set->kind = kind;
    set->sid = rec->sid;
    set->old_kind = rec->tty_kind;
    set->old_num = (int) rec->tty_num;
    snprintf(set->path, sizeof(set->path), "%s", path);

    // A pty the IMAGE owns -- tmux's pane, not the UI's window. Its master was
    // restored with an earlier task, so the pair already exists and the only
    // thing to do is point this process's standard streams at the slave. It
    // must NOT go through checkpoint_open_session_tty: that makes a window and
    // hands it to the UI, which is how a tmux pane came back as a terminal of
    // its own while tmux's master pointed at nothing.
    if (kind == CKPT_TTY_PTS) {
        int mapped = ckpt_pty_map_get(st, (int) rec->tty_num);
        if (mapped >= 0) {
            snprintf(set->path, sizeof(set->path), "/dev/pts/%d", mapped);
            set->tty_num = mapped;
            int err = create_stdio(set->path, TTY_PSEUDO_SLAVE_MAJOR, mapped);
            if (err < 0) {
                ckpt_refuse("could not attach restored pid %u to %s: %d",
                            rec->pid, set->path, err);
                return NULL;
            }
            // The terminal's state, exactly as for a window below: whose it
            // is, which group is in its FOREGROUND, its line discipline and
            // its size. None of it used to be put back here, and a tmux pane
            // came back with the shell's group in front instead of the job's,
            // in cooked mode under a program that had put it in raw mode, so
            // `q` never reached ktop and watch lost its pane to a prompt.
            // set->tty is what makes every other process of the session take
            // this terminal as its controlling one (ckpt_join_terminal); here,
            // too, only the process that opened it used to have one.
            struct tty *tty = fd_tty(current->files->files[0]);
            if (tty != NULL) {
                ckpt_restore_terminal(tty, rec);
                set->tty = tty;
            }
            // No set->terminal and no leader_pid: there is no window here for
            // the UI to adopt, and nothing whose exit ends one.
            CKPT_TRACE("pid %u came back on %s (pty the image owns, fg group %d)\n",
                       rec->pid, set->path, rec->tty_fg_group);
            st->set_count++;
            return set;
        }
    }
    if (kind == CKPT_TTY_PTS) {
        struct tty *tty = checkpoint_open_session_tty();
        if (tty == NULL || IS_ERR(tty)) {
            ckpt_refuse("could not make a terminal for restored session %u",
                        rec->sid);
            return NULL;
        }
        set->tty_num = tty->num;
        set->terminal = tty->data;
        set->tty = tty;
        snprintf(set->path, sizeof(set->path), "/dev/pts/%d", tty->num);
        int err = create_stdio(set->path, TTY_PSEUDO_SLAVE_MAJOR, tty->num);
        // create_stdio opened the node itself, so the reference pty_open_fake
        // handed over is ours to drop -- exactly what starting a session does.
        tty_release(tty);
        if (err < 0) {
            ckpt_refuse("could not attach restored session %u to %s: %d",
                        rec->sid, set->path, err);
            return NULL;
        }
        ckpt_restore_terminal(tty, rec);
        // The session LEADER is what the UI watches: when it exits the window
        // is finished, whatever else is still in the session. The image's
        // tasks arrive parents-first, so the first one on this terminal is it
        // -- and if the leader itself was not saved, the first survivor is the
        // honest answer to "whose exit ends this".
        set->leader_pid = (int) rec->pid;
        CKPT_TRACE("session %u came back on %s (leader pid %u, fg group %d)\n",
                   rec->sid, set->path, rec->pid, rec->tty_fg_group);
    } else if (kind == CKPT_TTY_CONSOLE) {
        create_stdio(set->path,
                     h->console[0] != '\0' && strcmp(set->path, h->console) == 0
                             ? (int) h->console_major : TTY_CONSOLE_MAJOR,
                     h->console[0] != '\0' && strcmp(set->path, h->console) == 0
                             ? (int) h->console_minor : rec->tty_num);
        CKPT_TRACE("pid %u came back on %s\n", rec->pid, set->path);
    } else {
        // No terminal: the host's own standard streams, the same descriptors
        // every CKPT_FD_STDIO record is given. Copies above 2
        // (open_host_stdio_copy says why), and put into no table here: the
        // table is filled record by record in ckpt_restore_task, and before
        // this used create_piped_stdio, which wrote into it -- the source of
        // three descriptors a process that had closed its standard streams
        // never had.
        for (unsigned i = 0; i < 3; i++) {
            set->stdio[i] = ckpt_host_stdio(st, i);
            if (set->stdio[i] != NULL)
                fd_retain(set->stdio[i]);   // the restore's own reference
        }
        CKPT_TRACE("pid %u came back with no terminal\n", rec->pid);
        st->set_count++;
        return set;
    }

    lock(&files->lock, 0);
    for (unsigned i = 0; i < 3; i++) {
        set->stdio[i] = i < files->size ? files->files[i] : NULL;
        if (set->stdio[i] != NULL)
            fd_retain(set->stdio[i]);   // the restore's own reference
    }
    unlock(&files->lock);
    st->set_count++;
    return set;
}

// The two ends of the pipe with this inode, created on first sight.
static int ckpt_pipe_for(struct ckpt_restore_state *st, uint64_t inode,
        struct fd **out_rd, struct fd **out_wr) {
    for (uint32_t i = 0; i < st->pipe_count; i++) {
        if (st->pipes[i].inode == inode) {
            *out_rd = st->pipes[i].rd;
            *out_wr = st->pipes[i].wr;
            return 0;
        }
    }
    int err = pipe_create_pair(out_rd, out_wr, inode);
    if (err < 0)
        return err;
    if (st->pipe_count == st->pipe_cap) {
        uint32_t cap = st->pipe_cap ? st->pipe_cap * 2 : 8;
        void *n = realloc(st->pipes, cap * sizeof(*st->pipes));
        if (n == NULL)
            return _ENOMEM;
        st->pipes = n;
        st->pipe_cap = cap;
    }
    st->pipes[st->pipe_count].inode = inode;
    st->pipes[st->pipe_count].rd = *out_rd;
    st->pipes[st->pipe_count].wr = *out_wr;
    st->pipe_count++;
    return 0;
}


// Build a descriptor with no file behind it from its description (see
// ckpt_describe_anon). A pidfd is made unbound and bound once every task
// exists: it usually names a child, and a child is restored after its parent.
static struct fd *ckpt_rebuild_anon(struct ckpt_restore_state *st,
        const struct ckpt_fd *cf, const char *payload) {
    size_t len = (size_t) cf->offset;
    switch (cf->kind) {
    case CKPT_FD_EPOLL:
        return epoll_ckpt_new();
    case CKPT_FD_EVENTFD: {
        struct ckpt_eventfd_desc d;
        if (len != sizeof(d))
            return ERR_PTR(_EINVAL);
        memcpy(&d, payload, sizeof(d));
        return eventfd_ckpt_new(d.val, d.semaphore != 0);
    }
    case CKPT_FD_SIGNALFD: {
        uint64_t mask;
        if (len != sizeof(mask))
            return ERR_PTR(_EINVAL);
        memcpy(&mask, payload, sizeof(mask));
        return signalfd_ckpt_new(mask);
    }
    case CKPT_FD_TIMERFD: {
        struct timerfd_ckpt d;
        if (len != sizeof(d))
            return ERR_PTR(_EINVAL);
        memcpy(&d, payload, sizeof(d));
        return timerfd_ckpt_new(&d);
    }
    case CKPT_FD_INOTIFY:
        return inotify_ckpt_new(payload, len);
    case CKPT_FD_MEMFD:
        return memfd_ckpt_new(payload, len);
    case CKPT_FD_PIDFD: {
        int32_t pid;
        if (len != sizeof(pid))
            return ERR_PTR(_EINVAL);
        memcpy(&pid, payload, sizeof(pid));
        struct fd *pidfd = pidfd_ckpt_new_unbound();
        if (IS_ERR(pidfd) || pid <= 0)
            return pidfd;   // pid 0: the process was already gone, and stays so
        if (st->pidfd_count == st->pidfd_cap) {
            uint32_t cap = st->pidfd_cap ? st->pidfd_cap * 2 : 8;
            void *n = realloc(st->pidfds, cap * sizeof(*st->pidfds));
            if (n == NULL) {
                fd_close(pidfd);
                return ERR_PTR(_ENOMEM);
            }
            st->pidfds = n;
            st->pidfd_cap = cap;
        }
        st->pidfds[st->pidfd_count].fd = pidfd;
        st->pidfds[st->pidfd_count].pid = pid;
        st->pidfd_count++;
        return pidfd;
    }
    default:
        return ERR_PTR(_EINVAL);
    }
}

static bool ckpt_kind_is_anon(uint32_t kind) {
    return kind >= CKPT_FD_EPOLL && kind <= CKPT_FD_MEMFD;
}

// The epoll registrations, once every descriptor in the image exists. One
// that cannot be put back is said, and stepped over: the set still works for
// everything else it was watching.
static int ckpt_restore_epoll_regs(FILE *f, const struct ckpt_header *h,
                                   struct ckpt_restore_state *st) {
    unsigned added = 0, failed = 0;
    for (uint32_t i = 0; i < h->n_epoll_regs; i++) {
        struct ckpt_epoll_reg r;
        int err = rd(f, &r, sizeof(r));
        if (err < 0)
            return err;
        struct fd *ep = r.epoll_id < st->id_count ? st->by_id[r.epoll_id] : NULL;
        struct fd *target = r.target_id < st->id_count ? st->by_id[r.target_id] : NULL;
        if (ep == NULL || target == NULL || !epoll_fd_is(ep)) {
            failed++;
            continue;
        }
        err = epoll_ckpt_add(ep, target, r.guest_fd, (int) r.types, r.data);
        if (err < 0) {
            printk("WARNING: checkpoint: epoll registration of fd %d could not "
                   "be put back (%d)\n", r.guest_fd, -err);
            failed++;
        } else {
            added++;
        }
    }
    CKPT_TRACE("epoll: %u registrations put back, %u not\n", added, failed);
    return 0;
}

// Every pidfd bound to the process it names, now that the process exists.
// One whose process did not come back stays unbound, which is the answer
// Linux gives for a process that is gone -- readable, ESRCH to signal -- and
// the same thing its holder would have seen had the process died on its own.
// A zombie counts as there: a pidfd on one is how its parent may reap it.
static void ckpt_restore_pidfds(struct ckpt_restore_state *st) {
    for (uint32_t i = 0; i < st->pidfd_count; i++) {
        struct task *task = pid_get_task_zombie_ref((dword_t) st->pidfds[i].pid);
        if (task == NULL) {
            printk("checkpoint: a pidfd named pid %d, which did not come back; "
                   "it reads as a process that has gone\n", st->pidfds[i].pid);
            continue;
        }
        pidfd_ckpt_bind(st->pidfds[i].fd, task);
        task_ref_cnt_mod(task, -1);
    }
}


static struct fifo_file *ckpt_fd_fifo(struct fd *fd) {
    struct fifo_file *fifo = tmpfs_fd_fifo(fd);
    return fifo != NULL ? fifo : fakefs_fd_fifo(fd);
}

// A named FIFO, reopened by path. Never blocking to do it: a FIFO open waits
// for the other end, which may be restored later or not at all, so it is
// opened O_NONBLOCK and given its own flags back afterwards. A write-only end
// cannot be opened that way without a reader (ENXIO), so it is attached
// read-write for now and re-attached as it was once every task exists -- the
// reader and writer counts, which decide EOF and EPIPE, end up exactly right.
static struct fd *ckpt_reopen_fifo(struct ckpt_restore_state *st,
        const struct ckpt_fd *cf, const char *path, const char *payload) {
    int access = (int) cf->flags & (O_WRONLY_ | O_RDWR_);
    int flags = ckpt_reopen_flags(cf->flags) | O_NONBLOCK_;
    bool deferred = false;
    struct fd *fd = generic_open(path, flags, 0);
    if (IS_ERR(fd) && PTR_ERR(fd) == _ENXIO && access == O_WRONLY_) {
        fd = generic_open(path, (flags & ~O_WRONLY_) | O_RDWR_, 0);
        deferred = true;
    }
    if (IS_ERR(fd))
        return fd;
    struct fifo_file *fifo = ckpt_fd_fifo(fd);
    if (fifo != NULL && cf->offset != 0) {
        bool primed = false;
        for (uint32_t i = 0; i < st->fifos_primed_count; i++)
            primed |= st->fifos_primed[i] == fifo;
        if (!primed) {
            if (st->fifos_primed_count == st->fifos_primed_cap) {
                uint32_t cap = st->fifos_primed_cap ? st->fifos_primed_cap * 2 : 8;
                void *n = realloc(st->fifos_primed, cap * sizeof(*st->fifos_primed));
                if (n == NULL) {
                    fd_close(fd);
                    return ERR_PTR(_ENOMEM);
                }
                st->fifos_primed = n;
                st->fifos_primed_cap = cap;
            }
            st->fifos_primed[st->fifos_primed_count++] = fifo;
            int err = fifo_file_prime(fifo, payload, (size_t) cf->offset);
            if (err < 0)
                printk("WARNING: checkpoint: %s had %llu bytes in it that could "
                       "not be put back (%d)\n", path,
                       (unsigned long long) cf->offset, -err);
        }
    }
    if (deferred) {
        if (st->fifo_writer_count == st->fifo_writer_cap) {
            uint32_t cap = st->fifo_writer_cap ? st->fifo_writer_cap * 2 : 8;
            void *n = realloc(st->fifo_writers, cap * sizeof(*st->fifo_writers));
            if (n == NULL) {
                fd_close(fd);
                return ERR_PTR(_ENOMEM);
            }
            st->fifo_writers = n;
            st->fifo_writer_cap = cap;
        }
        st->fifo_writers[st->fifo_writer_count].fd = fd;
        st->fifo_writers[st->fifo_writer_count].flags = cf->flags;
        st->fifo_writer_count++;
    } else {
        fd->flags = (int) cf->flags;
    }
    return fd;
}

// The write-only FIFO ends, re-attached as write-only now that their readers
// exist. One whose FIFO has no reader in the image at all stays read-write --
// a writer whose reader had already gone -- and is said: the one thing it
// will not do is fail its next write with EPIPE, as it would have.
static void ckpt_restore_fifos(struct ckpt_restore_state *st) {
    for (uint32_t i = 0; i < st->fifo_writer_count; i++) {
        struct fd *fd = st->fifo_writers[i].fd;
        struct fifo_file *fifo = ckpt_fd_fifo(fd);
        if (fifo == NULL)
            continue;
        int rdwr = fd->flags;
        fifo_file_close(fifo, fd);
        fd->flags = ((int) st->fifo_writers[i].flags & ~O_ACCMODE_) | O_WRONLY_ | O_NONBLOCK_;
        if (fifo_file_open(fifo, fd) < 0) {
            fd->flags = rdwr;
            fifo_file_open(fifo, fd);
            printk("WARNING: checkpoint: a FIFO writer came back with no reader; "
                   "it is attached read-write\n");
            continue;
        }
        fd->flags = (int) st->fifo_writers[i].flags;
    }
}

// ---- terminal descriptions ------------------------------------------------
//
// A terminal record (CKPT_FD_TTY) is one DESCRIPTION of a terminal the image
// does not own -- a window's pty, or the console. Each comes back as the
// description it was: on the terminal it names, with its own flags.
//
// There used to be one descriptor per terminal, handed to every terminal
// record of every process that used it, and chosen by the HOLDER's own
// terminal rather than by the record's. Three things went wrong with that:
//
//   - flags were never put back, so a description a program had made
//     non-blocking came back blocking. tmux makes the terminal of every client
//     it serves non-blocking, and a server that blocks writing to a terminal
//     nobody is draining stops serving every pane;
//   - a program's separate open of its terminal (less, vi and ssh open
//     /dev/tty) was merged into the session's shared description, flags and
//     all;
//   - a descriptor on a terminal that is not its holder's own -- the tmux
//     server holds every attached client's -- was given the holder's own
//     standard streams: another terminal, or none at all.

// The set a pty record names, by the number the image knew it by. Two sets can
// name one terminal (a process that called setsid and stayed on its parent's
// terminal is given one of its own); the holder's own session wins the tie.
static struct ckpt_stdio_set *ckpt_set_for_pts(struct ckpt_restore_state *st,
        int num, uint32_t sid) {
    struct ckpt_stdio_set *any = NULL;
    for (uint32_t i = 0; i < st->set_count; i++) {
        struct ckpt_stdio_set *set = &st->sets[i];
        if (set->old_kind != CKPT_TTY_PTS || set->old_num != num)
            continue;
        if (set->sid == sid)
            return set;
        if (any == NULL)
            any = set;
    }
    return any;
}

// The descriptor a terminal description comes back as, on this set's terminal:
// the set's own if the description is the one it stands for -- the first one
// mapped onto it claims it, and its flags are put on it -- and otherwise a
// descriptor of its own, opened on the same terminal with its own flags.
// O_NOCTTY: which process has the terminal as its controlling one is restored
// separately (ckpt_join_terminal), and an open must not decide it. Returns a
// new reference, or NULL with nothing to give.
static struct fd *ckpt_tty_description(struct ckpt_stdio_set *set, uint32_t id,
        int flags) {
    struct fd *primary = set->stdio[0];
    if (primary != NULL && fd_tty(primary) != NULL &&
            (!set->claimed || set->primary_id == id)) {
        if (!set->claimed) {
            set->claimed = true;
            set->primary_id = id;
            fd_setflags(primary, flags);
        }
        return fd_retain(primary);
    }
    struct fd *fd = generic_open(set->path, ckpt_reopen_flags((uint32_t) flags) | O_NOCTTY_, 0);
    if (!IS_ERR(fd))
        return fd;
    // Could not open it again. Share the set's own rather than lose the
    // descriptor, which is what every terminal record used to get.
    CKPT_TRACE("  terminal %s could not be opened again (%d); sharing its "
               "session's descriptor\n", set->path, (int) PTR_ERR(fd));
    return primary != NULL ? fd_retain(primary) : NULL;
}

static int ckpt_defer_tty(struct ckpt_restore_state *st, uint32_t id, int num,
        uint32_t sid, int flags, struct task *task, uint32_t fd, bool cloexec,
        bool head) {
    if (st->pending_tty_count == st->pending_tty_cap) {
        uint32_t cap = st->pending_tty_cap ? st->pending_tty_cap * 2 : 8;
        void *n = realloc(st->pending_ttys, cap * sizeof(*st->pending_ttys));
        if (n == NULL)
            return _ENOMEM;
        st->pending_ttys = n;
        st->pending_tty_cap = cap;
    }
    st->pending_ttys[st->pending_tty_count++] = (struct ckpt_pending_tty) {
        .id = id, .num = num, .sid = sid, .flags = flags, .task = task,
        .fd = fd, .cloexec = cloexec, .head = head,
    };
    return 0;
}

// Whether `fd` in `task` will be filled once every task exists: empty now,
// but not free.
static bool ckpt_fd_pending(struct ckpt_restore_state *st, struct task *task,
        fd_t fd) {
    for (uint32_t i = 0; i < st->pending_tty_count; i++)
        if (st->pending_ttys[i].task == task &&
                st->pending_ttys[i].fd == (uint32_t) fd)
            return true;
    return false;
}

static bool ckpt_tty_pending(struct ckpt_restore_state *st, uint32_t id) {
    for (uint32_t i = 0; i < st->pending_tty_count; i++)
        if (st->pending_ttys[i].head && st->pending_ttys[i].id == id)
            return true;
    return false;
}

// One CKPT_FD_TTY record, for the task being restored (`current`), whose own
// terminal set is `own`. Installed now, or deferred until every task exists if
// the terminal it names has not been rebuilt yet.
static int ckpt_restore_tty_record(struct ckpt_restore_state *st,
        struct ckpt_stdio_set *own, const struct ckpt_task *rec,
        const struct ckpt_fd *cf, const char *path, struct fdtable *files) {
    uint32_t type = (uint32_t) (cf->offset >> 32);
    int num = (int) (uint32_t) cf->offset;
    struct fd *fd = NULL;
    if (cf->offset != 0 && type == TTY_PSEUDO_SLAVE_MAJOR) {
        struct ckpt_stdio_set *target = ckpt_set_for_pts(st, num, rec->sid);
        if (target == NULL) {
            CKPT_TRACE("  fd %u: pts %d is not back yet; after every task\n",
                       cf->fd, num);
            return ckpt_defer_tty(st, cf->id, num, rec->sid, (int) cf->flags,
                                  current, cf->fd, cf->cloexec != 0, true);
        }
        fd = ckpt_tty_description(target, cf->id, (int) cf->flags);
    } else if (cf->offset != 0 && (own == NULL || own->kind != CKPT_TTY_CONSOLE) &&
            path[0] == '/') {
        // A console that is not this process's own terminal: its device path
        // is the same in every run, so it is simply opened again.
        fd = generic_open(path, ckpt_reopen_flags(cf->flags) | O_NOCTTY_, 0);
        if (IS_ERR(fd))
            fd = NULL;
    }
    if (fd == NULL && own != NULL)
        fd = ckpt_tty_description(own, cf->id, (int) cf->flags);
    if (fd == NULL)
        return 0;
    int err = ckpt_id_put(st, cf->id, fd);
    if (err == 0)
        err = fdtable_install_at(files, (fd_t) cf->fd, fd, cf->cloexec != 0);
    else
        fd_close(fd);
    return err;
}

// The ids a pending descriptor is reopened under: root's, as every restored
// descriptor is (see ckpt_restore_task), and then its holder's own again.
struct ckpt_ids { uid_t_ uid, euid, suid, fsuid, gid, egid, sgid, fsgid; };

static struct ckpt_ids ckpt_ids_become_root(void) {
    struct ckpt_ids ids = {
        current->uid, current->euid, current->suid, current->fsuid,
        current->gid, current->egid, current->sgid, current->fsgid,
    };
    current->uid = current->euid = current->suid = current->fsuid = 0;
    current->gid = current->egid = current->sgid = current->fsgid = 0;
    return ids;
}

static void ckpt_ids_restore(struct ckpt_ids ids) {
    current->uid = ids.uid; current->euid = ids.euid;
    current->suid = ids.suid; current->fsuid = ids.fsuid;
    current->gid = ids.gid; current->egid = ids.egid;
    current->sgid = ids.sgid; current->fsgid = ids.fsgid;
}

// `fd` into every descriptor that waited for head's description: the head's
// own, and each CKPT_FD_REF to it. The caller's reference is given up.
static void ckpt_install_pending(struct ckpt_restore_state *st,
        const struct ckpt_pending_tty *head, struct fd *fd, const char *what) {
    ckpt_id_put(st, head->id, fd);
    for (uint32_t j = 0; j < st->pending_tty_count; j++) {
        struct ckpt_pending_tty *e = &st->pending_ttys[j];
        if (e->id != head->id)
            continue;
        // Takes the reference whether it succeeds or not.
        fdtable_install_at(e->task->files, (fd_t) e->fd, fd_retain(fd),
                           e->cloexec);
        CKPT_TRACE("  pid %d fd %u: back on %s\n", e->task->pid, e->fd, what);
    }
    fd_close(fd);
}

// A pty slave whose master was not back when its record was read. Every
// master in the image is back now, so one with no entry in the map was not in
// the image, and the slave falls back to its holder's terminal as it always
// did.
static void ckpt_restore_pending_pty_slave(struct ckpt_restore_state *st,
        const struct ckpt_pending_tty *head) {
    char path[64] = "its holder's terminal";
    struct fd *fd = NULL;
    int mapped = ckpt_pty_map_get(st, head->num);
    if (mapped >= 0) {
        char pts[32];
        snprintf(pts, sizeof(pts), "/dev/pts/%d", mapped);
        struct task *saved = current;
        current = head->task;
        struct ckpt_ids ids = ckpt_ids_become_root();
        // O_NOCTTY: every session has its terminal by now (ckpt_join_terminal),
        // and a descriptor coming back must not make this one the holder's.
        fd = generic_open(pts, ckpt_reopen_flags((uint32_t) head->flags) | O_NOCTTY_, 0);
        ckpt_ids_restore(ids);
        if (IS_ERR(fd)) {
            CKPT_TRACE("  pid %d fd %u: %s could not be opened again (%d)\n",
                       head->task->pid, head->fd, pts, (int) PTR_ERR(fd));
            fd = NULL;
        } else {
            fd_open_creds_stamp(fd);
            snprintf(path, sizeof(path), "%s", pts);
        }
        current = saved;
    }
    if (fd == NULL) {
        struct ckpt_stdio_set *set = head->set < st->set_count ? &st->sets[head->set] : NULL;
        fd = set != NULL ? set->stdio[head->fd <= 2 ? head->fd : 0] : NULL;
        if (fd == NULL)
            return;
        fd_retain(fd);
    }
    ckpt_install_pending(st, head, fd, path);
}

// See ckpt_restore_state's pending_ttys. Opened as every restored descriptor
// is -- with the authority it was first opened with, root's here -- and then
// stamped as its holder's own.
static void ckpt_restore_pending_ttys(struct ckpt_restore_state *st) {
    for (uint32_t i = 0; i < st->pending_tty_count; i++) {
        struct ckpt_pending_tty *head = &st->pending_ttys[i];
        if (!head->head)
            continue;
        if (head->pty_slave) {
            ckpt_restore_pending_pty_slave(st, head);
            continue;
        }
        struct ckpt_stdio_set *target = ckpt_set_for_pts(st, head->num, head->sid);
        struct task *saved = current;
        current = head->task;
        struct ckpt_ids ids = ckpt_ids_become_root();
        struct fd *fd = target != NULL
                ? ckpt_tty_description(target, head->id, head->flags) : NULL;
        bool opened_here = fd != NULL && fd != target->stdio[0];
        if (fd == NULL) {
            // The terminal is not in the image -- its session did not come
            // back -- so the descriptor comes back on nothing, which is what
            // a terminal that has gone away reads as.
            struct fd *null = generic_open("/dev/null", O_RDWR_, 0);
            fd = IS_ERR(null) ? NULL : null;
            opened_here = fd != NULL;
            CKPT_TRACE("  pid %d fd %u: pts %d did not come back; /dev/null\n",
                       head->task->pid, head->fd, head->num);
        }
        ckpt_ids_restore(ids);
        if (opened_here)
            fd_open_creds_stamp(fd);
        current = saved;
        if (fd == NULL)
            continue;
        char what[32];
        snprintf(what, sizeof(what), "pts %d", head->num);
        ckpt_install_pending(st, head, fd, what);
    }
}

// Everything on a terminal joins the session that owns it. A tgroup carries
// its controlling terminal across fork (kernel/fork.c), so the shell had one
// before the suspend; only the process that re-opened the terminal gets one
// back on its own.
static void ckpt_join_terminal(struct ckpt_stdio_set *set) {
    if (set->tty == NULL)
        return;
    lock(&current->group->lock, 0);
    if (current->group->tty == NULL) {
        lock(&set->tty->lock, 0);
        set->tty->refcount++;
        unlock(&set->tty->lock);
        current->group->tty = set->tty;
    }
    unlock(&current->group->lock);
}

// Put queued signals back on a queue, oldest first, as they were, and return
// the pending set that goes with them: a signal is pending exactly when one of
// its is queued, so a bit is never set with nothing behind it.
static sigset_t_ ckpt_requeue_signals(struct list *queue, const struct ckpt_sigqueue *saved,
        uint32_t n) {
    sigset_t_ pending = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (saved[i].info.sig < 1 || saved[i].info.sig >= NUM_SIGS)
            continue;
        struct sigqueue *q = malloc(sizeof(*q));
        if (q == NULL)
            break;
        q->info = saved[i].info;
        q->from_timer = saved[i].from_timer != 0;
        list_add_tail(queue, &q->queue);
        sigset_add(&pending, saved[i].info.sig);
    }
    return pending;
}

// What follows a task's descriptors (struct ckpt_task's n_sigqueue and on):
// its queued signals, its process's, and its process's timers. Read onto
// `current`, which is the task being restored.
static int ckpt_restore_signals_and_timers(FILE *f, const struct ckpt_task *rec,
        struct ckpt_restore_state *st) {
    int err = 0;
    if (rec->n_sigqueue > CKPT_SIGQUEUE_MAX || rec->n_group_sigqueue > CKPT_SIGQUEUE_MAX)
        return _EINVAL;
    struct ckpt_sigqueue *own = NULL, *group = NULL;
    if (rec->n_sigqueue != 0 &&
            (own = malloc(rec->n_sigqueue * sizeof(*own))) == NULL)
        return _ENOMEM;
    if (rec->n_group_sigqueue != 0 &&
            (group = malloc(rec->n_group_sigqueue * sizeof(*group))) == NULL) {
        free(own);
        return _ENOMEM;
    }
    if ((err = rd(f, own, rec->n_sigqueue * sizeof(*own))) < 0 ||
            (err = rd(f, group, rec->n_group_sigqueue * sizeof(*group))) < 0) {
        free(own);
        free(group);
        return err;
    }
    // A native program's set comes back as its record gives it (identity:),
    // there being no queue of its in the image.
    struct sighand *sighand = current->sighand;
    lock(&sighand->lock, 0);
    sigset_t_ pending = ckpt_requeue_signals(&current->queue, own, rec->n_sigqueue);
    if (!rec->native)
        current->pending = pending;
    // The process's belong to the handlers' owner, whose record this is; a
    // task sharing them was given them with its owner.
    if (rec->sighand_owner == 0 && !rec->native)
        sighand->pending = ckpt_requeue_signals(&sighand->queue, group,
                                                rec->n_group_sigqueue);
    unlock(&sighand->lock);
    free(own);
    free(group);

    if (rec->group_timers) {
        struct group_timers_ckpt d;
        if ((err = rd(f, &d, sizeof(d))) < 0)
            return err;
        if (d.n_posix > TIMERS_MAX)
            return _EINVAL;
        struct posix_timer_ckpt *posix = calloc(d.n_posix != 0 ? d.n_posix : 1, sizeof(*posix));
        if (posix == NULL)
            return _ENOMEM;
        if ((err = rd(f, posix, d.n_posix * sizeof(*posix))) < 0) {
            free(posix);
            return err;
        }
        if (st->timers_count == st->timers_cap) {
            uint32_t cap = st->timers_cap ? st->timers_cap * 2 : 8;
            void *n = realloc(st->timers, cap * sizeof(*st->timers));
            if (n == NULL) {
                free(posix);
                return _ENOMEM;
            }
            st->timers = n;
            st->timers_cap = cap;
        }
        st->timers[st->timers_count++] = (struct ckpt_group_timers) {
            .group = current->group,
            .pid = rec->pid,
            .d = d,
            .posix = posix,
        };
    }

    // The deadline the call it was parked in had left, back on this run's
    // host clock, and whether a handler cancels that call's restart.
    if (rec->sleep_restart_valid) {
        current->sleep_restart_deadline = ckpt_host_deadline_after(timer_ckpt_left(
                timer_ckpt_clock_for(rec->sleep_restart_clock, false),
                rec->sleep_restart_value_ns));
        current->sleep_restart_clock = rec->sleep_restart_clock;
        current->sleep_restart_valid = true;
    }
    if (rec->poll_restart_valid) {
        current->poll_restart_deadline = ckpt_host_deadline_after(timer_ckpt_left(
                TIMER_CKPT_MONOTONIC, rec->poll_restart_value_ns));
        current->poll_restart_valid = true;
    }
    current->restart_nohand_pending = (rec->restart_pending & 1) != 0;
    current->restart_sys_pending = (rec->restart_pending & 2) != 0;
    if (rec->n_sigqueue != 0 || rec->n_group_sigqueue != 0 || rec->group_timers ||
            rec->sleep_restart_valid || rec->poll_restart_valid)
        CKPT_TRACE("  load pid %u: %u signals queued, %u for the process%s%s%s\n",
                   rec->pid, rec->n_sigqueue, rec->n_group_sigqueue,
                   rec->group_timers ? ", its process's timers" : "",
                   rec->sleep_restart_valid ? ", a sleep's deadline" : "",
                   rec->poll_restart_valid ? ", a poll's deadline" : "");
    return 0;
}

static int ckpt_restore_task(FILE *f, const struct ckpt_header *h,
        const struct ckpt_task *rec, struct ckpt_restore_state *st) {
    int err;
    bool thread = rec->tgid != 0 && rec->tgid != rec->pid;
    struct fdtable *files;
    char cwd[MAX_PATH + 1] = {0}, root[MAX_PATH + 1] = {0}, exe[MAX_PATH + 1] = {0};
    if ((err = rd(f, cwd, rec->cwd_len)) < 0) return err;
    if ((err = rd(f, root, rec->root_len)) < 0) return err;
    if ((err = rd(f, exe, rec->exe_len)) < 0) return err;
    // The supplementary groups, installed as soon as they are read: the task
    // owns them from here, so a restore that fails further on frees them with
    // it. Nothing below is decided by them -- the restore reopens files as
    // root, and root's access does not depend on its groups.
    if (rec->ngroups > 0) {
        uid_t_ *groups = calloc(rec->ngroups, sizeof(*groups));
        if (groups == NULL)
            return _ENOMEM;
        if ((err = rd(f, groups, rec->ngroups * sizeof(*groups))) < 0) {
            free(groups);
            return err;
        }
        free(current->groups);
        current->groups = groups;
    } else {
        free(current->groups);
        current->groups = NULL;
    }
    current->ngroups = rec->ngroups;

    // seccomp, installed as soon as it is read, as the groups are: the task
    // owns the chain from here, and a restore that fails further on frees it
    // with the task. Nothing below is filtered by it -- only a syscall's entry
    // asks, and the restore makes none as this task.
    if ((rec->seccomp_mode == SECCOMP_MODE_FILTER_) != (rec->seccomp_nprogs > 0) ||
            rec->seccomp_mode > SECCOMP_MODE_FILTER_)
        return _EINVAL;
    struct seccomp_filter *seccomp = NULL;
    if (rec->seccomp_nprogs > 0) {
        struct seccomp_ckpt_prog *progs = calloc(rec->seccomp_nprogs, sizeof(*progs));
        if (progs == NULL)
            return _ENOMEM;
        unsigned got = 0;
        err = 0;
        for (; got < rec->seccomp_nprogs; got++) {
            struct ckpt_seccomp_prog hdr;
            if ((err = rd(f, &hdr, sizeof(hdr))) < 0)
                break;
            // A classic BPF program is 1..4096 instructions; the import
            // checks the rest as an installing seccomp() would.
            if (hdr.len == 0 || hdr.len > 4096) {
                err = _EINVAL;
                break;
            }
            void *insns = malloc((size_t) hdr.len * 8);
            if (insns == NULL) {
                err = _ENOMEM;
                break;
            }
            progs[got] = (struct seccomp_ckpt_prog) {hdr.log != 0, hdr.len, insns};
            if ((err = rd(f, insns, (size_t) hdr.len * 8)) < 0) {
                got++;
                break;
            }
        }
        if (err == 0) {
            seccomp = seccomp_ckpt_import(progs, rec->seccomp_nprogs);
            if (seccomp == NULL)
                err = _EINVAL;
        }
        for (unsigned i = 0; i < got && i < rec->seccomp_nprogs; i++)
            free((void *) progs[i].insns);
        free(progs);
        if (err < 0)
            return err;
    }
    seccomp_filter_release(current->seccomp_filter);
    current->seccomp_filter = seccomp;
    __atomic_store_n(&current->seccomp_mode, (int) rec->seccomp_mode, __ATOMIC_RELEASE);
    // The whole process's, so every thread's record carries the same value.
    atomic_store(&current->group->undumpable, rec->undumpable != 0);
    // As with undumpable, every thread's record carries the process's value.
    lock(&current->group->lock, 0);
    current->group->personality = rec->personality;
    unlock(&current->group->lock);

    // A NATIVE task: the program's name, the argv it had, and the state it
    // produced about itself. No register file and no address space follow --
    // it is not being photographed, it is being told to come back and rebuild
    // itself, which is the only thing a C function on a host thread can do.
    char *native_name = NULL, *native_argv = NULL, *native_state = NULL;
    char *native_env = NULL;
    if (rec->native) {
        native_name = calloc(rec->native_name_len + 1, 1);
        native_argv = calloc(rec->native_argv_len + 1, 1);
        native_state = calloc(rec->native_state_len + 1, 1);
        native_env = calloc(rec->native_env_len + 1, 1);
        if (native_name == NULL || native_argv == NULL ||
                native_state == NULL || native_env == NULL)
            err = _ENOMEM;
        else if ((err = rd(f, native_name, rec->native_name_len)) >= 0 &&
                 (err = rd(f, native_argv, rec->native_argv_len)) >= 0 &&
                 (err = rd(f, native_state, rec->native_state_len)) >= 0)
            err = rd(f, native_env, rec->native_env_len);
        if (err < 0) {
            free(native_name); free(native_argv);
            free(native_state); free(native_env);
            return err;
        }
        st->native_name = native_name;
        st->native_argv = native_argv;
        st->native_argv_len = rec->native_argv_len;
        st->native_state = native_state;
        st->native_env = native_env;
        st->native_env_len = rec->native_env_len;
        st->native_standin_child = rec->native_standin_child;
        // Its session and process group, which the jump below would otherwise
        // skip along with everything that really is emulator-only. Without
        // them the re-launched program kept the brand-new session
        // ckpt_new_task gave it -- sid == its own pid -- and was on the
        // terminal as a session LEADER. When it exited, exit_hangup_session_tty
        // did what a leader's exit does and took the session off the terminal,
        // so the shell that had been waiting on it got ENOTTY from tcsetpgrp:
        // "can't set tty process group: Not a tty", no prompt after ktop, and
        // every external command after that finishing Done(2) with no output.
        tgroup_restore_ids(current, (pid_t_) rec->sid, (pid_t_) rec->pgid);
        goto descriptors;
    }

    struct cpu_state cpu;
    if ((err = rd(f, &cpu, sizeof(cpu))) < 0) return err;
    struct sigaction_ actions[NUM_SIGS];
    if ((err = rd(f, actions, sizeof(actions))) < 0) return err;
    rlim_t_ limits[sizeof(current->group->limits) / sizeof(current->group->limits[0])][2];
    if ((err = rd(f, limits, sizeof(limits))) < 0) return err;

    // A thread's group -- its limits, its session and process group -- is its
    // leader's, restored with the leader and joined by ckpt_new_task. Only a
    // group's first task puts them back; a second pass over the membership
    // would link the group into its session twice.
    if (!thread) {
        // The limits first, because RLIMIT_NOFILE gates how many descriptors
        // can be installed below and the image's value is the one that was in
        // force.
        lock(&current->group->lock, 0);
        memcpy(current->group->limits, limits, sizeof(limits));
        bool cpu_limited = current->group->limits[RLIMIT_CPU_].cur != RLIM_INFINITY_;
        unlock(&current->group->lock);
        // Enforced by a sampler, which the memcpy does not start.
        if (cpu_limited)
            cpu_limit_watch(current->group);
        // The session and the process group, as MEMBERSHIP and not just as
        // two numbers -- kernel/group.c says why the fields alone were not
        // enough.
        tgroup_restore_ids(current, (pid_t_) rec->sid, (pid_t_) rec->pgid);
    }

    // The guest architecture, and with it the address space's shape. A fresh
    // task's mm is built for the entry point's default; the image says what
    // this process actually was, and every mapping below depends on it. Set
    // before a single page is mapped.
    current->abi = (enum guest_abi) rec->abi;
    // An address space shared with an earlier task was filled by that task's
    // record, and this one has no maps.
    if (rec->mm_owner != 0)
        goto descriptors;
    struct mem *mem = current->mem;
    struct mm *mm = current->mm;
    mem_set_page_limit(mem, (page_t) rec->page_limit);
    mem_set_mmap_window(mem, (page_t) rec->mmap_floor, (page_t) rec->mmap_ceiling);
    mem_set_stack_bounds(mem, (page_t) rec->stack_top,
                         (uint64_t) rec->stack_limit_pages << PAGE_BITS);

    for (uint32_t i = 0; i < rec->n_maps; i++) {
        struct ckpt_map m;
        if ((err = rd(f, &m, sizeof(m))) < 0)
            return err;
        page_t start_page = (page_t) (m.start >> PAGE_BITS);
        if (m.kind == CKPT_MAP_RESERVED) {
            // Reserved again, not materialised: see enum ckpt_map_kind. Any
            // size, since a fault or a split can leave one below the threshold
            // a fresh mmap needs. Records never overlap, so this cannot split
            // or clear anything the image put back; failing that (a table
            // somehow full) the pages are still owed, and get entries.
            write_lock(&mem->lock);
            // pt_unmap_always also rejects a range outside the address space,
            // which a reservation would otherwise record unchecked.
            err = pt_unmap_always(mem, start_page, (pages_t) m.pages) < 0 ? _ENOMEM : 0;
            if (err == 0 && !mem_lazy_reserve_any_size(mem, start_page, (pages_t) m.pages, m.flags))
                err = pt_map_nothing(mem, start_page, (pages_t) m.pages, m.flags);
            write_unlock(&mem->lock);
            CKPT_TRACE("  load reservation %#llx +%llu pages flags %#x%s\n",
                       (unsigned long long) m.start, (unsigned long long) m.pages,
                       m.flags, err < 0 ? " (failed)" : "");
            if (err < 0)
                return err;
            continue;
        }
        if (m.kind != CKPT_MAP_PAGES)
            return _EINVAL;
        write_lock(&mem->lock);
        // A fresh mm is not empty -- mm_new maps the vdso -- and the image is
        // the complete truth about this address space, so anything already
        // sitting where a mapping goes is replaced rather than collided with.
        pt_unmap_always(mem, start_page, (pages_t) m.pages);
        // Mapped WRITABLE regardless of the saved protection, then set to the
        // saved flags once the bytes are in: a PROT_NONE guard page or a
        // read-only text segment cannot be filled through mem_ptr otherwise.
        err = pt_map_nothing(mem, start_page, (pages_t) m.pages,
                             P_READ | P_WRITE | P_ANONYMOUS);
        write_unlock(&mem->lock);
        if (err < 0)
            return err;
        for (uint64_t pg = 0; pg < m.pages; pg++) {
            guest_addr_t addr = ((guest_addr_t) (start_page + pg)) << PAGE_BITS;
            write_lock(&mem->lock);
            char *dst = mem_ptr(mem, addr, MEM_WRITE);
            write_unlock(&mem->lock);
            if (dst == NULL)
                return _EFAULT;
            if ((err = rd(f, dst, PAGE_SIZE)) < 0)
                return err;
        }
        write_lock(&mem->lock);
        err = pt_set_flags(mem, start_page, (pages_t) m.pages, (int) m.flags);
        write_unlock(&mem->lock);
        if (err < 0)
            return err;
    }

    mm->brk = rec->brk;
    mm->start_brk = rec->start_brk;
    mm->vdso = rec->vdso;
    mm->stack_start = rec->stack_start;
    mm->argv_start = rec->argv_start; mm->argv_end = rec->argv_end;
    mm->env_start = rec->env_start; mm->env_end = rec->env_end;
    mm->auxv_start = rec->auxv_start; mm->auxv_end = rec->auxv_end;
    // The executable, by path as the cwd is. Here, before the task's own
    // credentials go back on, so an execute-only binary still opens. One that
    // has since gone -- replaced by a package upgrade, say -- leaves the link
    // empty, which is what it was for everything before this.
    if (exe[0] == '/') {
        struct fd *exe_fd = generic_open(exe, O_RDONLY_, 0);
        if (!IS_ERR(exe_fd)) {
            struct fd *old = mm->exefile;
            mm->exefile = exe_fd;
            if (old != NULL)
                fd_close(old);
        } else {
            CKPT_TRACE("  load pid %u: executable %s did not reopen (%ld)\n",
                       rec->pid, exe, PTR_ERR(exe_fd));
        }
    }

descriptors:
    // A table shared with an earlier task was filled by that task's record:
    // nothing to close, nothing to install, and no descriptor records follow.
    // A process sharing it without being a thread (CLONE_FILES alone) still
    // has a group of its own, which joins its terminal as any process does.
    if (rec->files_owner != 0) {
        if (!thread) {
            struct ckpt_stdio_set *shared_set =
                    ckpt_stdio_set_for(st, h, rec, current->files);
            if (shared_set == NULL)
                return _EAGAIN;
            ckpt_join_terminal(shared_set);
        }
        goto identity;
    }

    // The descriptors. Everything the fresh task opened for itself goes
    // first: the image is the complete truth about what this process had open.
    files = current->files;
    lock(&files->lock, 0);
    for (unsigned i = 0; i < files->size; i++) {
        if (files->files[i] != NULL) {
            fd_close(files->files[i]);
            files->files[i] = NULL;
        }
    }
    unlock(&files->lock);

    // The standard streams, one set per TERMINAL and shared by every process
    // on it -- not created per task.
    //
    // Per task is what a fresh boot does, and it is wrong here for the same
    // reason it would be wrong to give a forked child its own dup of the
    // terminal: create_piped_stdio wraps the HOST's descriptors 0, 1 and 2, so
    // N restored processes meant N struct fds over the same three host
    // descriptors. The first child to exit closed them, and the app's real
    // stdout and stderr went with it -- the parent then wrote into a closed
    // descriptor and the session looked hung. A fork shares the struct fd; so
    // does this.
    struct ckpt_stdio_set *set = ckpt_stdio_set_for(st, h, rec, files);
    if (set == NULL)
        return _EAGAIN;
    ckpt_join_terminal(set);
    // Nothing goes into 0, 1 or 2 except by this process's own record for it.
    //
    // The set's descriptors used to be installed there first, for every task,
    // and a record replaced them -- so a process that had CLOSED its standard
    // streams came back holding the set's. sysvinit closes all three at
    // startup: a resumed Devuan session's init held the app's host stdio,
    // which it could not describe either, so the session could never be saved
    // again. A stream the image does not name was closed, and stays closed.
    // Making a terminal's set opens it straight into 0, 1 and 2, so whatever
    // is left in an unnamed one after the loop is closed there.
    bool named[3] = {false, false, false};

    // What the loop below was working on when it gave up. A restore that
    // aborts on one descriptor used to report nothing but the errno, and the
    // app said "session NOT restored: -2 (no reason recorded)" -- true, and
    // useless: -2 is ENOENT, and which of a process's descriptors could not be
    // reopened, and from what path, is the entire question.
    uint32_t failed_fd = 0, failed_kind = 0;
    char failed_path[MAX_PATH + 1] = {0};

    // Reopened with the access the process ALREADY HAD, not asked for again.
    //
    // Until `identity:` below this task carries its PARENT's credentials --
    // ckpt_new_task copies them -- so a reopen was permission-checked as
    // whoever the parent was. A shell's forked child on a pty the restore had
    // just rebuilt owned by root was checked as the uid-1000 shell, and came
    // back EACCES: "pid 753 could not restore fd 0 (pts /dev/pts/1): -13",
    // which refused the whole session. The same check would refuse a daemon
    // that opened its log as root and then dropped privilege -- a descriptor
    // it holds and legitimately could not open again.
    //
    // The access was granted when the descriptor was first opened. A restore
    // re-establishes it; it does not re-decide it. Root for the reopens, then
    // the task's own credentials at `identity:`, where every descriptor this
    // task opened is re-stamped with them (fd_open_creds_stamp).
    current->uid = current->euid = current->suid = current->fsuid = 0;
    current->gid = current->egid = current->sgid = current->fsgid = 0;

    for (uint32_t i = 0; i < rec->n_fds; i++) {
        struct ckpt_fd cf;
        if ((err = rd(f, &cf, sizeof(cf))) < 0)
            goto fds_done;
        char path[MAX_PATH + 1] = {0};
        if (cf.path_len > MAX_PATH) { err = _EINVAL; goto fds_done; }
        if ((err = rd(f, path, cf.path_len)) < 0)
            goto fds_done;

        failed_fd = cf.fd;
        failed_kind = cf.kind;
        snprintf(failed_path, sizeof(failed_path), "%s", path);
        if (cf.fd < 3)
            named[cf.fd] = true;
        CKPT_TRACE("  load fd %u %-5s id %u flags %#x off %llu %s\n",
                   cf.fd, ckpt_kind_name(cf.kind), cf.id, cf.flags,
                   (unsigned long long) cf.offset, path);

        // The same object as one already built -- a descriptor this process
        // shares with another, or with itself at a second number.
        if (cf.kind == CKPT_FD_REF) {
            struct fd *shared = cf.id < st->id_count ? st->by_id[cf.id] : NULL;
            if (shared == NULL && ckpt_tty_pending(st, cf.id)) {
                // A terminal description that is itself waiting for its
                // terminal (ckpt_restore_pending_ttys): this one waits with it.
                if ((err = ckpt_defer_tty(st, cf.id, 0, 0, 0, current, cf.fd,
                                          cf.cloexec != 0, false)) < 0)
                    goto fds_done;
                continue;
            }
            if (shared == NULL) {
                err = _EINVAL;
                goto fds_done;
            }
            fd_retain(shared);
            if ((err = fdtable_install_at(files, (fd_t) cf.fd, shared,
                                          cf.cloexec != 0)) < 0)
                goto fds_done;
            continue;
        }

        if (cf.kind == CKPT_FD_FIFO) {
            if (cf.offset > (1u << 20)) { err = _EINVAL; goto fds_done; }
            char *payload = NULL;
            if (cf.offset != 0) {
                payload = malloc((size_t) cf.offset);
                if (payload == NULL) { err = _ENOMEM; goto fds_done; }
                if ((err = rd(f, payload, (size_t) cf.offset)) < 0) {
                    free(payload);
                    goto fds_done;
                }
            }
            struct fd *ffd = ckpt_reopen_fifo(st, &cf, path, payload);
            free(payload);
            if (IS_ERR(ffd)) {
                err = (int) PTR_ERR(ffd);
                goto fds_done;
            }
            if ((err = ckpt_id_put(st, cf.id, ffd)) < 0) {
                fd_close(ffd);
                goto fds_done;
            }
            if ((err = fdtable_install_at(files, (fd_t) cf.fd, ffd,
                                          cf.cloexec != 0)) < 0)
                goto fds_done;
            continue;
        }

        if (ckpt_kind_is_anon(cf.kind)) {
            if (cf.offset > CKPT_ANON_MAX) { err = _EINVAL; goto fds_done; }
            char *payload = NULL;
            if (cf.offset != 0) {
                payload = malloc((size_t) cf.offset);
                if (payload == NULL) { err = _ENOMEM; goto fds_done; }
                if ((err = rd(f, payload, (size_t) cf.offset)) < 0) {
                    free(payload);
                    goto fds_done;
                }
            }
            struct fd *afd = ckpt_rebuild_anon(st, &cf, payload);
            free(payload);
            if (IS_ERR(afd)) {
                err = (int) PTR_ERR(afd);
                goto fds_done;
            }
            afd->flags = (int) cf.flags;
            if ((err = ckpt_id_put(st, cf.id, afd)) < 0) {
                fd_close(afd);
                goto fds_done;
            }
            if ((err = fdtable_install_at(files, (fd_t) cf.fd, afd,
                                          cf.cloexec != 0)) < 0)
                goto fds_done;
            continue;
        }

        if (cf.kind == CKPT_FD_PIPE) {
            // NOT named rd/wr: `rd` is this file's reader function, and a
            // local of that name turns rd(f, ...) below into a call through a
            // struct fd pointer.
            struct fd *pipe_rd = NULL, *pipe_wr = NULL;
            if ((err = ckpt_pipe_for(st, cf.pipe_inode, &pipe_rd, &pipe_wr)) < 0)
                goto fds_done;
            struct fd *end = cf.pipe_write_end ? pipe_wr : pipe_rd;
            // The description's own flags, O_NONBLOCK above all, on the guest
            // side and the host's alike (realfs_setflags keeps the two in
            // step: realfs_read trusts the guest flag and reads the host fd
            // raw). A fresh pipe is blocking, and nothing put them back: a
            // restored tmux server wedged on the first signal it got, because
            // libevent drains its signal pipe until EAGAIN and the second read
            // of a now-blocking pipe never returned. Every pane went deaf and
            // every tmux command after it hung.
            if ((err = fd_setflags(end, (int) cf.flags)) < 0)
                goto fds_done;
            // The bytes that were in flight, put back at the write end so the
            // reader sees them exactly where it left off. Only the read end's
            // record carries them, so this runs once per pipe.
            if (cf.offset != 0) {
                char *buf = malloc((size_t) cf.offset);
                if (buf == NULL) { err = _ENOMEM; goto fds_done; }
                if ((err = rd(f, buf, (size_t) cf.offset)) < 0) {
                    free(buf);
                    goto fds_done;
                }
                ssize_t put = write(pipe_wr->real_fd, buf, (size_t) cf.offset);
                free(buf);
                if (put != (ssize_t) cf.offset) {
                    ckpt_refuse("pipe %llu had %llu bytes in it, more than a "
                                "fresh pipe will hold",
                                (unsigned long long) cf.pipe_inode,
                                (unsigned long long) cf.offset);
                    err = _EAGAIN;
                    goto fds_done;
                }
            }
            if ((err = ckpt_id_put(st, cf.id, end)) < 0)
                goto fds_done;
            fd_retain(end);   // the process's own
            if ((err = fdtable_install_at(files, (fd_t) cf.fd, end,
                                          cf.cloexec != 0)) < 0)
                goto fds_done;
            continue;
        }

        // A pty pair the image owns. The master re-opens /dev/ptmx, which
        // allocates a fresh number, and that number is recorded so the slave
        // -- restored later, because its holder is a child of the master's --
        // can be pointed at the same pair instead of being handed a window of
        // its own.
        if (cf.kind == CKPT_FD_PTY_MASTER || cf.kind == CKPT_FD_PTY_SLAVE) {
            char pty_path[64];
            if (cf.kind == CKPT_FD_PTY_MASTER) {
                snprintf(pty_path, sizeof(pty_path), "/dev/ptmx");
            } else {
                int mapped = ckpt_pty_map_get(st, (int) cf.offset);
                if (mapped < 0) {
                    // The master is not back YET, or not in the image at all,
                    // and only the end of the restore can tell which
                    // (ckpt_restore_pending_ttys). Deciding here gave an
                    // attached tmux client's terminal, held by an inner server
                    // older than the outer one whose pane it is, this task's
                    // /dev/null: the server read EOF from it at once, dropped
                    // the client, and the pane's shell -- sharing the
                    // description -- was left on /dev/null too.
                    if ((err = ckpt_defer_tty(st, cf.id, (int) cf.offset,
                                              rec->sid, (int) cf.flags, current,
                                              cf.fd, cf.cloexec != 0, true)) < 0)
                        goto fds_done;
                    struct ckpt_pending_tty *p =
                            &st->pending_ttys[st->pending_tty_count - 1];
                    p->pty_slave = true;
                    p->set = (uint32_t) (set - st->sets);
                    CKPT_TRACE("  fd %u: the master of pts %llu is not back yet; "
                               "after every task\n", cf.fd,
                               (unsigned long long) cf.offset);
                    continue;
                }
                snprintf(pty_path, sizeof(pty_path), "/dev/pts/%d", mapped);
            }
            struct fd *pty = generic_open(pty_path, ckpt_reopen_flags(cf.flags), 0);
            if (IS_ERR(pty)) {
                err = (int) PTR_ERR(pty);
                goto fds_done;
            }
            if (cf.kind == CKPT_FD_PTY_MASTER) {
                struct tty *made = fd_tty(pty);
                pty_unlock_slave_of(made);
                if (cf.pty_owner_known)
                    pty_set_slave_owner_of(made, (uid_t_) cf.pty_uid,
                                           (uid_t_) cf.pty_gid,
                                           (mode_t_) cf.pty_perms);
                if (made != NULL &&
                        (err = ckpt_pty_map_put(st, (int) cf.offset, made->num)) < 0) {
                    fd_close(pty);
                    goto fds_done;
                }
                CKPT_TRACE("    pty master %llu came back as %d\n",
                           (unsigned long long) cf.offset,
                           made != NULL ? made->num : -1);
            }
            if ((err = ckpt_id_put(st, cf.id, pty)) < 0)
                goto fds_done;
            if ((err = fdtable_install_at(files, (fd_t) cf.fd, pty,
                                          cf.cloexec != 0)) < 0)
                goto fds_done;
            continue;
        }

        if (cf.kind == CKPT_FD_SOCKET) {
            // A pair's queued messages come first, then the description.
            char *queued = NULL;
            if (cf.offset > (16u << 20)) { err = _EINVAL; goto fds_done; }
            if (cf.offset != 0) {
                queued = malloc((size_t) cf.offset);
                if (queued == NULL) { err = _ENOMEM; goto fds_done; }
                if ((err = rd(f, queued, (size_t) cf.offset)) < 0) {
                    free(queued);
                    goto fds_done;
                }
            }
            struct sock_ckpt_desc desc;
            if ((err = rd(f, &desc, sizeof(desc))) < 0) {
                free(queued);
                goto fds_done;
            }
            int sock_err = 0;
            struct fd *sock = NULL;
            if (desc.state == SOCK_CKPT_PAIR) {
                uint32_t end = desc.pair_end & 1;
                int slot = -1;
                for (uint32_t k = 0; k < st->sockpair_count; k++)
                    if (st->sockpairs[k].cookie == desc.pair_cookie)
                        slot = (int) k;
                if (slot < 0) {
                    if (st->sockpair_count == st->sockpair_cap) {
                        uint32_t cap = st->sockpair_cap ? st->sockpair_cap * 2 : 8;
                        void *n = realloc(st->sockpairs, cap * sizeof(*st->sockpairs));
                        if (n == NULL) { free(queued); err = _ENOMEM; goto fds_done; }
                        st->sockpairs = n;
                        st->sockpair_cap = cap;
                    }
                    struct fd *e0, *e1;
                    if ((err = sock_ckpt_rebuild_pair(&desc, &e0, &e1)) < 0) {
                        free(queued);
                        goto fds_done;
                    }
                    slot = (int) st->sockpair_count++;
                    st->sockpairs[slot].cookie = desc.pair_cookie;
                    st->sockpairs[slot].end[0] = e0;
                    st->sockpairs[slot].end[1] = e1;
                    st->sockpairs[slot].claimed[0] = false;
                    st->sockpairs[slot].claimed[1] = false;
                }
                if (st->sockpairs[slot].claimed[end]) {
                    free(queued);
                    err = _EINVAL;
                    goto fds_done;
                }
                // This process has it now; the pointer stays for the other
                // end's queue, sent from here.
                st->sockpairs[slot].claimed[end] = true;
                sock = st->sockpairs[slot].end[end];
                sock_ckpt_apply_pair_end(sock, &desc);
                if (queued != NULL) {
                    int qerr = sock_ckpt_requeue(st->sockpairs[slot].end[end ^ 1],
                                                 queued, (size_t) cf.offset);
                    if (qerr < 0)
                        printk("WARNING: checkpoint: pid %u fd %u: %llu queued bytes "
                               "could not be put back (%d)\n", rec->pid, cf.fd,
                               (unsigned long long) cf.offset, -qerr);
                }
            } else {
                sock = sock_ckpt_rebuild(&desc, &sock_err);
            }
            free(queued);
            if (sock == NULL) {
                err = sock_err != 0 ? sock_err : _EIO;
                goto fds_done;
            }
            // A listener that could not be put back comes back hung up, and
            // the session carries on -- so say so where a person can see it.
            const char *sock_why = sock_ckpt_rebuild_failure();
            if (sock_why != NULL)
                ckpt_note_restore(rec->pid, cf.fd, sock_why);
            CKPT_TRACE("    socket %s domain %u type %u proto %u backlog %u\n",
                       sock_ckpt_state_name(desc.state), desc.domain,
                       desc.type, desc.protocol, desc.backlog);
            // The guest's own flags, O_NONBLOCK above all. A socket's host
            // descriptor is non-blocking whatever the guest asked for (fs/
            // sock.c's socket_force_host_nonblock), so the guest's flag lives
            // only in fd->flags -- and nothing put it back. Invisible while
            // every connection came back hung up; once pairs came back live,
            // dbus-daemon's sockets were blocking, its first recvmsg with
            // nothing queued never returned, and every login waited on it.
            sock->flags = (int) cf.flags;
            sock_ckpt_apply_options(sock, &desc);
            if ((err = ckpt_id_put(st, cf.id, sock)) < 0)
                goto fds_done;
            if ((err = fdtable_install_at(files, (fd_t) cf.fd, sock,
                                          cf.cloexec != 0)) < 0)
                goto fds_done;
            continue;
        }

        // Re-attached, not restored: the terminal or the host pipe this guest
        // was talking to went with the process that owned it. sockrestart is
        // the precedent -- record enough to REBUILD, because the original is
        // destroyed either way.
        if (cf.kind == CKPT_FD_TTY) {
            if ((err = ckpt_restore_tty_record(st, set, rec, &cf, path, files)) < 0)
                goto fds_done;
            continue;
        }

        // The host's own standard streams (the CLI with output piped). There
        // is no description to rebuild: the record says WHICH stream it was,
        // and it comes back as that stream of this run, at whatever number
        // the process held it.
        //
        // By the stream, not by the number. A record at 0, 1 or 2 used to
        // keep the set's descriptor of the same NUMBER, so after `exec 1>&2`
        // a resumed shell's stdout was the host's stdout again; and a process
        // on a terminal was given its terminal's set, not the stream at all.
        if (cf.kind == CKPT_FD_STDIO) {
            struct fd *src = ckpt_host_stdio(st, cf.offset);
            if (src == NULL) {
                char why[96];
                snprintf(why, sizeof(why), "host standard stream %llu could not "
                         "be attached; left closed", (unsigned long long) cf.offset);
                ckpt_note_restore(rec->pid, cf.fd, why);
                printk("WARNING: checkpoint: pid %u fd %u: %s\n", rec->pid, cf.fd, why);
                continue;
            }
            if ((err = ckpt_id_put(st, cf.id, src)) < 0)
                goto fds_done;
            fd_retain(src);   // the process's own
            if ((err = fdtable_install_at(files, (fd_t) cf.fd, src,
                                          cf.cloexec != 0)) < 0)
                goto fds_done;
            continue;
        }

        struct fd *fd = generic_open(path, ckpt_reopen_flags(cf.flags), 0);
        if (IS_ERR(fd)) {
            // A descriptor that cannot be reopened DEGRADES. It does not take
            // the session with it.
            //
            // The file it named may simply not be there any more, and for a
            // whole class of them it never will be: /run, /tmp and /dev/shm
            // are tmpfs, so everything a daemon put there is gone on the way
            // back. A device reported exactly this -- "pid 601 could not
            // restore fd 0 (file /run/pacct_source): -2", atopacctd's fifo --
            // and the entire session was discarded over one descriptor
            // belonging to one background daemon, which is a worse answer than
            // any guest could have given.
            //
            // /dev/null is the inert stand-in: reads give EOF and writes are
            // swallowed, which is what a process holding a vanished file
            // should see. Recorded, not silent -- the restore note reaches
            // /proc/ish/checkpoint and the app's Diagnostics -- so a guest
            // that comes back missing something says which something.
            int open_err = (int) PTR_ERR(fd);
            char why[160];
            snprintf(why, sizeof(why), "%s could not be reopened (%d); gave it /dev/null",
                     path[0] != '\0' ? path : "a file", open_err);
            ckpt_note_restore(rec->pid, cf.fd, why);
            printk("WARNING: checkpoint: pid %u fd %u: %s\n", rec->pid, cf.fd, why);
            fd = generic_open("/dev/null", ckpt_reopen_flags(cf.flags), 0);
            if (IS_ERR(fd)) {
                // /dev/null itself is missing: the root is not one we can
                // restore into at all, and that IS worth refusing.
                err = (int) PTR_ERR(fd);
                goto fds_done;
            }
        }
        if (cf.kind == CKPT_FD_FILE && fd->ops->lseek != NULL)
            fd->ops->lseek(fd, (off_t_) cf.offset, LSEEK_SET);
        fd->offset = cf.offset;
        if ((err = ckpt_id_put(st, cf.id, fd)) < 0)
            goto fds_done;
        // A fresh task's table holds three descriptors; the image may name
        // fd 10, because a shell parks its saved stdin up there. Grow to fit
        // rather than refuse -- the number is part of what is restored.
        if ((err = fdtable_install_at(files, (fd_t) cf.fd, fd,
                                      cf.cloexec != 0)) < 0)
            goto fds_done;
    }
    err = 0;
fds_done:
    if (err < 0) {
        ckpt_refuse("pid %u could not restore fd %u (%s%s%s): %d",
                    rec->pid, failed_fd, ckpt_kind_name(failed_kind),
                    failed_path[0] != '\0' ? " " : "", failed_path, err);
        return err;
    }
    // A standard stream no record named was closed when this process was
    // saved. Anything there now came from making its terminal's set, not
    // from the image.
    for (unsigned i = 0; i < 3; i++) {
        if (!named[i] && f_close((fd_t) i) == 0)
            CKPT_TRACE("  fd %u: closed when saved, and closed again\n", i);
    }

    // Credentials, identity and the rest of the task.
    if (rec->native)
        goto identity;
identity:
    if ((err = ckpt_restore_signals_and_timers(f, rec, st)) < 0) {
        ckpt_refuse("pid %u: its queued signals or timers could not be read back: %d",
                    rec->pid, err);
        return err;
    }
    current->uid = rec->uid; current->gid = rec->gid;
    current->euid = rec->euid; current->egid = rec->egid;
    current->suid = rec->suid; current->sgid = rec->sgid;
    current->fsuid = rec->fsuid; current->fsgid = rec->fsgid;
    memcpy(current->cap_effective, rec->cap_effective, sizeof(current->cap_effective));
    memcpy(current->cap_permitted, rec->cap_permitted, sizeof(current->cap_permitted));
    memcpy(current->cap_inheritable, rec->cap_inheritable, sizeof(current->cap_inheritable));
    memcpy(current->cap_ambient, rec->cap_ambient, sizeof(current->cap_ambient));
    memcpy(current->cap_bounding, rec->cap_bounding, sizeof(current->cap_bounding));
    current->keepcaps = rec->keepcaps != 0;
    current->no_new_privs = rec->no_new_privs != 0;
    // The descriptors this task's restore opened were stamped with the root
    // credentials they were reopened under. To the open-creds model they are
    // opens this process made itself, so they get its credentials now. Ones
    // another task's restore made -- shared by id -- keep that task's stamp,
    // and are told apart by tgid, which fd_create records and which is this
    // task's alone.
    // A shared table was re-stamped by its owner's restore, with the owner's
    // credentials, which are the ones those descriptors were opened with.
    if (rec->files_owner == 0) {
        lock(&current->files->lock, 0);
        for (unsigned n = 0; n < current->files->size; n++) {
            struct fd *fd = current->files->files[n];
            if (fd != NULL && fd->open_creds.known &&
                    fd->open_creds.tgid == current->tgid)
                fd_open_creds_stamp(fd);
        }
        unlock(&current->files->lock);
    }
    memcpy(current->comm, rec->comm, sizeof(current->comm));
    current->blocked = rec->blocked;
    // Which thread was told to take what the process has queued is not in the
    // image. Every thread looks once, at its first signal check, and stops
    // looking if nothing there is its to take (receive_signals).
    current->group_sigpending = true;
    // Everything else's was rebuilt from its queue, above.
    if (rec->native)
        current->pending = rec->pending;
    current->altstack = rec->altstack;
    current->altstack_size = rec->altstack_size;
    current->clear_tid = rec->clear_tid;
    current->exit_signal = rec->exit_signal;
    current->pdeath_signal = rec->pdeath_signal;
    current->nice = rec->nice;
    current->sched_policy = rec->sched_policy;
    current->robust_list = rec->robust_list;
    current->did_exec = rec->did_exec != 0;

    // Shared handlers and a shared cwd/umask were put back by their owner.
    if (!rec->native && rec->sighand_owner == 0) {
        lock(&current->sighand->lock, 0);
        memcpy(current->sighand->action, actions, sizeof(actions));
        unlock(&current->sighand->lock);
    }
    if (rec->fs_owner == 0) {
        lock(&current->fs->lock, 0);
        current->fs->umask = rec->umask;
        unlock(&current->fs->lock);
        if (cwd[0] == '/') {
            struct fd *pwd = generic_open(cwd, O_RDONLY_, 0);
            if (!IS_ERR(pwd))
                fs_chdir(current->fs, pwd);
        }
    }

    if (rec->native)
        return 0;   // no register file: it is a function call, not an image

    // The register file last, so nothing above can have run guest code with a
    // half-restored one. The two pointers in struct cpu_state name host
    // objects that belong to THIS run: the address space's mmu, and the flag
    // the wake path sets to break out of guest execution. Everything else in
    // there is guest value state; these two are re-attached rather than
    // restored.
    struct mmu *mmu = current->cpu.mmu;
    current->cpu = cpu;
    current->cpu.mmu = mmu;
    // poked_ptr points INTO its own cpu_state (&cpu->_poked, set by every
    // engine's entry). So it can be neither restored from the image -- that is
    // a host address from a process that has exited -- nor carried over from
    // before the assignment: a task built by task_create_with_pid got its
    // parent's whole cpu_state by struct copy, parent's _poked address and
    // all. A restored child would then have its "stop executing" flag set by
    // pokes aimed at its parent and never by its own, so it ran on past every
    // wake and its parent's wait() never returned.
    current->cpu.poked_ptr = &current->cpu._poked;
    return 0;
}

// Hand a restored native program back its state and arrange for it to run.
//
// A native program is not photographed and not resumed mid-instruction: it is
// RE-LAUNCHED and told to rebuild itself, which is the only thing a C function
// on a host thread can do. The channel is the one its fork-by-relaunch child
// already reads a state from (struct native_program's ckpt_state_var), so
// nothing new has to be taught to the program.
//
// A PIPE, and the state is written before the program is dispatched. It fits:
// a shell's state is tens of kilobytes and a pipe buffer is 64, and if it did
// not, the write would block against a reader that has not started -- so an
// oversized state is refused here rather than deadlocking the restore.
static int ckpt_dispatch_native(struct task *task, struct ckpt_restore_state *st) {
    const struct native_program *prog = native_program_lookup(st->native_name);
    if (prog == NULL) {
        ckpt_refuse("this build has no native program called %s", st->native_name);
        return _ENOENT;
    }

    // argv and envp out of their NUL-separated blocks.
    unsigned argc = 0;
    for (uint32_t i = 0; i < st->native_argv_len; i++)
        if (st->native_argv[i] == '\0')
            argc++;
    unsigned envc = 0;
    for (uint32_t i = 0; i < st->native_env_len; i++)
        if (st->native_env[i] == '\0')
            envc++;

    char **argv = calloc(argc + 1, sizeof(*argv));
    char **envp = calloc(envc + 2, sizeof(*envp));
    if (argv == NULL || envp == NULL) {
        free(argv); free(envp);
        return _ENOMEM;
    }
    unsigned n = 0;
    for (uint32_t i = 0; i < st->native_argv_len && n < argc; ) {
        argv[n++] = st->native_argv + i;
        i += strlen(st->native_argv + i) + 1;
    }
    if (argc == 0)
        argv[argc = 0] = NULL;
    unsigned m = 0;
    for (uint32_t i = 0; i < st->native_env_len && m < envc; ) {
        envp[m++] = st->native_env + i;
        i += strlen(st->native_env + i) + 1;
    }

    char fdvar[64] = "";
    struct fd *state_rd = NULL, *state_wr = NULL;
    if (st->native_standin_child == 0 &&
            prog->ckpt_state_var != NULL && st->native_state[0] != '\0') {
        size_t len = strlen(st->native_state);
        // A GUEST pipe, not a host one. The program reads its state through
        // the shim, which routes every descriptor through the guest's own
        // table -- so a raw host descriptor number means nothing to it. That
        // is what made a restored zsh source an empty state and report that it
        // "did not finish": it was reading whatever the guest happened to have
        // at that number, which was nothing.
        int err = pipe_create_pair(&state_rd, &state_wr, adhoc_next_inode());
        if (err < 0) {
            free(argv); free(envp);
            return err;
        }
        ssize_t put = write(state_wr->real_fd, st->native_state, len);
        fd_close(state_wr);
        if (put != (ssize_t) len) {
            fd_close(state_rd);
            free(argv); free(envp);
            ckpt_refuse("%s's saved state is %zu bytes, more than a pipe will "
                        "hold before the program starts reading it",
                        prog->name, len);
            return _E2BIG;
        }
        // At a number nothing in the image used. The image's descriptors are
        // already installed, or waiting for their terminal (pending_ttys) at
        // a number that is empty only until the end of the restore, so the
        // first slot above them that is neither is free for good -- and the
        // program unsets the variable naming it at startup, so nothing it runs
        // inherits either.
        struct fdtable *files = task->files;
        fd_t at = 0;
        lock(&files->lock, 0);
        for (at = 3; ((unsigned) at < files->size && files->files[at] != NULL) ||
                     ckpt_fd_pending(st, task, at); at++)
            ;
        unlock(&files->lock);
        if ((err = fdtable_install_at(files, at, state_rd, false)) < 0) {
            free(argv); free(envp);
            return err;
        }
        snprintf(fdvar, sizeof(fdvar), "%s=%d", prog->ckpt_state_var, (int) at);
        envp[m++] = fdvar;
        CKPT_TRACE("  native %s: %zu bytes of state on guest fd %d\n",
                   prog->name, len, (int) at);
    }
    envp[m] = NULL;

    // Hand it a terminal in the state a fresh one is in.
    //
    // A native program is RE-LAUNCHED, not resumed -- it starts from scratch
    // and, like every full-screen program, saves the terminal mode it finds at
    // startup so it can put it back on exit. The mode it finds here is the one
    // its PREVIOUS incarnation left behind, which for anything full-screen is
    // raw: the checkpoint faithfully saved `tty->termios` mid-ktop, and the
    // restore faithfully applied it. So the re-launched program saved raw as
    // "original", set raw, and on exit restored raw -- leaving the shell on a
    // terminal with no echo and no line discipline. Reported as "the terminal
    // locked up" after exiting ktop across a suspend; it was not locked, it
    // was deaf.
    //
    // Before native_exec_set_pending, which only RECORDS the program --
    // task_run_current starts it later -- so this lands before it runs and
    // reads the mode. Emulated programs are untouched: those are resumed
    // exactly, registers and all, so their raw mode is still theirs and still
    // correct.
    //
    // Only for the process that reads the terminal NEXT, though. A task outside
    // the foreground group -- a shell whose job holds the terminal, or an exec
    // stand-in waiting on the program it started -- does not read it until
    // that job is done, and the job may be an emulated program resumed exactly,
    // raw mode and all. Resetting under it would hand a still-running editor a
    // cooked terminal.
    bool reads_terminal_next = false;
    if (st->native_standin_child == 0 && task->group != NULL) {
        lock(&task->group->lock, 0);
        struct tty *ctty = task->group->tty;
        pid_t_ own_pgid = task->group->pgid;
        unlock(&task->group->lock);
        if (ctty != NULL) {
            lock(&ctty->lock, 0);
            reads_terminal_next = ctty->fg_group == 0 || ctty->fg_group == own_pgid;
            unlock(&ctty->lock);
        }
    }
    if (reads_terminal_next)
        tty_reset_termios_to_default(task->group->tty);

    // Recorded rather than run: task_run_current calls native_exec_run_pending
    // on the way in, which is exactly how a native program starts on a fresh
    // boot. The copies it makes are its own, so the blocks above may go.
    struct task *saved = current;
    current = task;
    int err = native_exec_set_pending(prog, (int) argc, argv, envp);
    // A restore, not an exec: wait for the task's foreground job first, and
    // bring a stand-in back as its wait (native_exec_run_pending).
    if (err == 0)
        native_exec_mark_restored(st->native_standin_child);
    current = saved;
    free(argv);
    free(envp);
    return err;
}

// Build a task to restore INTO: a fresh process, at the pid the image names,
// as a child of the task that image named as its parent.
//
// The same shape as kernel/init.c's construct_task, and deliberately not a
// call to it: that one allocates the next free pid and roots everything at
// init, which is exactly the two things a restore must not do.
// The already-built tasks a record shares with: its thread group's leader and
// the owner of each object (struct ckpt_task's tgid and owners). NULL for
// whatever the task has of its own.
struct ckpt_owners {
    struct task *leader, *mm, *files, *fs, *sighand;
};

static struct task *ckpt_built(struct task **built, unsigned nbuilt, uint32_t pid) {
    if (pid == 0)
        return NULL;
    for (unsigned i = 0; i < nbuilt; i++)
        if (built[i] != NULL && (uint32_t) built[i]->pid == pid)
            return built[i];
    return NULL;
}

// Built the way clone() builds one: an object shared with an owner is that
// owner's, retained, exactly as copy_task retains it for CLONE_VM / FILES / FS
// / SIGHAND, and a THREAD joins its leader's group -- limits, session, process
// group, terminal and all -- as CLONE_THREAD puts it there. Everything else is
// new. The parent is the task's own, which for a thread is the thread that
// created it (AOK links threads under their creator; see
// [[threads-are-children-of-creator]]), and parents are always built first.
static struct task *ckpt_new_task(struct task *parent, pid_t_ pid,
        const struct ckpt_owners *own) {
    struct task *task = task_create_with_pid(parent, pid);
    if (task == NULL)
        return NULL;
    // task_create_ aliases the parent's namespaces; the child's references
    // are taken here, as construct_task takes them (kernel/init.c).
    if (parent != NULL) {
        uts_ns_retain(task->uts_ns);
        ipc_ns_retain(task->ipc_ns);
    }

    if (own->leader != NULL) {
        struct tgroup *group = own->leader->group;
        complex_lockt(&pids_lock, 0);
        lock(&group->lock, 0);
        task->group = group;
        task->tgid = own->leader->pid;
        list_add(&group->threads, &task->group_links);
        unlock(&group->lock);
        unlock(&pids_lock);
    } else {
        struct tgroup *group = malloc(sizeof(struct tgroup));
        if (group == NULL)
            return NULL;
        *group = (struct tgroup) {};
        list_init(&group->threads);
        lock_init(&group->lock, "ckpt_new_task\0");
        cond_init(&group->child_exit);
        cond_init(&group->stopped_cond);
        group->leader = task;
        group->personality = 0; // the image's, once the leader's record is read
        // The defaults, before the image's own limits land further down.
        // Without them RLIMIT_NOFILE is zero on a freshly built tgroup, and
        // the first descriptor the restore tries to install comes back EMFILE
        // -- a "too many open files" on a table holding none.
        memcpy(group->limits, init_rlimits, sizeof(init_rlimits));
        list_add(&group->threads, &task->group_links);
        task->group = group;
        task->tgid = task->pid;
        task_setsid(task);
    }

    if (own->mm != NULL) {
        mm_retain(own->mm->mm);
        task_set_mm(task, own->mm->mm);
    } else {
        task_set_mm(task, mm_new(task->abi));
    }
    if (own->sighand != NULL) {
        own->sighand->sighand->refcount++;
        task->sighand = own->sighand->sighand;
    } else {
        task->sighand = sighand_new();
    }
    if (own->files != NULL) {
        own->files->files->refcount++;
        task->files = own->files->files;
    } else {
        task->files = fdtable_new(3);
    }
    if (own->fs != NULL) {
        own->fs->fs->refcount++;
        task->fs = own->fs->fs;
        return task;
    }
    task->fs = fs_info_new();
    task->fs->umask = 0022;

    struct task *saved = current;
    current = task;
    task->fs->root = generic_open("/", O_RDONLY_, 0);
    current = saved;
    if (IS_ERR(task->fs->root))
        return NULL;
    task->fs->pwd = fd_retain(task->fs->root);
    return task;
}

// A restore that fails part-way must leave NOTHING behind.
//
// It did not. By the time it can fail it has built tasks -- frozen from birth,
// never started, but in the pid table and holding what was rebuilt for them:
// descriptors, ptys, a listening socket bound on the host -- mounted the
// image's tmpfses and filled them, and rewritten pid 1 as the image's init.
// The app then boots in the same process, and the fresh boot inherited all of
// it. On device that was a session with no working terminal and no sshd, that
// only an app restart cured: a ghost sshd held port 1022 and the old session's
// pid file said sshd was running, and the new init started with the image's
// descriptors open.
//
// Undone in the order it was done, backwards. Tasks were built after their
// parents, so going back takes every child out before the task it belongs to.
// One that never got a thread is taken apart without running a single
// instruction (task_never_ran_destroy, fork's own unwind for a child whose
// thread could not be made) -- starting it to kill it would first run a native
// program's pending launch, then guest code on half-restored state. One that
// DID start, because the failure was starting a later one, is killed the
// ordinary way.
static void ckpt_restore_unwind(struct task **built, unsigned nbuilt,
        struct task *first) {
    unsigned destroyed = 0, killed = 0;
    for (unsigned i = nbuilt; i-- > 0; ) {
        struct task *t = built[i];
        if (t == NULL || t == first)
            continue;
        if (atomic_load_explicit(&t->host_thread_started, memory_order_acquire)) {
            struct siginfo_ info = { .sig = SIGKILL_, .code = SI_KERNEL_ };
            send_signal(t, SIGKILL_, info);
            killed++;
        } else {
            native_exec_discard_pending(t);
            task_never_ran_destroy(t);
            destroyed++;
        }
    }

    // pid 1 is the entry point's own task, and the boot that follows runs init
    // in it. execve replaces its memory and resets its handlers, but keeps
    // every descriptor not marked close-on-exec and the signal mask -- so the
    // image's init's descriptors, blocked set and pending signals would all
    // have reached the new init and everything it starts.
    if (nbuilt > 0 && built[0] == first) {
        lock(&first->general_lock, 0);
        struct fdtable *dead = first->files;
        first->files = fdtable_new(3);
        unlock(&first->general_lock);
        fdtable_release(dead);
        lock(&first->sighand->lock, 0);
        struct sigqueue *q, *qtmp;
        list_for_each_entry_safe(&first->queue, q, qtmp, queue) {
            list_remove(&q->queue);
            free(q);
        }
        // And the process's, which the image's init had queued too.
        list_for_each_entry_safe(&first->sighand->queue, q, qtmp, queue) {
            list_remove(&q->queue);
            free(q);
        }
        first->sighand->pending = 0;
        first->pending = 0;
        first->blocked = 0;
        first->group_sigpending = false;
        first->group_handoff = 0;
        unlock(&first->sighand->lock);
        // The call the image's init was parked in is not the one the boot runs.
        first->sleep_restart_valid = false;
        first->poll_restart_valid = false;
        first->restart_nohand_pending = false;
        first->restart_sys_pending = false;
        first->ckpt_restored = false;
        first->clear_tid = 0;
        first->robust_list = 0;
    }

    // Deepest first, the reverse of how they went up. Lazily: a descriptor the
    // teardown above has not finished closing must not be able to keep the
    // image's /run in front of the boot's.
    unsigned unmounted = 0;
    for (unsigned i = ckpt_restore_mounted_n; i-- > 0; ) {
        int e = do_umount_lazy(ckpt_restore_mounted[i]);
        if (e < 0)
            printk("WARNING: checkpoint: could not unmount %s after the failed "
                   "restore (%d)\n", ckpt_restore_mounted[i], -e);
        else
            unmounted++;
        free(ckpt_restore_mounted[i]);
    }
    ckpt_restore_mounted_n = 0;
    printk("checkpoint: undid the failed restore: %u processes taken apart, "
           "%u killed, %u tmpfs unmounted\n", destroyed, killed, unmounted);
}

int checkpoint_restore(const char *host_path) {
    ckpt_restore_note[0] = '\0';
    ckpt_restore_mounted_n = 0;
    FILE *f = fopen(host_path, "rb");
    if (f != NULL)
        setvbuf(f, NULL, _IOFBF, 1 << 20);   // see checkpoint_save's note
    if (f == NULL)
        return errno_map();

    int err;
    struct ckpt_header h;
    struct task **built = NULL;
    unsigned nbuilt = 0;
    // The boot this process made before trying the image, for undoing the
    // clocks if the image cannot be restored.
    extern time_t boot_time;
    time_t boot_before_restore = boot_time;
    bool clocks_resumed = false;
    // What the restore builds as it goes: the shared standard streams, the
    // descriptor identity table, and the pipes. See ckpt_restore_task.
    struct ckpt_restore_state st = {0};
    if ((err = rd(f, &h, sizeof(h))) < 0)
        goto out;
    err = _EINVAL;
    if (memcmp(h.magic, CKPT_MAGIC, sizeof(h.magic)) != 0)
        goto out;
    if (h.version != CKPT_VERSION || h.page_size != PAGE_SIZE)
        goto out;
    // The refusal that keeps a byte-copied struct cpu_state honest.
    if (h.build_fingerprint != ckpt_fingerprint() ||
            h.cpu_state_size != sizeof(struct cpu_state))
        goto out;
    if (h.n_tasks == 0 || h.n_tasks > 4096)
        goto out;
    // The root, before anything is changed: the image is intact and belongs to
    // another root, so this refuses rather than fails, with a code of its own
    // so that whoever offered the image keeps it for that root. Only when both
    // sides have an identity; 0 is "unknown", not "any".
    h.root_name[sizeof(h.root_name) - 1] = '\0';
    char here_name[sizeof(h.root_name)];
    uint64_t here = ckpt_running_root(here_name, sizeof(here_name));
    if (h.root_identity != 0 && here != 0 && h.root_identity != here) {
        ckpt_refuse("this session was saved on the root \"%s\", not on this one (\"%s\")",
                    h.root_name, here_name);
        err = _EXDEV;
        goto out;
    }

    // The UTS namespace, before any task runs. Only when the image has one:
    // an empty hostname means the checkpointed guest had none set either, and
    // clearing what this launch already established would be inventing a
    // change the image does not describe.
    h.hostname[sizeof(h.hostname) - 1] = '\0';
    h.domainname[sizeof(h.domainname) - 1] = '\0';
    if (h.hostname[0] != '\0')
        uts_set_boot_hostname(h.hostname);
    if (h.domainname[0] != '\0') {
        lock(&init_uts_ns.lock, 0);
        snprintf(init_uts_ns.domainname, sizeof(init_uts_ns.domainname),
                 "%s", h.domainname);
        unlock(&init_uts_ns.lock);
    }

    // The guest's clocks, before anything is built: every restored task comes
    // back holding deadlines on them, and a task that was frozen inside
    // clock_nanosleep(TIMER_ABSTIME) re-executes it against whatever these
    // say. Put back as they were, not restarted at zero -- see
    // guest_clock_resume. Undone at `out` if the restore fails, because the
    // fresh boot that follows a failed restore is a boot.
    struct guest_clock_reading clocks = {
        .monotonic_ns = h.clock_monotonic_ns,
        .boottime_ns = h.clock_boottime_ns,
        .raw_ns = h.clock_raw_ns,
        .realtime_ns = h.clock_realtime_ns,
        .boot_time = h.clock_boot_time,
    };
    int64_t stopped_ns = guest_clock_resume(&clocks);
    clocks_resumed = true;
    printk("checkpoint: clocks resumed: monotonic %lld.%03lld s, uptime %lld.%03lld s "
           "(%lld.%03lld s stopped)\n",
           (long long) (h.clock_monotonic_ns / 1000000000),
           (long long) (h.clock_monotonic_ns / 1000000 % 1000),
           (long long) ((h.clock_boottime_ns + stopped_ns) / 1000000000),
           (long long) ((h.clock_boottime_ns + stopped_ns) / 1000000 % 1000),
           (long long) (stopped_ns / 1000000000),
           (long long) (stopped_ns / 1000000 % 1000));

    // The tmpfs trees, before anything reads a task record: a restored
    // descriptor on /run/... is reopened by path, and a socket rebuilt by
    // sock_ckpt_rebuild binds a name whose directory has to exist.
    if ((err = ckpt_tmpfs_restore(f, h.n_tmpfs)) < 0)
        goto out;

    built = calloc(h.n_tasks, sizeof(*built));
    if (built == NULL) { err = _ENOMEM; goto out; }

    struct task *first = current;
    for (uint32_t i = 0; i < h.n_tasks; i++) {
        struct ckpt_task rec;
        if ((err = rd(f, &rec, sizeof(rec))) < 0)
            goto out;
        err = _EINVAL;
        if (rec.cwd_len > MAX_PATH || rec.root_len > MAX_PATH || rec.exe_len > MAX_PATH ||
                rec.ngroups > MAX_GROUPS ||
                rec.seccomp_nprogs > CKPT_MAX_SECCOMP_PROGS ||
                rec.n_sigactions != NUM_SIGS)
            goto out;

        struct task *task;
        if (i == 0 && rec.pid == (uint32_t) first->pid) {
            // The image's first task IS this one: the entry point has already
            // made a pid 1 and it is the process the image calls pid 1.
            task = first;
        } else {
            struct task *parent = NULL;
            for (unsigned j = 0; j < nbuilt; j++)
                if (built[j]->pid == (pid_t_) rec.ppid)
                    parent = built[j];
            // A task whose parent is not in the image was reparented to init
            // between the freeze and the walk. init is where it was going.
            if (parent == NULL)
                parent = first;
            // What it shares, all built already: an owner is always earlier
            // in the image than the tasks that point at it. One that is not
            // there is an image that does not describe itself.
            struct ckpt_owners own = {0};
            bool thread = rec.tgid != 0 && rec.tgid != rec.pid;
            own.leader = thread ? ckpt_built(built, nbuilt, rec.tgid) : NULL;
            // A leader the image does not have costs the grouping, not the
            // session: the thread comes back as a process of its own, which is
            // what every thread was before groups were recorded at all.
            if (thread && own.leader == NULL) {
                printk("WARNING: checkpoint: pid %u's thread group %u is not in "
                       "the image; it comes back as a process of its own\n",
                       rec.pid, rec.tgid);
                rec.tgid = rec.pid;
                thread = false;
            }
            own.mm = ckpt_built(built, nbuilt, rec.mm_owner);
            own.files = ckpt_built(built, nbuilt, rec.files_owner);
            own.fs = ckpt_built(built, nbuilt, rec.fs_owner);
            own.sighand = ckpt_built(built, nbuilt, rec.sighand_owner);
            if ((rec.mm_owner != 0 && own.mm == NULL) ||
                    (rec.files_owner != 0 && own.files == NULL) ||
                    (rec.fs_owner != 0 && own.fs == NULL) ||
                    (rec.sighand_owner != 0 && own.sighand == NULL)) {
                ckpt_refuse("pid %u shares with a task the image does not have "
                            "(tgid %u, owners %u/%u/%u/%u)", rec.pid, rec.tgid,
                            rec.mm_owner, rec.files_owner, rec.fs_owner,
                            rec.sighand_owner);
                err = _EINVAL;
                goto out;
            }
            task = ckpt_new_task(parent, (pid_t_) rec.pid, &own);
            if (task == NULL) {
                ckpt_refuse("could not recreate pid %u", rec.pid);
                err = _EAGAIN;
                goto out;
            }
            // Frozen from birth, so nothing runs until every task is built.
            atomic_store_explicit(&task->ckpt_freeze_wanted, true,
                                  memory_order_release);
        }
        task->ckpt_restored = true;
        task->ckpt_syscalls_traced = 0;
        task->start_time_ticks = rec.start_time_ticks;
        built[nbuilt++] = task;

        if (rec.departed) {
            // As do_exit left it: exited, off its group's thread list, holding
            // no address space, descriptors, fs or handlers, and still the
            // task its group's exit will be reported as. Its threads, later in
            // the image, join its group.
            CKPT_TRACE("load pid %u (ppid %u) %s: DEPARTED LEADER, exit code %#x\n",
                       rec.pid, rec.ppid, rec.comm, rec.exit_code);
            memcpy(task->comm, rec.comm, sizeof(task->comm));
            task->exit_code = rec.exit_code;
            task->exit_signal = rec.exit_signal;
            complex_lockt(&pids_lock, 0);
            lock(&task->group->lock, 0);
            list_remove(&task->group_links);
            unlock(&task->group->lock);
            unlock(&pids_lock);
            lock(&task->general_lock, 0);
            struct mm *dead_mm = task->mm;
            struct fdtable *dead_files = task->files;
            struct fs_info *dead_fs = task->fs;
            struct sighand *dead_sighand = task->sighand;
            task->mm = NULL;
            task->mem = NULL;
            task->cpu.mmu = NULL;
            task->files = NULL;
            task->fs = NULL;
            task->sighand = NULL;
            unlock(&task->general_lock);
            mm_release(dead_mm);
            fdtable_release(dead_files);
            fs_info_release(dead_fs);
            sighand_release(dead_sighand);
            task->exiting = true;
            atomic_store_explicit(&task->exit_finished, true, memory_order_release);
            atomic_store_explicit(&task->ckpt_freeze_wanted, false,
                                  memory_order_release);
            continue;   // no register file, no maps, no descriptors follow
        }

        if (rec.zombie) {
            CKPT_TRACE("load pid %u (ppid %u) %s: ZOMBIE, exit code %#x\n",
                       rec.pid, rec.ppid, rec.comm, rec.exit_code);
            memcpy(task->comm, rec.comm, sizeof(task->comm));
            task->exit_code = rec.exit_code;
            task->exit_signal = rec.exit_signal;
            // Off its group's thread list, where exit_tgroup left the real
            // one: wait() will not reap a process that still has threads
            // (process_has_threads_locked), so a zombie rebuilt on the list
            // made its parent's wait block for ever. Never seen until zombies
            // were saved at all -- the collection had been skipping them.
            complex_lockt(&pids_lock, 0);
            lock(&task->group->lock, 0);
            list_remove(&task->group_links);
            unlock(&task->group->lock);
            unlock(&pids_lock);
            task->zombie = true;
            // No thread will ever run do_exit for it, so nothing else says it
            // is finished -- and a zombie that is not is never freed once
            // reaped (task_destroy_unlinked).
            atomic_store_explicit(&task->exit_finished, true, memory_order_release);
            atomic_store_explicit(&task->ckpt_freeze_wanted, false,
                                  memory_order_release);
            continue;   // no register file, no maps, no descriptors follow
        }

        CKPT_TRACE("load pid %u (tgid %u ppid %u pgid %u sid %u) %s: %u maps, %u fds, "
                   "shares %u/%u/%u/%u, tty %s\n",
                   rec.pid, rec.tgid, rec.ppid, rec.pgid, rec.sid, rec.comm,
                   rec.n_maps, rec.n_fds, rec.mm_owner, rec.files_owner,
                   rec.fs_owner, rec.sighand_owner,
                   rec.tty_kind == CKPT_TTY_PTS ? "pts" :
                   rec.tty_kind == CKPT_TTY_CONSOLE ? "console" : "none");
        st.native_name = st.native_argv = st.native_state = st.native_env = NULL;
        st.native_standin_child = 0;
        // ISH_CHECKPOINT_TEST_FAIL_PID=<pid>: fail the restore at that task,
        // after every task before it is built -- the shape of the device's
        // EACCES, which made a restore fail with a partly built machine behind
        // it. Only a test knob: nothing but tests/manual sets it.
        {
            const char *fp = getenv("ISH_CHECKPOINT_TEST_FAIL_PID");
            if (fp != NULL && fp[0] != '\0' && (uint32_t) atoi(fp) == rec.pid) {
                ckpt_refuse("test: failing the restore at pid %u", rec.pid);
                err = _EIO;
                goto out;
            }
        }
        struct task *saved = current;
        current = task;
        err = ckpt_restore_task(f, &h, &rec, &st);
        current = saved;
        if (err == 0 && rec.native)
            err = ckpt_dispatch_native(task, &st);
        free(st.native_name); free(st.native_argv);
        free(st.native_state); free(st.native_env);
        st.native_name = st.native_argv = st.native_state = st.native_env = NULL;
        if (err < 0)
            goto out;
    }

    // Every descriptor in the image exists now, so the things that point at
    // descriptors elsewhere can be put back: epoll registrations, and pidfds.
    ckpt_restore_fifos(&st);
    // Before the epoll registrations: an event loop may be watching the very
    // descriptor that was waiting for its terminal (tmux's is).
    ckpt_restore_pending_ttys(&st);
    if ((err = ckpt_restore_epoll_regs(f, &h, &st)) < 0)
        goto out;
    ckpt_restore_pidfds(&st);

    // Every task exists and is complete; now let them go. The freezer's own
    // parking lot does the releasing, so a restored task and a checkpointed
    // one wait in exactly the same place.
    atomic_fetch_add_explicit(&ckpt_restoring, 1, memory_order_acq_rel);
    atomic_fetch_add_explicit(&ckpt_freeze_active, 1, memory_order_acq_rel);
    for (unsigned i = 1; i < nbuilt; i++) {
        if (built[i]->zombie || built[i]->exiting)
            continue;   // nothing to run; it is a status waiting to be read
        CKPT_TRACE("starting restored pid %d\n", built[i]->pid);
        if (task_start(built[i]) < 0) {
            ckpt_refuse("could not start restored pid %d", built[i]->pid);
            err = _EAGAIN;
            ckpt_thaw_all();
    atomic_fetch_sub_explicit(&ckpt_restoring, 1, memory_order_acq_rel);
            goto out;
        }
    }
    // The timers, now that nothing can fail and every task they signal has
    // its thread -- all but the first, which a signal before its start only
    // queues (signal_wake_task). Before the thaw, so no guest code runs with a
    // timer of its missing.
    for (uint32_t i = 0; i < st.timers_count; i++) {
        struct ckpt_group_timers *t = &st.timers[i];
        unsigned failed = group_timers_ckpt_arm(t->group, &t->d, t->posix);
        CKPT_TRACE("armed pid %u's timers: itimer %s, %u POSIX timers, %u failed\n",
                   t->pid, t->d.real.armed ? "armed" : "not armed", t->d.n_posix, failed);
        if (failed != 0) {
            char why[96];
            snprintf(why, sizeof(why), "%u of its timers could not be armed again", failed);
            ckpt_note_restore(t->pid, 0, why);
            printk("WARNING: checkpoint: pid %u: %s\n", t->pid, why);
        }
    }
    ckpt_thaw_all();
    atomic_fetch_sub_explicit(&ckpt_restoring, 1, memory_order_acq_rel);

    // NO redraw nudge here. It was tried and it crashed the app.
    //
    // A SIGWINCH to each restored terminal was meant to make a resumed shell
    // reprint its prompt. But a re-launched zsh loads the zle module during
    // its own startup, and zle's getbyte calls zrefresh whenever `resetneeded`
    // is set -- which is exactly what a WINCH sets. The signal landed inside
    // query_terminal, so zrefresh ran before zle had built a prompt, and
    // countprompt(lpromptbuf) dereferenced NULL (deps/zsh Src/Zle/
    // zle_refresh.c:770). Device crash 2026-09-12, a pty attach in the
    // breadcrumbs in the same second.
    //
    // It is also no longer needed: the workspace now carries each terminal's
    // screen and scrollback across the suspend, and that history already ENDS
    // with the prompt the shell had printed. Restoring what was on the screen
    // is a better answer than asking the shell to draw it again.
    lock(&ckpt_lock, 0);
    // Hand the restored sessions to the UI. Published only on success: a
    // restore that failed half way leaves tasks that are about to be torn
    // down, and a window adopting one of those would show a corpse.
    ckpt_session_count = ckpt_session_taken = 0;
    for (uint32_t i = 0; i < st.set_count &&
                         ckpt_session_count < (sizeof(ckpt_sessions) /
                                               sizeof(ckpt_sessions[0])); i++) {
        if (st.sets[i].kind != CKPT_TTY_PTS || st.sets[i].terminal == NULL)
            continue;
        ckpt_sessions[ckpt_session_count++] = (struct checkpoint_restored_session) {
            .leader_pid = st.sets[i].leader_pid,
            .tty_num = st.sets[i].tty_num,
            .terminal = st.sets[i].terminal,
        };
    }
    ckpt_status.restored = true;
    ckpt_status.generation++;
    snprintf(ckpt_status.last_path, sizeof(ckpt_status.last_path), "%s", host_path);
    ckpt_status.pages = (unsigned long) h.total_pages;
    ckpt_status.bytes = (unsigned long long) h.total_pages * PAGE_SIZE;
    ckpt_status.tasks = h.n_tasks;
    unlock(&ckpt_lock);
    err = 0;

out:
    // The seccomp filters the restore shared between tasks, which each task
    // now holds for itself.
    seccomp_ckpt_import_done();
    // The restore's own references; each task holds its own.
    for (uint32_t i = 0; i < st.set_count; i++)
        for (unsigned j = 0; j < 3; j++)
            if (st.sets[i].stdio[j] != NULL)
                fd_close(st.sets[i].stdio[j]);
    for (unsigned j = 0; j < 3; j++)
        if (st.host_stdio[j] != NULL)
            fd_close(st.host_stdio[j]);
    for (uint32_t i = 0; i < st.id_count; i++)
        if (st.by_id[i] != NULL)
            fd_close(st.by_id[i]);
    // Both ends of every pipe: pipe_create_pair hands each over with one
    // reference, and this is where it goes.
    for (uint32_t i = 0; i < st.pipe_count; i++) {
        fd_close(st.pipes[i].rd);
        fd_close(st.pipes[i].wr);
    }
    free(st.by_id);
    free(st.pipes);
    free(st.sets);
    free(st.pidfds);
    free(st.fifos_primed);
    free(st.fifo_writers);
    for (uint32_t i = 0; i < st.sockpair_count; i++)
        for (int e = 0; e < 2; e++)
            if (!st.sockpairs[i].claimed[e] && st.sockpairs[i].end[e] != NULL)
                fd_close(st.sockpairs[i].end[e]);
    free(st.sockpairs);
    free(st.pending_ttys);
    for (uint32_t i = 0; i < st.timers_count; i++)
        free(st.timers[i].posix);
    free(st.timers);
    if (err < 0 && (nbuilt > 0 || ckpt_restore_mounted_n > 0))
        ckpt_restore_unwind(built, nbuilt, current);
    else
        for (unsigned i = 0; i < ckpt_restore_mounted_n; i++)
            free(ckpt_restore_mounted[i]);
    ckpt_restore_mounted_n = 0;
    // A restore that failed must not leave the image's clocks behind for the
    // fresh boot that follows it, which would start with the image's uptime.
    if (err < 0 && clocks_resumed) {
        guest_clock_restart(boot_before_restore);
        if (nbuilt > 0 && built[0] == current)
            current->start_time_ticks = 0;   // init, again: born at boot
    }
    free(built);
    fclose(f);
    return err;
}

// ------------------------------------------------------------ the trigger

static _Atomic bool ckpt_guest_control;

void checkpoint_set_guest_control(bool allowed) {
    atomic_store_explicit(&ckpt_guest_control, allowed, memory_order_release);
}

bool checkpoint_guest_control(void) {
    return atomic_load_explicit(&ckpt_guest_control, memory_order_acquire);
}

void checkpoint_set_session(const char *host_path) {
    lock(&ckpt_lock, 0);
    snprintf(ckpt_session_path, sizeof(ckpt_session_path), "%s",
             host_path != NULL ? host_path : "");
    unlock(&ckpt_lock);
}

const char *checkpoint_session(void) {
    // Read without the lock: it is written once, by the entry point, before
    // any guest task exists.
    return ckpt_session_path;
}

int checkpoint_request(const char *host_path, bool and_halt) {
    // Refuse NOW for anything that would stop the deferred save from
    // happening at all -- see the note in checkpoint.h.
    int err = ckpt_check_scope();
    if (err < 0) {
        lock(&ckpt_lock, 0);
        ckpt_status.last_err = err;
        unlock(&ckpt_lock);
        return err;
    }
    lock(&ckpt_lock, 0);
    snprintf(ckpt_pending_path, sizeof(ckpt_pending_path), "%s", host_path);
    ckpt_pending = true;
    ckpt_pending_halt = and_halt;
    ckpt_pending_pid = current != NULL ? current->pid : 0;
    ckpt_status.last_err = 0;
    ckpt_status.last_refusal[0] = '\0';
    unlock(&ckpt_lock);
    return 0;
}

// Whether the task that asked for a pending checkpoint can still take it.
static bool ckpt_asker_alive(pid_t_ asker) {
    complex_lockt(&pids_lock, 0);
    struct task *t = pid_get_task((dword_t) asker);
    bool alive = t != NULL && !t->zombie && !t->exiting;
    unlock(&pids_lock);
    return alive;
}

// The request is taken by the task that MADE it, at its next boundary -- "one
// pass later", with nothing of its own run in between.
//
// Any task used to take it, whichever reached the top of its loop first, and a
// busy one usually won: a child starting up makes a syscall every few
// microseconds, the shell that asked makes one only when it gets back. Taking
// the request is not freezing the machine, though -- the taker still has to
// get through checkpoint_save's own setup before ckpt_freeze_all stops
// anyone -- and in that gap the asker, finding nothing pending, went on
// running. `echo suspend > /proc/ish/checkpoint; echo after` printed "after"
// six times out of six with a busy child, and the image held a shell that was
// past its own suspend. checkpoint_restore.sh's two-process leg saw it about
// one run in six. Another task still takes it once the asker is gone -- one
// that asked and exited must not lose its suspend.
void checkpoint_run_pending(void) {
    char path[PATH_MAX];
    bool halt_after;
    lock(&ckpt_lock, 0);
    bool want = ckpt_pending;
    pid_t_ asker = ckpt_pending_pid;
    unlock(&ckpt_lock);
    if (!want)
        return;
    if (asker != 0 && (current == NULL || current->pid != asker) &&
            ckpt_asker_alive(asker))
        return;
    lock(&ckpt_lock, 0);
    want = ckpt_pending;
    if (want) {
        memcpy(path, ckpt_pending_path, sizeof(path));
        halt_after = ckpt_pending_halt;
        ckpt_pending = false;
    }
    unlock(&ckpt_lock);
    if (!want)
        return;

    // If the task that asked is ITSELF a native program, it describes itself
    // here. The freezer only asks the others -- this one is not frozen, it is
    // the one doing the freezing -- and its state, like theirs, exists only on
    // its own thread. Reached from native_checkpoint(), which is the only
    // place a native program comes back through.
    const struct native_program *self = native_program_running(current);
    if (self != NULL && self->ckpt_dump != NULL &&
            current->ckpt_native_state == NULL) {
        ckpt_dumping = true;
        current->ckpt_native_state = self->ckpt_dump();
        ckpt_dumping = false;
    }

    int err = checkpoint_save(path);
    if (err < 0) {
        lock(&ckpt_lock, 0);
        ckpt_status.last_err = err;
        CKPT_TRACE("save failed: %d -- %s\n", err,
                   ckpt_status.last_refusal[0] ? ckpt_status.last_refusal
                                               : "(no reason recorded)");
        unlock(&ckpt_lock);
        free(current->ckpt_native_state);
        current->ckpt_native_state = NULL;
        return;
    }
    // Consumed, whichever way it went: it describes a moment that has passed,
    // and leaving it would have the next checkpoint write a stale state.
    free(current->ckpt_native_state);
    current->ckpt_native_state = NULL;

    if (!halt_after)
        return;

    // A SUSPEND: the image is written, so this guest's job is done and the
    // next launch is the one that continues it. Stopping the machine rather
    // than exiting the task -- an exit would run the guest's own shutdown,
    // and the image already describes a process that is very much alive.
    CKPT_TRACE("suspending: image written, halting\n");
    if (halt_hook != NULL)
        halt_hook(0);
    exit(0);
}
