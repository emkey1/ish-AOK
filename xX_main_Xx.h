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
