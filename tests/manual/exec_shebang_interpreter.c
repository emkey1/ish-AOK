// What a #! line names, and what actually runs.
//
//   An interpreter named on a #! line is executed, so it faces every question
//   any other executable faces -- including, in iSH-AOK, whether it is one of
//   the programs compiled into the binary (/AOK/native/*, kernel/native.h).
//   That question was only asked in __do_execve, so it was asked when such a
//   program was exec'd directly and NOT when it was reached as an interpreter.
//
//   The failure was silent, which is what makes it worth a test. The file
//   served at /AOK/native/<name> is a `#!/bin/sh` placeholder, so a script
//   saying `#!/AOK/native/bash` got a shell script as its interpreter, found no
//   loader that would take one, and came back ENOEXEC -- and ENOEXEC is exactly
//   the errno every shell answers by re-running the file under /bin/sh. So
//   `#!/AOK/native/bash` ran under dash: not the program asked for, not the
//   placeholder's diagnostic, and no error anywhere.
//
// The ordinary #! rules are here too, because they are what the fix must not
// break: an interpreter with no argument, one with an argument, and one reached
// through a symlink. Those run everywhere including on a real Linux oracle; the
// native cases are skipped where there is no /AOK.
//
// Measured against x86_64 glibc on Linux 6.12.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define OUT_MAX 4096

// Named on a #! line as this binary's argument, it makes the binary report the
// argv the kernel handed it and stop. See the symlink case below.
#define INTERP_MARKER "--print-argv"

static char base[128];

static void ok(const char *label, const char *got, const char *want) {
    if (strcmp(got, want) == 0) {
        test_logf("  %-52s %s\n", label, got);
        return;
    }
    printf("FAIL %s\n       got: %s\n  expected: %s\n", label, got, want);
    failures_total++;
}

// strerror text for the same errno differs between musl and glibc ("Symbolic
// link loop" vs "Too many levels of symbolic links"), so anything asserting an
// errno compares the number and lets the text follow.
static void ok_prefix(const char *label, const char *got, const char *want) {
    if (strncmp(got, want, strlen(want)) == 0) {
        test_logf("  %-52s %s\n", label, got);
        return;
    }
    printf("FAIL %s\n       got: %s\n  expected prefix: %s\n", label, got, want);
    failures_total++;
}

static void failf_msg(const char *label, const char *why) {
    printf("FAIL %s: %s\n", label, why);
    failures_total++;
}

// Write `text` to `path` and make it executable.
static int write_script(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f == NULL)
        return -1;
    fputs(text, f);
    if (fclose(f) != 0)
        return -1;
    return chmod(path, 0755);
}

// execv `path` with `args` and collect the merged stdout+stderr.
//
// Merged deliberately: when this regresses, the symptom is a diagnostic from
// the WRONG interpreter -- "Syntax error: Bad for loop variable" is dash -- and
// a failure that shows it names its own cause.
//
// execv rather than system(): a shell hides the bug. It catches ENOEXEC and
// re-runs the file under /bin/sh, which IS the silence being tested for. execv
// reports the errno instead, and the child prints it into the same pipe.
static int run_exec(const char *path, char *const args[], char *out, int *status_out) {
    out[0] = '\0';
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;

    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        if (pipefd[1] != STDOUT_FILENO && pipefd[1] != STDERR_FILENO)
            close(pipefd[1]);
        execv(path, args);
        fprintf(stderr, "EXECV-FAILED errno=%d (%s)", errno, strerror(errno));
        fflush(NULL);
        _exit(127);
    }
    close(pipefd[1]);

    size_t n = 0;
    while (n < OUT_MAX - 1) {
        ssize_t r = read(pipefd[0], out + n, OUT_MAX - 1 - n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        n += (size_t) r;
    }
    out[n] = '\0';
    close(pipefd[0]);
    // Trailing newlines only; interior ones are part of what is compared.
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = '\0';

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        continue;
    if (status_out != NULL)
        *status_out = status;
    return 0;
}

// Run a script through its own #! line, with "alpha beta" appended, and compare
// what came back.
static void case_script(const char *label, const char *name, const char *text,
                        const char *want, int with_args) {
    char script[sizeof base + 32];
    snprintf(script, sizeof script, "%s/%s", base, name);
    if (write_script(script, text) != 0) {
        failf_msg(label, strerror(errno));
        return;
    }
    char *const args_with[] = { script, (char *) "alpha", (char *) "beta", NULL };
    char *const args_bare[] = { script, NULL };
    char out[OUT_MAX];
    if (run_exec(script, with_args ? args_with : args_bare, out, NULL) != 0) {
        failf_msg(label, strerror(errno));
        return;
    }
    ok(label, out, want);
}

// A chain of scripts: s0 is `#!/bin/sh`, and each s<i> above it names s<i-1> as
// its interpreter. Executing s<n> therefore takes n+1 interpreter rewrites to
// reach /bin/sh, which is the thing being counted.
//
// s0 reports how many arguments it was given, because each rewrite PREPENDS its
// interpreter and pushes the previous file down the vector: run s<n> with two
// arguments and the shell sees s0 plus s1..s<n> plus those two. Getting the
// count right is the difference between resolving a chain and merely surviving
// one.
static int build_chain(unsigned n) {
    char path[sizeof base + 32];
    snprintf(path, sizeof path, "%s/s0", base);
    if (write_script(path, "#!/bin/sh\necho \"chain $#\"\n") != 0)
        return -1;
    for (unsigned i = 1; i <= n; i++) {
        char text[sizeof base + 32];
        snprintf(text, sizeof text, "#!%s/s%u\n", base, i - 1);
        snprintf(path, sizeof path, "%s/s%u", base, i);
        if (write_script(path, text) != 0)
            return -1;
    }
    return 0;
}

// Run the top of an n-deep chain with two arguments, and hand back what came
// out. `out` gets either s0's line or the child's EXECV-FAILED errno.
static int run_chain(unsigned n, char *out) {
    if (build_chain(n) != 0)
        return -1;
    char top[sizeof base + 32];
    snprintf(top, sizeof top, "%s/s%u", base, n);
    char *const args[] = { top, (char *) "alpha", (char *) "beta", NULL };
    return run_exec(top, args, out, NULL);
}

// Is /AOK/native/<name> a program this build actually carries? The path exists
// on every iSH-AOK root whether or not the program is compiled in -- what is
// served there when it is not is a placeholder that says so and exits 127
// (fs/aok.c). Ask by running it: an absent program cannot answer.
static int native_available(const char *prog) {
    char path[64];
    snprintf(path, sizeof path, "/AOK/native/%s", prog);
    if (access(path, X_OK) != 0)
        return 0;
    char *const args[] = { path, (char *) "-c", (char *) "echo NATIVE-READY", NULL };
    char out[OUT_MAX];
    if (run_exec(path, args, out, NULL) != 0)
        return 0;
    return strcmp(out, "NATIVE-READY") == 0;
}

static void cleanup(void) {
    DIR *d = opendir(base);
    if (d != NULL) {
        struct dirent *e;
        char path[sizeof base + 256];
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            snprintf(path, sizeof path, "%s/%s", base, e->d_name);
            unlink(path);
        }
        closedir(d);
    }
    rmdir(base);
}

int main(int argc, char **argv) {
    // Interpreter mode, for the symlink case: print the argv the kernel built,
    // one field per '|'. Before test_init, which rejects unknown options.
    if (argc >= 2 && strcmp(argv[1], INTERP_MARKER) == 0) {
        for (int i = 0; i < argc; i++)
            printf("%s%s", i == 0 ? "" : "|", argv[i]);
        fflush(NULL);
        return 0;
    }
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    snprintf(base, sizeof base, "/tmp/shebang_interp_%d", (int) getpid());
    if (mkdir(base, 0755) != 0) {
        printf("FAIL could not create %s: %s\n", base, strerror(errno));
        return finish_suite("exec_shebang_interpreter");
    }

    // --- the ordinary #! rules, which the native fix must leave alone ------
    //
    // /bin/echo is the interpreter for these: it prints the argv the kernel
    // built for it, which is the whole of what a #! line is specified to do.
    if (access("/bin/echo", X_OK) == 0) {
        char want[sizeof base + 64];

        // No argument: interpreter, script, then the caller's args.
        snprintf(want, sizeof want, "%s/no-arg.sh alpha beta", base);
        case_script("shebang: interpreter, no argument", "no-arg.sh",
                    "#!/bin/echo\n", want, 1);

        // One argument, which sits between the interpreter and the script.
        // Leading spaces after #! and trailing whitespace are both tolerated by
        // Linux and must stay tolerated here.
        snprintf(want, sizeof want, "MARK %s/with-arg.sh alpha beta", base);
        case_script("shebang: interpreter with an argument", "with-arg.sh",
                    "#!  /bin/echo MARK   \n", want, 1);

    } else {
        test_logf("  (no /bin/echo here -- ordinary #! cases skipped)\n");
    }

    // Through a symlink. This binary is the interpreter rather than /bin/echo,
    // because on a BusyBox root every /bin tool is itself a symlink and busybox
    // dispatches on argv[0] -- a link named anything else is "applet not
    // found", which says the symlink resolved but tells us nothing about argv.
    // Reached this way the test prints the whole argv the kernel built, so
    // argv[0] (the interpreter exactly as written on the #! line) is checked
    // too.
    {
        const char *label = "shebang: interpreter through a symlink";
        char self[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", self, sizeof self - 1);
        if (len <= 0) {
            test_logf("  (no /proc/self/exe -- symlinked-interpreter case skipped)\n");
        } else {
            self[len] = '\0';
            char link[sizeof base + 32];
            snprintf(link, sizeof link, "%s/link-interp", base);
            unlink(link);
            if (symlink(self, link) != 0) {
                failf_msg(label, strerror(errno));
            } else {
                char text[sizeof base + 64];
                snprintf(text, sizeof text, "#!%s %s\n", link, INTERP_MARKER);
                char want[4 * sizeof base];
                snprintf(want, sizeof want, "%s|%s|%s/linked.sh|alpha|beta",
                         link, INTERP_MARKER, base);
                case_script(label, "linked.sh", text, want, 1);
            }
        }
    }

    // The everyday case: /bin/sh, which must still be /bin/sh.
    {
        char want[sizeof base + 64];
        snprintf(want, sizeof want, "plain %s/plain.sh alpha beta", base);
        case_script("shebang: #!/bin/sh still runs under /bin/sh", "plain.sh",
                    "#!/bin/sh\necho \"plain $0 $1 $2\"\n", want, 1);
    }

    // --- a #! interpreter that is itself a #! script -----------------------
    //
    // Linux resolves a chain of these and bounds it: measured on 6.12, five
    // rewrites resolve and the sixth is ELOOP. AOK used to resolve exactly one
    // and answer ENOEXEC for anything deeper -- which no shell reports, because
    // ENOEXEC is the errno they all answer by re-running the file under
    // /bin/sh. See EXEC_MAX_DEPTH in kernel/exec.c.
    {
        char out[OUT_MAX];

        // Two rewrites: the interpreter is a script naming /bin/sh. argv is
        // s0 s1 alpha beta, so s0 sees three arguments.
        if (run_chain(1, out) != 0)
            failf_msg("shebang: an interpreter may be a #! script", strerror(errno));
        else
            ok("shebang: an interpreter may be a #! script", out, "chain 3");

        // Five rewrites: the deepest Linux resolves. s0 sees s1..s4 plus two.
        if (run_chain(4, out) != 0)
            failf_msg("shebang: a chain resolves to the depth Linux allows", strerror(errno));
        else
            ok("shebang: a chain resolves to the depth Linux allows", out, "chain 6");

        // Six: one too many, and the answer is ELOOP rather than a chain
        // resolved halfway or an ENOEXEC nobody sees.
        if (run_chain(5, out) != 0)
            failf_msg("shebang: one rewrite too many is ELOOP", strerror(errno));
        else
            ok_prefix("shebang: one rewrite too many is ELOOP", out,
                      "EXECV-FAILED errno=40 ");

        // An interpreter that does not exist answers ENOENT however deep it is:
        // the file is opened by the handler that named it, and only then does
        // the depth get tested. Ordering the two the other way would report a
        // typo in a #! line as a loop.
        char bad[sizeof base + 32], top[sizeof base + 32], text[sizeof base + 32];
        snprintf(bad, sizeof bad, "%s/bad", base);
        if (write_script(bad, "#!/no/such/interpreter\n") == 0) {
            snprintf(text, sizeof text, "#!%s/bad\n", base);
            snprintf(top, sizeof top, "%s/badtop", base);
            if (write_script(top, text) == 0) {
                char *const args[] = { top, NULL };
                if (run_exec(top, args, out, NULL) == 0)
                    ok_prefix("shebang: a missing interpreter is ENOENT, not ELOOP",
                              out, "EXECV-FAILED errno=2 ");
            }
        }
    }

    // --- native interpreters, the class this test exists for ---------------

    if (access("/AOK/native", F_OK) != 0) {
        test_logf("  (no /AOK/native here -- native interpreter cases skipped)\n");
        cleanup();
        return finish_suite("exec_shebang_interpreter");
    }

    // bash. A C-style for loop is a bash-ism dash rejects outright, so the
    // ENOEXEC-to-dash fallback this test guards against cannot pass it.
    if (native_available("bash")) {
        case_script("shebang: #!/AOK/native/bash dispatches natively",
                    "native-bash.sh",
                    "#!/AOK/native/bash\n"
                    "for ((i=0;i<2;i++)); do echo \"i=$i\"; done\n"
                    "[ -n \"$BASH_VERSION\" ] && echo bash-ok\n",
                    "i=0\ni=1\nbash-ok", 0);

        // ...and through a symlink, which is how a native program is meant to
        // be given an ordinary name (kernel/native.h): dispatch is keyed off
        // the resolved fd, and that has to hold on the #! path too.
        char link[sizeof base + 32];
        snprintf(link, sizeof link, "%s/mybash", base);
        unlink(link);
        if (symlink("/AOK/native/bash", link) != 0) {
            failf_msg("shebang: native bash through a symlink", strerror(errno));
        } else {
            char text[sizeof base + 96];
            snprintf(text, sizeof text,
                     "#!%s\n[ -n \"$BASH_VERSION\" ] && echo linked-bash-ok\n",
                     link);
            case_script("shebang: native bash through a symlink",
                        "linked-bash.sh", text, "linked-bash-ok", 0);
        }
    } else {
        test_logf("  (native bash not in this build -- skipped)\n");
    }

    // The placeholder gets to speak. What is served at /AOK/native/<name> is a
    // `#!/bin/sh` script whose only job is to say that native dispatch did not
    // happen -- which it only ever does when the file has been copied somewhere
    // the dispatcher cannot recognise, or the build lacks the program. Copying
    // it is how that state is reachable from a test.
    //
    // This is the case the chain support above exists for. Reached as a #!
    // interpreter it used to be a script interpreting a script, so it came back
    // ENOEXEC and every shell quietly re-ran the user's script under /bin/sh --
    // the loud diagnostic swallowed by the one path that most needed it.
    {
        const char *label = "shebang: the /AOK/native placeholder still says so";
        char copy[sizeof base + 32];
        snprintf(copy, sizeof copy, "%s/notnative", base);
        FILE *src = fopen("/AOK/native/bash", "r");
        if (src == NULL) {
            test_logf("  (no /AOK/native/bash to copy -- placeholder case skipped)\n");
        } else {
            FILE *dst = fopen(copy, "w");
            int copied = dst != NULL;
            int c;
            while (copied && (c = fgetc(src)) != EOF)
                if (fputc(c, dst) == EOF)
                    copied = 0;
            fclose(src);
            if (dst != NULL && fclose(dst) != 0)
                copied = 0;
            if (!copied || chmod(copy, 0755) != 0) {
                failf_msg(label, strerror(errno));
            } else {
                char text[sizeof base + 64];
                snprintf(text, sizeof text, "#!%s\necho unreachable\n", copy);
                char script[sizeof base + 32];
                snprintf(script, sizeof script, "%s/viaplaceholder.sh", base);
                if (write_script(script, text) != 0) {
                    failf_msg(label, strerror(errno));
                } else {
                    char *const args[] = { script, NULL };
                    char out[OUT_MAX];
                    int status = 0;
                    if (run_exec(script, args, out, &status) != 0) {
                        failf_msg(label, strerror(errno));
                    } else {
                        // Output and status in ONE assertion, deliberately.
                        // The stub exits 127, and so does this test's own child
                        // when execv fails -- so a separate status check would
                        // pass on a kernel that never ran the stub at all.
                        char got[OUT_MAX + 32];
                        snprintf(got, sizeof got, "%s | exit %d", out,
                                 WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                        ok(label, got,
                           "notnative: native dispatch unavailable in this build"
                           " | exit 127");
                    }
                }
            }
        }
    }

    // zsh. `print -r --` is a zsh builtin dash does not have, and ZSH_VERSION
    // is set only by zsh itself.
    if (native_available("zsh")) {
        case_script("shebang: #!/AOK/native/zsh dispatches natively",
                    "native-zsh.sh",
                    "#!/AOK/native/zsh\n"
                    "[[ -n $ZSH_VERSION ]] && print -r -- zsh-ok\n",
                    "zsh-ok", 0);
    } else {
        test_logf("  (native zsh not in this build -- skipped)\n");
    }

    cleanup();
    return finish_suite("exec_shebang_interpreter");
}
