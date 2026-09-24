// stack_rlimit_pushdown.c -- RLIMIT_STACK lowered by someone else applies now.
//
// Linux checks RLIMIT_STACK when the stack grows, so a limit lowered with
// prlimit(2) takes effect at the target's next stack fault. AOK caches the
// bound in the address space (the fault path cannot take the tgroup lock),
// and until build 556 it pushed a new limit there only when the caller was
// changing its OWN limit. A limit lowered by a third party waited for the
// target's next exec, so meanwhile the stack could grow to the old limit.
//
// Two ways to be a third party, both checked:
//   other process  the parent lowers a child's limit with prlimit(child, ...)
//   own tgid       a non-leader thread calls prlimit(getpid(), ...). That pid
//                  names the leader, not the calling thread, so it looked like
//                  "someone else" too, although it is the same address space.
//
// Each child then recurses RECURSE_BYTES, well past the lowered LOW_LIMIT and
// well short of the limit it started with, and must die of SIGSEGV. The
// control child gets no change and must recurse the same distance and exit 0:
// it proves the recursion fits under the starting limit, so a SIGSEGV in the
// other two came from the lowered one.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include "test_common.h"

#define LOW_LIMIT     (1024 * 1024)
#define RECURSE_BYTES (4 * 1024 * 1024)
#define START_LIMIT   (16 * 1024 * 1024)
#define FRAME_BYTES   4096

// Exit codes from a child that survived its recursion.
#define C_SURVIVED     0
#define C_PRLIMIT_FAIL 2
#define C_THREAD_FAIL  3
#define C_PIPE_FAIL    4

static volatile unsigned sink;

__attribute__((noinline)) static unsigned recurse(unsigned depth) {
    volatile unsigned char frame[FRAME_BYTES];
    frame[0] = (unsigned char) depth;
    frame[FRAME_BYTES - 1] = (unsigned char) (depth >> 8);
    if (depth == 0)
        return frame[0];
    // Not a tail call: the frame stays live across the recursion.
    unsigned r = recurse(depth - 1);
    return r + frame[FRAME_BYTES - 1];
}

static void grow_stack(void) {
    sink = recurse(RECURSE_BYTES / FRAME_BYTES);
}

static void *lower_own_limit(void *arg) {
    (void) arg;
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) != 0)
        return (void *) 1;
    rl.rlim_cur = LOW_LIMIT;
    // getpid() is the tgid: the leader, not this thread.
    if (prlimit(getpid(), RLIMIT_STACK, &rl, NULL) != 0)
        return (void *) 1;
    return NULL;
}

enum mode { M_CONTROL, M_OTHER_PROCESS, M_OWN_TGID };

// Returns the child's wait status.
static int run_child(enum mode mode) {
    int ready[2], go[2];
    if (pipe(ready) != 0 || pipe(go) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char c = 0;
        close(ready[0]);
        close(go[1]);
        if (write(ready[1], &c, 1) != 1)
            _exit(C_PIPE_FAIL);
        if (read(go[0], &c, 1) != 1)
            _exit(C_PIPE_FAIL);
        if (mode == M_OWN_TGID) {
            pthread_t t;
            void *res = (void *) 1;
            if (pthread_create(&t, NULL, lower_own_limit, NULL) != 0)
                _exit(C_THREAD_FAIL);
            pthread_join(t, &res);
            if (res != NULL)
                _exit(C_PRLIMIT_FAIL);
        }
        grow_stack();
        _exit(C_SURVIVED);
    }
    close(ready[1]);
    close(go[0]);
    char c;
    if (read(ready[0], &c, 1) != 1)
        c = 0;
    if (mode == M_OTHER_PROCESS) {
        struct rlimit rl;
        if (prlimit(pid, RLIMIT_STACK, NULL, &rl) != 0) {
            printf("FAIL prlimit(child) read: %s\n", strerror(errno));
            failures_total++;
        }
        rl.rlim_cur = LOW_LIMIT;
        if (prlimit(pid, RLIMIT_STACK, &rl, NULL) != 0) {
            printf("FAIL prlimit(child) write: %s\n", strerror(errno));
            failures_total++;
        }
    }
    if (write(go[1], &c, 1) != 1) {
        printf("FAIL go pipe\n");
        failures_total++;
    }
    close(go[1]);
    close(ready[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return status;
}

static void describe(const char *label, int status, int want_segv) {
    if (want_segv) {
        if (WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV) {
            test_logf("%s: SIGSEGV past the lowered limit\n", label);
            return;
        }
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == C_SURVIVED) {
        test_logf("%s: survived %d KiB of recursion\n", label, RECURSE_BYTES / 1024);
        return;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == C_SURVIVED)
        printf("FAIL %s: survived %d KiB of recursion after its limit was lowered to %d KiB\n",
               label, RECURSE_BYTES / 1024, LOW_LIMIT / 1024);
    else if (WIFEXITED(status))
        printf("FAIL %s: exit %d\n", label, WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        printf("FAIL %s: killed by signal %d\n", label, WTERMSIG(status));
    else
        printf("FAIL %s: status %#x\n", label, status);
    failures_total++;
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    // Children inherit this, and it has to comfortably exceed RECURSE_BYTES
    // or the control could not pass. Raising the soft limit needs no
    // privilege, only a hard limit that allows it.
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) != 0) {
        printf("FAIL getrlimit: %s\n", strerror(errno));
        return finish_suite("stack_rlimit_pushdown");
    }
    if (rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < START_LIMIT) {
        if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < START_LIMIT) {
            printf("stack_rlimit_pushdown: SKIP (hard RLIMIT_STACK below %d KiB)\n",
                   START_LIMIT / 1024);
            return 0;
        }
        rl.rlim_cur = START_LIMIT;
        if (setrlimit(RLIMIT_STACK, &rl) != 0) {
            printf("FAIL setrlimit: %s\n", strerror(errno));
            return finish_suite("stack_rlimit_pushdown");
        }
    }

    describe("control", run_child(M_CONTROL), 0);
    describe("other process", run_child(M_OTHER_PROCESS), 1);
    describe("own tgid from a thread", run_child(M_OWN_TGID), 1);
    return finish_suite("stack_rlimit_pushdown");
}
