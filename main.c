#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <pthread.h>

#include "fs/dev.h"
#include "fs/devices.h"
#include "fs/fd.h"
#include "fs/path.h"
#include "fs/real.h"
#include "fs/stat.h"
#include "jit/jit.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "kernel/swap.h"
#include "kernel/checkpoint.h"
#include "xX_main_Xx.h"
#include "platform/platform.h"

extern void run_at_boot(void);

static void configure_standalone_i386_safety(int argc, char *const argv[]) {
#if defined(__APPLE__) && defined(__aarch64__)
    int saved_optind = optind;
    int saved_opterr = opterr;
    optind = 1;
    opterr = 0;

    int opt;
    while ((opt = getopt(argc, argv, "+r:f:d:c:")) != -1) {
        switch (opt) {
            case 'r':
            case 'f':
            case 'd':
            case 'c':
                break;
            default:
                optind = saved_optind;
                opterr = saved_opterr;
                return;
        }
    }

    const char *command = optind < argc ? argv[optind] : NULL;
    optind = saved_optind;
    opterr = saved_opterr;

    const char *force_jit = getenv("ISH_HOST_I386_JIT");
    if (force_jit == NULL)
        return;

    if (strcmp(force_jit, "1") == 0 || strcasecmp(force_jit, "true") == 0 ||
            strcasecmp(force_jit, "yes") == 0 || strcasecmp(force_jit, "on") == 0)
        return;

    if (command == NULL)
        return;

    const char *basename = strrchr(command, '/');
    const char *comm = basename != NULL ? basename + 1 : command;
    if (comm == NULL || comm[0] == '\0')
        return;

    i386_single_step_comm_set(comm);
    i386_no_cache_comm_set(comm);
#else
    (void) argc;
    (void) argv;
#endif
}

static void configure_standalone_amd64_jit(void) {
    const char *force_jit = getenv("ISH_HOST_AMD64_JIT");
    if (force_jit == NULL) {
        // The amd64 JIT is only implemented and validated on aarch64 hosts (the
        // iOS target). On other hosts the gadget path is incomplete and SIGSEGVs
        // on even trivial amd64 programs, so default it off and run the
        // interpreter; force it on for development with ISH_HOST_AMD64_JIT=1.
#if defined(__aarch64__)
        amd64_jit_set_enabled(true);
#else
        amd64_jit_set_enabled(false);
#endif
        return;
    }

    if (strcmp(force_jit, "1") == 0 || strcasecmp(force_jit, "true") == 0 ||
            strcasecmp(force_jit, "yes") == 0 || strcasecmp(force_jit, "on") == 0) {
        amd64_jit_set_enabled(true);
        return;
    }

    if (strcmp(force_jit, "0") == 0 || strcasecmp(force_jit, "false") == 0 ||
            strcasecmp(force_jit, "no") == 0 || strcasecmp(force_jit, "off") == 0) {
        amd64_jit_set_enabled(false);
        return;
    }
}

// The guest gets a minimal, predictable environment rather than the host's --
// but PATH and HOME must be in it. Without PATH, execvp/posix_spawnp in the
// guest fall back to libc's narrow default, and anything that locates its own
// helpers via a PATH self-search fails confusingly (gcc invoked as plain "gcc"
// couldn't find cc1). Same defaults as run_guest_command_capture's in
// kernel/init.c; TERM passes through from the host when set.
static char *build_initial_envp(void) {
    static const char path_var[] =
        "PATH=/AOK/persist/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    static const char home_var[] = "HOME=/root";
    const char *term = getenv("TERM");
    if (term == NULL)
        term = "dumb";
    size_t term_size = sizeof("TERM=") + strlen(term); // includes the NUL

    // NUL-separated variables, terminated by an empty string (do_execve's
    // envp format; see args_size in kernel/exec.c).
    char *envp = malloc(sizeof(path_var) + sizeof(home_var) + term_size + 1);
    if (envp == NULL)
        return NULL;
    char *p = envp;
    memcpy(p, path_var, sizeof(path_var));
    p += sizeof(path_var);
    memcpy(p, home_var, sizeof(home_var));
    p += sizeof(home_var);
    p += snprintf(p, term_size + 1, "TERM=%s", term) + 1;
    *p = '\0';
    return envp;
}

// Invoked (via halt_hook) when guest init exits. Mirror init's wait-status as the
// host process exit code, then terminate immediately. Using _exit (after flushing
// stdio) rather than returning is deliberate: this runs on whichever guest thread
// happened to finalize the teardown, while pids_lock and the task's general_lock
// are still held — _exit avoids atexit handlers that might re-enter those locks,
// and lets the OS reclaim every lingering guest pthread cleanly instead of the
// pthread_kill(SIGKILL) sweep that would otherwise kill us with signal 9.
// See ISH_CHECKPOINT_AFTER below. A host thread, deliberately: the point is to
// exercise the path the app uses, which is not a guest task either.
double cli_checkpoint_delay;
const char *cli_checkpoint_path;
static void *cli_checkpoint_after(void *unused) {
    (void) unused;
    while (cli_checkpoint_path == NULL)
        usleep(1000);
    usleep((useconds_t) (cli_checkpoint_delay * 1000000));
    int err = checkpoint_save_external(cli_checkpoint_path);
    // To a file beside the image, not to stderr: by the time this runs the
    // guest may have closed the host's standard streams on its way out, and a
    // diagnostic that vanishes is worse than none.
    char log[PATH_MAX];
    snprintf(log, sizeof(log), "%s.log", cli_checkpoint_path);
    FILE *lf = fopen(log, "w");
    if (lf != NULL) {
        struct checkpoint_status ck;
        checkpoint_get_status(&ck);
        fprintf(lf, "%s (%d) %s\n", err == 0 ? "written" : "refused", err,
                ck.last_refusal);
        fclose(lf);
    }
    return NULL;
}

static noreturn void cli_halt(int status) {
    if (getenv("ISH_QUIESCE_STATS") != NULL) {
        extern void quiesce_stats_dump(const char *tag);
        quiesce_stats_dump("exit");
    }
    {
        extern void hle_stats_dump(void); // no-op unless ISH_HLE_STATS counted calls
        hle_stats_dump();
    }
    {
        extern void jit_timing_dump(void); // no-op unless ISH_JIT_TIMING counted compiles
        jit_timing_dump();
    }
    {
        extern void lockstats_dump(void); // no-op unless a lockstats knob is set
        lockstats_dump();
    }
    // Deliberately NOT fflush(NULL). That walks every host stream and takes
    // each one's lock, and the shim gives a native program host FILEs for its
    // stdout and stderr -- so a guest task killed inside stdio leaves a stream
    // lock held by a thread that is gone, and Darwin does not release a mutex
    // when its owner dies. This process then waits here for ever: two were
    // found at 0% CPU, 5 and 23 hours after their guest had exited, blocked in
    // _fwalk -> sflush_locked -> flockfile. Flush the same streams, but skip
    // any whose lock cannot be taken -- see nlibc_flush_all_streams().
    {
        extern void nlibc_flush_stream_if_lockable(FILE *stream);
        extern void nlibc_flush_all_streams(void);
        nlibc_flush_stream_if_lockable(stdout);
        nlibc_flush_stream_if_lockable(stderr);
        nlibc_flush_all_streams();
    }
    if ((status & 0x7f) == 0)          // WIFEXITED
        _exit((status >> 8) & 0xff);
    _exit(128 + (status & 0x7f));      // WIFSIGNALED: shell convention 128+signo
}

static void ignore_eexist(int err) {
    if (err < 0 && err != _EEXIST)
        fprintf(stderr, "warning: setup step failed: %s\n", strerror(-err));
}

// iSH has no devtmpfs (fs/mount.c accepts the guest's mount as a no-op and
// relies on the rootfs image already having these baked in). Many
// Docker-exported rootfs tarballs (Arch's included) can't ship real device
// nodes and instead pack a plain regular-file stand-in at /dev/null (or omit
// the rest of the standard set entirely) -- opening or writing that "device"
// then just accumulates real bytes on the fakefs backing store forever
// instead of discarding them. Repair the standard set here at every boot so
// it doesn't matter what the source tarball shipped.
// `fmt` is S_IFCHR or S_IFBLK. It is a parameter rather than a constant because
// /dev/aokswap0 is a block device, and a node of the wrong TYPE is exactly what
// this function exists to repair -- comparing against S_ISCHR unconditionally
// would have it recreate the block node on every boot, forever.
static void ensure_dev_node_typed(const char *path, int major, int minor, mode_t_ mode,
                                  mode_t_ fmt) {
    dev_t_ dev = dev_make(major, minor);
    struct statbuf stat;
    int err = generic_statat(AT_PWD, path, &stat, false);
    if (err == 0 && (stat.mode & S_IFMT) == fmt && stat.rdev == dev)
        return;
    if (err == 0)
        generic_unlinkat(AT_PWD, path);
    ignore_eexist(generic_mknodat(AT_PWD, path, fmt | mode, dev));
}

static void ensure_dev_node_mode(const char *path, int major, int minor, mode_t_ mode) {
    ensure_dev_node_typed(path, major, minor, mode, S_IFCHR);
}

static void ensure_dev_node(const char *path, int major, int minor) {
    ensure_dev_node_mode(path, major, minor, 0666);
}

static void setup_host_mounts(void) {
    ignore_eexist(generic_mkdirat(AT_PWD, "/dev", 0755));
    ensure_dev_node("/dev/null", MEM_MAJOR, DEV_NULL_MINOR);
    ensure_dev_node("/dev/zero", MEM_MAJOR, DEV_ZERO_MINOR);
    ensure_dev_node("/dev/full", MEM_MAJOR, DEV_FULL_MINOR);
    ensure_dev_node("/dev/random", MEM_MAJOR, DEV_RANDOM_MINOR);
    ensure_dev_node("/dev/urandom", MEM_MAJOR, DEV_URANDOM_MINOR);
    ensure_dev_node("/dev/tty", TTY_ALTERNATE_MAJOR, DEV_TTY_MINOR);
    ensure_dev_node("/dev/ptmx", TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR);
    ensure_dev_node("/dev/fuse", MISC_MAJOR, DEV_FUSE_MINOR);
    // The swap area as a block device, so /proc/swaps has a real path to name.
    // brw-rw---- like a Linux swap device, and present whether or not swap is
    // enabled -- an unbound block node is an ordinary Linux state, and creating
    // it from the enable path is not an option (generic_mknodat needs a valid
    // `current` and takes filesystem locks, and swap_enable is reached from the
    // app's preference path where `current` is not the caller you expect).
    ensure_dev_node_typed("/dev/aokswap0", AOKSWAP_MAJOR, DEV_AOKSWAP_MINOR, 0660, S_IFBLK);
    // The kernel log, which the driver has always implemented (fs/mem.c) and
    // no root has ever had a node for: every syslog daemon starts by opening
    // it, and busybox's klogd and rsyslog's imklog both want /dev/kmsg.
    // 0644, matching Linux's 1:11 node: an unprivileged process may read the
    // log but not write it, and a 0666 node would let it open for writing and
    // only then be refused.
    ensure_dev_node_mode("/dev/kmsg", MEM_MAJOR, DEV_KMSG_MINOR, 0644);
    // systemd's getty@tty1.service (and friends) carry
    // ConditionPathExists=/dev/tty0 -- the Linux "current VT" alias -- and
    // silently skip without it, so a systemd guest finishes booting with no
    // login on the console. The condition only stat()s the node; agetty
    // itself opens /dev/tty1. Provide both (Arch minirootfs tarballs ship
    // neither). vconsole-setup stays skipped regardless (verified), and a
    // stray open of tty0 just yields an unattached tty.
    ensure_dev_node("/dev/tty0", TTY_CONSOLE_MAJOR, 0);
    ensure_dev_node("/dev/tty1", TTY_CONSOLE_MAJOR, 1);
    // /dev/console is created by the iOS app (AppDelegate.m's
    // EnsureCharacterDevice) but was missing from this CLI repair set, so a
    // rootfs tarball that doesn't ship it (the x86_64 Arch minirootfs is one)
    // booted the CLI with no /dev/console at all -- systemd (and everything
    // that writes boot status or opens the console) then silently got ENOENT.
    // Same node the app and the aarch64 image use (5:1).
    ensure_dev_node("/dev/console", TTY_ALTERNATE_MAJOR, DEV_CONSOLE_MINOR);
    // The root is a device now (/proc/diskstats, /sys/block); say so where
    // userland looks for the list of filesystems. See kernel/init.c.
    ensure_root_fstab_entry();
    // /dev/fd and the three std* links, without which bash process
    // substitution -- `diff <(a) <(b)` -- is ENOENT in every guest.
    ensure_dev_fd_links();
    ignore_eexist(generic_mkdirat(AT_PWD, "/dev/pts", 0755));
    // Not every bundled root's base tarball ships /dev/shm, and iSH has no
    // boot-time tmpfs auto-mount for it; create it unconditionally so POSIX
    // shm (wl_shm clients, sem_open, etc.) always has somewhere to open.
    ignore_eexist(generic_mkdirat(AT_PWD, "/dev/shm", 01777));
    // ...and enforce the mode when it already exists: mkdirat is an EEXIST
    // no-op, so a pre-existing /dev/shm with the wrong mode (e.g. 0755
    // root:root from a root image) breaks every non-root shm_open() with
    // EACCES. See the matching AppDelegate.m fix (Wayland session died on
    // first keyboard attach because wlroots couldn't allocate the keymap
    // shm file early in boot).
    generic_setattrat(AT_PWD, "/dev/shm", (struct attr) {.type = attr_mode, .mode = S_IFDIR|01777}, false);
    // /tmp gets the same enforcement: Linux guarantees it 1777, and a rootfs
    // whose /tmp came through stricter locks every non-root session out of
    // temp-file creation (see the matching AppDelegate.m fix -- the Wayland
    // Display session's /tmp handshake files all failed with EACCES when the
    // session ran as the default user).
    generic_setattrat(AT_PWD, "/tmp", (struct attr) {.type = attr_mode, .mode = S_IFDIR|01777}, false);
    ignore_eexist(generic_mkdirat(AT_PWD, "/proc", 0555));
    ignore_eexist(generic_mkdirat(AT_PWD, "/sys", 0555));

    // aokfs's inline/generated content (README, /tools, /tests, /docs) needs no
    // backing files, but the few bundled real files (test audio, the benchmark
    // tarball) are read from the source tree via mount->source. Resolve that
    // against the source root baked in at build time rather than the process's
    // CWD -- a bare `access("tests/audio", R_OK)` only succeeded when ish was
    // launched from the repo root, so running it from e.g. the build directory
    // silently skipped mounting /AOK entirely (leaving whatever bare directory,
    // if any, the guest rootfs already had at that path).
#ifdef ISH_SOURCE_ROOT
    const char *aok_source_root = ISH_SOURCE_ROOT;
#else
    const char *aok_source_root = ".";
#endif
    char aok_audio_check[MAX_PATH + 1];
    snprintf(aok_audio_check, sizeof(aok_audio_check), "%s/tests/audio", aok_source_root);
    if (access(aok_audio_check, R_OK) == 0) {
        ignore_eexist(generic_mkdirat(AT_PWD, "/AOK", 0555));
        ignore_eexist(do_mount(&aokfs, aok_source_root, "/AOK", "", MS_READONLY_));
    }

    ignore_eexist(do_mount(&procfs, "proc", "/proc", "", 0));
    ignore_eexist(do_mount(&sysfs, "sysfs", "/sys", "", 0));
    ignore_eexist(do_mount(&devptsfs, "devpts", "/dev/pts", "", 0));

    // Dev-only: mount a host directory as realfs at /realmnt to reproduce
    // real-fs-backed behavior (e.g. /AOK/persist) against a local fakefs root.
    const char *real_mnt = getenv("ISH_REAL_MNT");
    if (real_mnt != NULL && real_mnt[0] != '\0') {
        ignore_eexist(generic_mkdirat(AT_PWD, "/realmnt", 0755));
        ignore_eexist(do_mount(&realfs, real_mnt, "/realmnt", "", 0));
    }

    // Dev-only: mount a second fakefs (SQLite-backed) root at /fakemnt2, to
    // reproduce cross-root bugs (e.g. mv between two /AOK/roots-style fakefs
    // mounts) against a local repro rig without the iOS app.
    const char *fake_mnt2 = getenv("ISH_FAKE_MNT2");
    if (fake_mnt2 != NULL && fake_mnt2[0] != '\0') {
        char fake_mnt2_data[MAX_PATH + 1];
        snprintf(fake_mnt2_data, sizeof(fake_mnt2_data), "%s/data", fake_mnt2);
        ignore_eexist(generic_mkdirat(AT_PWD, "/fakemnt2", 0755));
        ignore_eexist(do_mount(&fakefs, fake_mnt2_data, "/fakemnt2", "", 0));
    }
}

// Dev harness for the fakefs suspension quiesce gate (ISH_TEST_QUIESCE), run
// alongside ISH_TEST_GUEST_CMD so there is real transaction traffic to drain.
static void *quiesce_test_thread(void *arg) {
    (void) arg;
    // Let the guest get going and open some transactions first.
    // ISH_TEST_QUIESCE_DELAY_MS moves the window: 300 ms is enough for
    // filesystem traffic, but a swap test has to compile and run a program
    // before there is any eviction to interrupt, and a gate that engages before
    // the thing it gates has started proves nothing.
    const char *delay_env = getenv("ISH_TEST_QUIESCE_DELAY_MS");
    long delay_ms = delay_env != NULL ? strtol(delay_env, NULL, 10) : 300;
    if (delay_ms < 0)
        delay_ms = 0;
    usleep((useconds_t) delay_ms * 1000);
    unsigned straggling = 0;
    bool drained = fakefs_quiesce_begin(2000, &straggling);
    // The pager gets the same gate, for the same reason the app applies it: it
    // writes guest memory to a file, and being mid-write when the process is
    // frozen is not a state to be in. Exercised here so the CLI can test it.
    bool swap_drained = swap_quiesce_begin(2000);
    fprintf(stderr, "[quiesce] engaged: drained=%d straggling=%u swap=%d\n",
            drained, straggling, swap_drained);
    // Hold it briefly: guest tasks wanting a transaction must park, not spin or
    // deadlock, and must not be holding fs->lock while they wait.
    const char *hold_env = getenv("ISH_TEST_QUIESCE_HOLD_MS");
    long hold_ms = hold_env != NULL ? strtol(hold_env, NULL, 10) : 500;
    if (hold_ms < 0)
        hold_ms = 0;
    usleep((useconds_t) hold_ms * 1000);
    fakefs_quiesce_end();
    swap_quiesce_end();
    fprintf(stderr, "[quiesce] lifted\n");
    return NULL;
}


// ---- ISH_CLI_PTY: a session on a PSEUDO-terminal, from the command line ----
//
// The iOS app's shape -- a Terminal object owns the master side -- made
// reachable from the CLI. Without it the CLI runs its command as init on the
// console, and kernel/checkpoint.c's CKPT_TTY_PTS restore path (sessions,
// controlling terminals, foreground process groups -- everything job control
// touches) cannot be exercised outside the app. That made every restore bug a
// four-minute round trip through the simulator UI.
//
// The master side is THIS process: guest output goes to stdout, and a reader
// thread feeds stdin into the terminal a byte at a time, so a full-screen
// program (ktop, an editor) can be driven by typing. For that the invoking
// terminal is put into raw mode -- and put BACK on exit, because leaving
// someone's shell raw is not a debugging aid. Only .write is mandatory to
// fs/tty.c; .init is where the reader starts.
static struct termios cli_pty_saved_termios;
static bool cli_pty_termios_saved;

static void cli_pty_restore_host_terminal(void) {
    if (cli_pty_termios_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &cli_pty_saved_termios);
}

// Which terminal a reader feeds. Not a reference: the reader must not keep the
// terminal alive after its session is over. See cli_pty_read_thread.
struct cli_pty_reader {
    struct tty *tty; // identity only -- compared, never dereferenced
    int type;
    int num;
};

static void *cli_pty_read_thread(void *opaque) {
    struct cli_pty_reader reader = *(struct cli_pty_reader *) opaque;
    free(opaque);
    int in = dup(STDIN_FILENO);
    if (in < 0)
        in = STDIN_FILENO;
    char ch;
    for (;;) {
        ssize_t n = read(in, &ch, 1);
        if (n == 1) {
            // The terminal belongs to the session, not to this thread. When the
            // last descriptor on it closes -- the shell exits, as a restored zsh
            // that read EIO did -- tty_release frees the struct, and a byte
            // arriving after that walked the freed tty->fds into poll_wakeup.
            // Hold a reference across the call instead, re-found by device
            // number exactly as the app's -sendInput does, and stop once the
            // terminal is gone: nothing will ever read it again.
            struct tty *tty = tty_lookup_ref(reader.type, reader.num, reader.tty);
            if (tty == NULL)
                return NULL;
            tty_input(tty, &ch, 1, 0);
            tty_put(tty);
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            usleep(10000);
            continue;
        }
        // EOF (stdin is /dev/null, or a pipe that closed): nothing more will
        // ever arrive, so stop reading rather than spinning on it.
        return NULL;
    }
}

static int cli_pty_init(struct tty *tty) {
    struct winsize winsz;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &winsz) == 0) {
        tty->winsize.col = winsz.ws_col;
        tty->winsize.row = winsz.ws_row;
    }
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &cli_pty_saved_termios) == 0) {
        cli_pty_termios_saved = true;
        atexit(cli_pty_restore_host_terminal);
        struct termios raw = cli_pty_saved_termios;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    // Called from tty_get under ttys_lock, before the tty is in its slot, so
    // the reader's first tty_lookup_ref waits for that lock and finds it.
    struct cli_pty_reader *reader = malloc(sizeof(*reader));
    if (reader == NULL)
        return _ENOMEM;
    *reader = (struct cli_pty_reader) {.tty = tty, .type = tty->type, .num = tty->num};
    pthread_t th;
    if (pthread_create(&th, NULL, cli_pty_read_thread, reader) != 0) {
        free(reader);
        return _EIO;
    }
    pthread_detach(th);
    return 0;
}

static int cli_pty_write(struct tty *UNUSED(tty), const void *buf, size_t len,
                         bool UNUSED(blocking)) {
    return (int) write(STDOUT_FILENO, buf, len);
}

static struct tty_driver_ops cli_pty_ops = { .init = cli_pty_init, .write = cli_pty_write };
static struct tty_driver cli_pty_driver = { .ops = &cli_pty_ops };

static struct tty *cli_pty_open_session(void) {
    struct tty *tty = pty_open_fake(&cli_pty_driver);
    return IS_ERR(tty) ? NULL : tty;
}

int main(int argc, char *const argv[]) {
    // The system's memory-pressure source, which outranks our own per-process
    // headroom arithmetic; see host_mem_pressure_start() in platform/darwin.c.
    host_mem_pressure_start();
    // Before any lock is taken: the lock_t and wrlock_t hooks read this flag on
    // every acquire, and a lock held from before it was armed would be dropped
    // as an unmatched release.
    lockstats_init();
    run_at_boot();
    configure_standalone_i386_safety(argc, argv);
    configure_standalone_amd64_jit();
    // The CLI now defaults to multicore (like the iOS app), so local and fakefs
    // repro runs exercise the same concurrency -- and the same races -- as a
    // multi-core device. The effective lever is the emulated CPU count
    // (get_cpu_count(), >= 4 on the CLI; see platform/darwin.c); doEnableMulticore
    // is a legacy toggle kept in sync for clarity. Set ISH_MULTICORE=0 to flip
    // the toggle back, or ISH_GUEST_CPU_COUNT=1 to actually run a serial guest.
    {
        extern bool doEnableMulticore;
        doEnableMulticore = true;
        const char *mc = getenv("ISH_MULTICORE");
        if (mc != NULL && (strcmp(mc, "0") == 0 || strcasecmp(mc, "false") == 0 ||
                           strcasecmp(mc, "no") == 0 || strcasecmp(mc, "off") == 0))
            doEnableMulticore = false;
    }
    // HLE of fingerprinted guest libc functions (jit/hle.c). Default OFF;
    // ISH_HLE=1 enables it (arm64/riscv64 guests only).
    {
        extern bool doEnableHLE;
        const char *hle = getenv("ISH_HLE");
        if (hle != NULL && strcmp(hle, "0") != 0 && strcasecmp(hle, "false") != 0 &&
                strcasecmp(hle, "no") != 0 && strcasecmp(hle, "off") != 0)
            doEnableHLE = true;
    }
    // Crypto accelerator (kernel/ish_accel.c): host-native ChaCha20-Poly1305
    // via ISH_SYS_AEAD. Default OFF; ISH_CRYPTO_ACCEL=1 enables it (only takes
    // effect if the RFC 8439 self-test passes).
    ish_accel_init();
    {
        extern bool doEnableCryptoAccel;
        const char *ca = getenv("ISH_CRYPTO_ACCEL");
        if (ca != NULL && strcmp(ca, "0") != 0 && strcasecmp(ca, "false") != 0 &&
                strcasecmp(ca, "no") != 0 && strcasecmp(ca, "off") != 0)
            doEnableCryptoAccel = true;
    }
    // Pixman accelerator (kernel/ish_accel_pix.c): host-native FILL/COPY/OVER
    // via ISH_SYS_PIXOP, consumed by the guest-side LD_PRELOAD pixman shim
    // (opt/AOK/pixman/). Default OFF; ISH_PIX_ACCEL=1 enables it (only takes
    // effect if its self-test passes).
    ish_accel_pix_init();
    {
        extern bool doEnablePixAccel;
        const char *pa = getenv("ISH_PIX_ACCEL");
        if (pa != NULL && strcmp(pa, "0") != 0 && strcasecmp(pa, "false") != 0 &&
                strcasecmp(pa, "no") != 0 && strcasecmp(pa, "off") != 0)
            doEnablePixAccel = true;
    }
    // ISH_CLI_PTY=1 -- run the command as a child of init on a PSEUDO-terminal
    // instead of as init on the console, which is the app's shape and the only
    // way to reach the checkpoint's CKPT_TTY_PTS restore path from here. See
    // cli_pty_open_session above and the block in xX_main_Xx.h.
    if (getenv("ISH_CLI_PTY") != NULL) {
        cli_session_tty_open = cli_pty_open_session;
        checkpoint_open_session_tty = cli_pty_open_session;
    }
    // ISH_CHECKPOINT_AFTER=<seconds>:<path> -- take a checkpoint from a thread
    // that is NOT a guest task, after the guest has been running a while.
    //
    // This is the APP's path, exercised where it can be tested: the app
    // backgrounds on its UI thread and has to know the image is on disk before
    // iOS freezes it, which is a different entry point from the guest writing
    // to /proc/ish/checkpoint. Without this the only way to reach
    // checkpoint_save_external would be to run the app.
    {
        const char *spec = getenv("ISH_CHECKPOINT_AFTER");
        if (spec != NULL && *spec != '\0') {
            static char at[PATH_MAX];
            static double delay;
            const char *colon = strchr(spec, ':');
            if (colon != NULL) {
                delay = atof(spec);
                snprintf(at, sizeof(at), "%s", colon + 1);
                pthread_t th;
                pthread_create(&th, NULL, cli_checkpoint_after, NULL);
                pthread_detach(th);
                extern double cli_checkpoint_delay;
                extern const char *cli_checkpoint_path;
                cli_checkpoint_delay = delay;
                cli_checkpoint_path = at;
            }
        }
    }
    halt_hook = cli_halt;
    // hle_stats_dump runs from cli_halt, after guest teardown has closed the
    // (possibly shared) host stderr fd -- give it a private dup now.
    if (getenv("ISH_HLE_STATS") != NULL) {
        extern int hle_stats_fd;
        int fd = dup(STDERR_FILENO);
        if (fd >= 0)
            hle_stats_fd = fd;
    }
    // Same reason as hle_stats_fd immediately above: jit_timing_dump also
    // runs from cli_halt, after guest teardown has closed stderr.
    if (getenv("ISH_JIT_TIMING") != NULL) {
        extern int jit_timing_stats_fd;
        int fd = dup(STDERR_FILENO);
        if (fd >= 0)
            jit_timing_stats_fd = fd;
    }
    // Same again for the fakefs lock stats.
    if (getenv("ISH_FAKEFS_LOCKSTATS") != NULL || getenv("ISH_LOCKSTATS") != NULL) {
        extern int lockstats_fd;
        int fd = dup(STDERR_FILENO);
        if (fd >= 0)
            lockstats_fd = fd;
    }

    char *envp = build_initial_envp();
    if (envp == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        return 1;
    }

    // Runs inside xX_main_Xx, between mounting the root and exec'ing the
    // command, rather than after both. See kernel/init.h: doing it afterwards
    // made every /AOK path (and every /dev node this creates) unreachable to
    // the one command the CLI was asked to run.
    ish_boot_setup_hook = setup_host_mounts;
    int err = xX_main_Xx(argc, argv, envp);
    free(envp);
    if (err < 0) {
        fprintf(stderr, "xX_main_Xx: %s\n", strerror(-err));
        return 1;
    }

    // Dev harness for the LLM-Chat guest-shell tool primitive. With
    // ISH_TEST_GUEST_CMD set, run that command in the guest via
    // run_guest_command_capture(), print the captured result, and exit -- a way
    // to validate the primitive against a local fakefs without the iOS app.
    // e.g. ISH_TEST_GUEST_CMD='echo out; echo err >&2; exit 7' ./ish -f alpinex86 /bin/sh
    const char *test_cmd = getenv("ISH_TEST_GUEST_CMD");
    if (test_cmd != NULL) {
        // With ISH_TEST_QUIESCE set, exercise the suspension quiesce gate
        // against the guest command's live filesystem traffic: engage it while
        // transactions are in flight, confirm the drain reaches zero, then lift
        // it and confirm the guest still finishes. A deadlock or a lost wakeup
        // shows up as this never completing. See fs/fake-db.h.
        pthread_t quiesce_thread;
        bool quiesce_test = getenv("ISH_TEST_QUIESCE") != NULL;
        if (quiesce_test)
            pthread_create(&quiesce_thread, NULL, quiesce_test_thread, NULL);

        // With ISH_TEST_GUEST_USER also set, run as that account via the su
        // path (run_guest_command_capture_user) -- the primitive behind "Open
        // Everything as Default User" for the app's headless command surfaces.
        // Optional ISH_TEST_GUEST_TIMEOUT_MS overrides the 10 s cap, for
        // exercising the process-group timeout kill.
        const char *test_user = getenv("ISH_TEST_GUEST_USER");
        const char *timeout_str = getenv("ISH_TEST_GUEST_TIMEOUT_MS");
        int timeout_ms = timeout_str != NULL ? atoi(timeout_str) : 10000;
        struct guest_command_result r;
        int rc = run_guest_command_capture_user(test_user, test_cmd, NULL, timeout_ms, 0, &r);
        if (quiesce_test)
            pthread_join(quiesce_thread, NULL);
        fprintf(stderr,
                "[guest-cmd] rc=%d launched=%d exited=%d code=%d sig=%d timed_out=%d truncated=%d len=%zu\n",
                rc, r.launched, r.exited, r.exit_code, r.term_signal,
                r.timed_out, r.truncated, r.output_len);
        fprintf(stderr, "[guest-cmd] ---output---\n%s\n[guest-cmd] ---end---\n",
                r.output != NULL ? r.output : "(null)");
        free(r.output);
        // ISH_TEST_GUEST_LINGER_MS keeps the emulator alive after the capture
        // returns, so a process that wrongly survived the timeout kill has time
        // to leave observable evidence (e.g. touch a marker file) before
        // everything dies with the host process.
        const char *linger_str = getenv("ISH_TEST_GUEST_LINGER_MS");
        if (linger_str != NULL)
            usleep((useconds_t) atoi(linger_str) * 1000);
        _exit(0);
    }

    task_run_current();
}
