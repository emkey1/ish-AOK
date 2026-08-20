#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
#include "xX_main_Xx.h"

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
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
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
    p += sprintf(p, "TERM=%s", term) + 1;
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
        extern void fakefs_lockstats_dump(void); // no-op unless ISH_FAKEFS_LOCKSTATS
        fakefs_lockstats_dump();
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
static void ensure_dev_node(const char *path, int major, int minor) {
    dev_t_ dev = dev_make(major, minor);
    struct statbuf stat;
    int err = generic_statat(AT_PWD, path, &stat, false);
    if (err == 0 && S_ISCHR(stat.mode) && stat.rdev == dev)
        return;
    if (err == 0)
        generic_unlinkat(AT_PWD, path);
    ignore_eexist(generic_mknodat(AT_PWD, path, S_IFCHR | 0666, dev));
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
    usleep(300 * 1000);
    unsigned straggling = 0;
    bool drained = fakefs_quiesce_begin(2000, &straggling);
    fprintf(stderr, "[quiesce] engaged: drained=%d straggling=%u\n", drained, straggling);
    // Hold it briefly: guest tasks wanting a transaction must park, not spin or
    // deadlock, and must not be holding fs->lock while they wait.
    usleep(500 * 1000);
    fakefs_quiesce_end();
    fprintf(stderr, "[quiesce] lifted\n");
    return NULL;
}

int main(int argc, char *const argv[]) {
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
    if (getenv("ISH_FAKEFS_LOCKSTATS") != NULL) {
        extern int fakefs_lockstats_fd;
        int fd = dup(STDERR_FILENO);
        if (fd >= 0)
            fakefs_lockstats_fd = fd;
    }

    char *envp = build_initial_envp();
    if (envp == NULL) {
        fprintf(stderr, "malloc: %s\n", strerror(errno));
        return 1;
    }

    int err = xX_main_Xx(argc, argv, envp);
    free(envp);
    if (err < 0) {
        fprintf(stderr, "xX_main_Xx: %s\n", strerror(-err));
        return 1;
    }

    setup_host_mounts();

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

        struct guest_command_result r;
        int rc = run_guest_command_capture(test_cmd, NULL, 10000, 0, &r);
        if (quiesce_test)
            pthread_join(quiesce_thread, NULL);
        fprintf(stderr,
                "[guest-cmd] rc=%d launched=%d exited=%d code=%d sig=%d timed_out=%d truncated=%d len=%zu\n",
                rc, r.launched, r.exited, r.exit_code, r.term_signal,
                r.timed_out, r.truncated, r.output_len);
        fprintf(stderr, "[guest-cmd] ---output---\n%s\n[guest-cmd] ---end---\n",
                r.output != NULL ? r.output : "(null)");
        free(r.output);
        _exit(0);
    }

    task_run_current();
}
