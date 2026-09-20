#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include "kernel/init.h"
#include "kernel/fs.h"
#include "fs/devices.h"
#include "fs/real.h"
#include "fs/sock.h"
#include "kernel/swap.h"
#include <stdatomic.h>
#include "kernel/checkpoint.h"
#ifdef __APPLE__
#include <sys/resource.h>
#define IOPOL_TYPE_VFS_HFS_CASE_SENSITIVITY 1
#define IOPOL_VFS_HFS_CASE_SENSITIVITY_FORCE_CASE_SENSITIVE 1
#endif

void real_tty_reset_term(void);

static void exit_handler(struct task *task, int code) {
    if (task->parent != NULL)
        return;
    real_tty_reset_term();
    if (code & 0xff)
        raise(code & 0xff);
    exit(code >> 8);
}

// this function parses command line arguments and initializes global
// data structures. thanks programming discussions discord server for the name.
// https://discord.gg/9zT7NHP
static inline int xX_main_Xx(int argc, char *const argv[], const char *envp) {
#ifdef __APPLE__
    // Enable case-sensitive filesystem mode on macOS, if possible.
    // In order for this to succeed, either we need to be running as root, or
    // be given the com.apple.private.iopol.case_sensitivity entitlement. The
    // second option isn't possible so you'll need to give iSH the setuid root
    // bit. In that case it's important to drop root permissions ASAP.
    // https://worthdoingbadly.com/casesensitive-iossim/
    int iopol_err = setiopolicy_np(IOPOL_TYPE_VFS_HFS_CASE_SENSITIVITY,
            IOPOL_SCOPE_PROCESS,
            IOPOL_VFS_HFS_CASE_SENSITIVITY_FORCE_CASE_SENSITIVE);
    if (iopol_err != 0 && errno != EPERM)
        perror("could not enable case sensitivity");
    setgid(getgid());
    setuid(getuid());
#endif

    // parse cli options
    int opt;
    const char *root = NULL;
    const char *workdir = NULL;
    const struct fs_ops *fs = &realfs;
    const char *console = "/dev/tty1";
    while ((opt = getopt(argc, argv, "+r:f:d:c:")) != -1) {
        switch (opt) {
            case 'r':
            case 'f':
                root = optarg;
                if (opt == 'f')
                    fs = &fakefs;
                break;
            case 'd':
                workdir = optarg;
                break;
            case 'c':
                console = optarg;
                break;

        }
    }

    char root_realpath[MAX_PATH + 1] = "/";
    if (root != NULL && realpath(root, root_realpath) == NULL) {
        perror(root);
        exit(1);
    }
    if (fs == &fakefs)
        strcat(root_realpath, "/data");
    int err = mount_root(fs, root_realpath);
    if (err < 0)
        return err;

    become_first_process();
    current->thread = pthread_self();
    // And say so. init does not go through task_start -- the thread that will
    // run it is the one already running -- so the flag task_start sets stayed
    // false for the one task that is always there. Anything keying off it read
    // init as "no host thread yet": kernel/resource.c skipped its CPU time,
    // and kernel/checkpoint.c's freezer would not poke it, so a checkpoint
    // taken from outside the guest timed out on pid 1 every time.
    atomic_store_explicit(&current->host_thread_started, true,
                          memory_order_release);
    // Simulated swap, if and only if the user asked for it. Off by default, so
    // on every ordinary launch this reads one environment variable, finds
    // nothing, and returns -- no file, no allocation, no thread. Here rather
    // than in main.c so the app gets it too; the app's Settings reach it
    // through swap_set_preference() instead of the environment.
    swap_startup();
    netlink_link_watch_start();
    char cwd[MAX_PATH + 1];
    if (root == NULL && workdir == NULL) {
        getcwd(cwd, sizeof(cwd));
        workdir = cwd;
    }
    if (workdir != NULL) {
        struct fd *pwd = generic_open(workdir, O_RDONLY_, 0);
        if (IS_ERR(pwd)) {
            fprintf(stderr, "error opening working dir: %ld\n", PTR_ERR(pwd));
            return 1;
        }
        fs_chdir(current->fs, pwd);
    }

    // Whatever this entry point wants present in the root, before the first
    // program is loaded out of it rather than after (kernel/init.h).
    if (ish_boot_setup_hook != NULL)
        ish_boot_setup_hook();

    // Bring a guest back instead of starting one. This stands in for what the
    // app will do when iOS has killed it: same device, same root, same build.
    // An environment variable rather than a getopt letter because the CLI's
    // option string is "+r:f:d:c:" and everything after the first non-option
    // is the guest's own command line -- a restore has no command line at all.
    // A SESSION: one path that is both where a suspend writes and where the
    // next launch looks. If the file is there the guest resumes; if it is not,
    // it boots normally and the file appears the first time something asks for
    // a suspend. That is the shape the app needs, tried out on the CLI first.
    const char *session = getenv("ISH_SESSION");
    const char *restore_path = getenv("ISH_RESTORE");
    if (session != NULL && session[0] != '\0') {
        checkpoint_set_session(session);
        if (access(session, R_OK) == 0)
            restore_path = session;
    }
    if (restore_path != NULL && restore_path[0] != '\0') {
        // BEFORE the restore, not after it: a restored process can have a
        // terminal open, and re-opening one goes through tty_device_open,
        // which asserts on a major with no driver registered. The ordinary
        // boot path registers it after do_execve because nothing before that
        // point opens a tty; a restore opens several.
        tty_drivers[TTY_CONSOLE_MAJOR] = &real_tty_driver;
        int rerr = checkpoint_restore(restore_path);
        if (rerr < 0) {
            // With the reason, not just the errno. The app already prints
            // last_refusal; the CLI printed the number alone, so the one place
            // this is easiest to debug was the one place that said least --
            // "-2" where the app would have said which descriptor and path.
            struct checkpoint_status rst;
            checkpoint_get_status(&rst);
            fprintf(stderr, "ISH_RESTORE %s: %d%s%s\n", restore_path, rerr,
                    rst.last_refusal[0] != '\0' ? ": " : "",
                    rst.last_refusal[0] != '\0' ? rst.last_refusal : "");
            return rerr;
        }
        // The image is consumed by being restored. Leaving it would resume
        // the SAME moment again on the launch after this one, which is a
        // stale session rather than the one the user just had -- and would
        // quietly hide a guest that had since suspended over the top of it.
        if (session != NULL && restore_path == session)
            unlink(session);
        tty_drivers[TTY_CONSOLE_MAJOR] = &real_tty_driver;
        exit_hook = exit_handler;
        return 0;
    }

    char argv_copy[4096];
    int i = optind;
    size_t p = 0;
    while (i < argc) {
        const size_t arg_len = strlen(argv[i]) + 1;
        if (p + arg_len > sizeof(argv_copy))
            return _E2BIG;
        memcpy(&argv_copy[p], argv[i], arg_len);
        p += arg_len;
        i++;
    }
    if (p >= sizeof(argv_copy))
        return _E2BIG;
    argv_copy[p] = '\0';
    if (argv[optind] == NULL)
	    return _ENOENT;

    // ISH_CLI_PTY (see main.c): start the command the way the app starts a
    // session -- as a CHILD of init, on a pseudo-terminal -- rather than as
    // init itself on the console. Same sequence as
    // app/TerminalViewController.m's startSession: become_new_init_child, make
    // the pts, create_stdio on it, do_execve, task_start. init stays behind as
    // a sleeper so the process outlives the session, which is also what the app
    // does. Debug only.
    if (cli_session_tty_open != NULL) {
        struct task *init_task = current;
        // init is not on the person's terminal in the app either; give it a
        // descriptor that is checkpointable and that nothing is reading.
        create_stdio("/dev/null", 1, 3);
        static const char sleeper[] = "/bin/sleep\0" "2000000\0";
        err = do_execve("/bin/sleep", 2, (char *) sleeper,
                        envp == NULL ? "\0" : envp);
        if (err < 0)
            return err;
        intptr_t cerr = become_new_init_child();
        if (cerr < 0) {
            current = init_task;
            return (int) cerr;
        }
        struct tty *session_tty = cli_session_tty_open();
        if (session_tty == NULL || IS_ERR(session_tty)) {
            current = init_task;
            return _EIO;
        }
        char pts_path[64];
        snprintf(pts_path, sizeof(pts_path), "/dev/pts/%d", session_tty->num);
        err = create_stdio(pts_path, TTY_PSEUDO_SLAVE_MAJOR, session_tty->num);
        tty_release(session_tty);
        if (err < 0) {
            current = init_task;
            return err;
        }
        err = do_execve(argv[optind], argc - optind, argv_copy,
                        envp == NULL ? "\0" : envp);
        if (err < 0) {
            current = init_task;
            return err;
        }
        if (task_start(current) < 0) {
            current = init_task;
            return _EAGAIN;
        }
        current = init_task;
        tty_drivers[TTY_CONSOLE_MAJOR] = &real_tty_driver;
        exit_hook = exit_handler;
        return 0;
    }

    err = do_execve(argv[optind], argc - optind, argv_copy, envp == NULL ? "\0" : envp);
    if (err < 0)
        return err;
    tty_drivers[TTY_CONSOLE_MAJOR] = &real_tty_driver;
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
        // When launching an init system, mimic the Linux kernel's console
        // handoff: init's stdio points at /dev/console, which never becomes
        // a controlling terminal, leaving tty1's session free for
        // getty@tty1 to claim (see the /dev/console alias in fs/tty.c).
        // Direct commands (ish -f fs /bin/sh) keep the /dev/tty1 default so
        // the shell auto-acquires the tty and job control works.
        const char *base = strrchr(argv[optind], '/');
        base = base != NULL ? base + 1 : argv[optind];
        const char *tty1_default = "/dev/tty1";
        if (strcmp(base, "init") == 0 && strcmp(console, tty1_default) == 0)
            console = "/dev/console";
        int console_major_exp = TTY_CONSOLE_MAJOR, console_minor_exp = 1;
        if (strcmp(console, "/dev/console") == 0) {
            console_major_exp = TTY_ALTERNATE_MAJOR;
            console_minor_exp = DEV_CONSOLE_MINOR;
        }
        err = create_stdio(console, console_major_exp, console_minor_exp);
        if (err < 0)
            return err;
    } else {
        err = create_piped_stdio();
        if (err < 0)
            return err;
    }
    exit_hook = exit_handler;
    return 0;
}
