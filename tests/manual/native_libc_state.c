/*
 * native_libc_state.c -- native programs each have their own rand() stream.
 *
 * Native programs are threads of one host process, and Darwin's rand() is one
 * stream for the whole process. Native zsh's $RANDOM is rand(), so two native
 * zshes drawing at once took numbers from each other's sequence: `RANDOM=42`
 * followed by draws stopped being reproducible, which zsh's own relaunch
 * replay of $RANDOM (deps/zsh/Src/aok_fork.c) assumes it is. The same audit
 * (nm -u over the native archives, each name checked on the host from two
 * threads) found inet_ntoa's one buffer, the multibyte conversion state behind
 * a NULL mbstate_t and mbtowc/wctomb, and lgamma's signgam; all are per thread
 * now (kernel/native_libc.c). $RANDOM is the one a test can drive.
 *
 * One native zsh draws alone for the reference; then four draw at once, and
 * each must print exactly the reference.
 *
 * Exits 0 and prints "native_libc_state: PASS" on success.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define ZSH "/AOK/native/zsh"
#define DRAWS 3000
#define COPIES 4
#define SCRIPT "RANDOM=42; repeat 3000 print $RANDOM"

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s\n", what);
        failures_total++;
    }
}

static pid_t spawn_to(const char *path) {
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        _exit(121);
    dup2(fd, 1);
    close(fd);
    execl(ZSH, "zsh", "-f", "-c", SCRIPT, (char *) NULL);
    _exit(122);
}

static int wait_ok(pid_t pid) {
    int status;
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return NULL;
    static char buf[COPIES + 1][DRAWS * 8];
    static int next;
    char *out = buf[next++ % (COPIES + 1)];
    *len = fread(out, 1, DRAWS * 8 - 1, f);
    out[*len] = '\0';
    fclose(f);
    return out;
}

static int lines(const char *s) {
    int n = 0;
    for (; *s; s++)
        n += *s == '\n';
    return n;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(180));
    if (access(ZSH, X_OK) != 0) {
        printf("native_libc_state: SKIP (no native zsh)\n");
        return 0;
    }
    char dir[] = "/tmp/native_libc_state.XXXXXX";
    if (mkdtemp(dir) == NULL) {
        check(0, "mkdtemp");
        return finish_suite("native_libc_state");
    }

    char ref_path[96], path[COPIES][96];
    snprintf(ref_path, sizeof(ref_path), "%s/ref", dir);
    check(wait_ok(spawn_to(ref_path)), "the reference run");
    size_t ref_len = 0;
    const char *ref = slurp(ref_path, &ref_len);
    check(ref != NULL && lines(ref) == DRAWS, "the reference has every draw");

    pid_t pids[COPIES];
    for (int i = 0; i < COPIES; i++) {
        snprintf(path[i], sizeof(path[i]), "%s/out%d", dir, i);
        pids[i] = spawn_to(path[i]);
    }
    for (int i = 0; i < COPIES; i++) {
        char what[96];
        snprintf(what, sizeof(what), "copy %d ran", i);
        check(wait_ok(pids[i]), what);
    }
    for (int i = 0; ref != NULL && i < COPIES; i++) {
        size_t len = 0;
        const char *out = slurp(path[i], &len);
        int first_diff = -1;
        if (out != NULL && (len != ref_len || memcmp(out, ref, len) != 0)) {
            int line = 0;
            for (size_t k = 0; k < len && k < ref_len; k++) {
                if (out[k] != ref[k]) {
                    first_diff = line;
                    break;
                }
                line += out[k] == '\n';
            }
            if (first_diff < 0)
                first_diff = line;
        }
        char what[128];
        snprintf(what, sizeof(what), "copy %d drew the RANDOM=42 sequence (first difference at draw %d)",
                 i, first_diff);
        check(out != NULL && first_diff < 0, what);
    }

    unlink(ref_path);
    for (int i = 0; i < COPIES; i++)
        unlink(path[i]);
    rmdir(dir);
    return finish_suite("native_libc_state");
}
