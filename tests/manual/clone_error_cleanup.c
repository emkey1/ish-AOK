// clone_error_cleanup.c — regression for the clone() error-path session/pgroup
// corruption (issue #423 Tier 2).
//
// A non-CLONE_THREAD clone that fails AFTER copy_task links the new thread-group
// into the caller's session/pgroup lists (e.g. a bad CLONE_PARENT_SETTID pointer)
// used to free that group WITHOUT unlinking it -> the freed list node stayed
// linked in the live, pid-rooted session/pgroup lists -> use-after-free the next
// time those lists were walked (process-group signal, session-exit check).
//
// Repro: pile up many such failed clones (each leaks a dangling freed node into
// our own pgroup/session lists), then walk those lists repeatedly via fork/exit
// and killpg. On a buggy build the walk dereferences freed nodes and crashes; a
// fixed build unlinks the group in the error path and survives.
//
// AOK fails the clone with EFAULT before any child is created; a real Linux
// kernel may instead create the child (then fault) -- the test tolerates both
// and only asserts that the process survives the list walks.
#define _GNU_SOURCE
#include <sched.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include "test_common.h"

#ifndef CLONE_PARENT_SETTID
#define CLONE_PARENT_SETTID 0x00100000
#endif

// An unwritable parent_tid pointer (arg 3 of clone on both i386 and amd64): a
// PROT_NONE page is guaranteed to fault when copy_task's CLONE_PARENT_SETTID
// store runs -- after it has already linked the new group into our session/
// pgroup lists, which is the corruption window under test.
static void *bad_ptid;

// Returns 1 if the clone hit the error path (faulted, no child), 0 otherwise.
static int bad_clone_once(void) {
    errno = 0;
    long rc = syscall(SYS_clone, (long)(CLONE_PARENT_SETTID | SIGCHLD), 0L,
                      (long) bad_ptid, 0L, 0L);
    if (rc == 0)
        _exit(0);              // we are an unexpected child; leave immediately
    if (rc > 0) {
        waitpid((pid_t) rc, NULL, 0);   // real Linux may create+fault; reap it
        return 0;
    }
    return 1;                  // rc < 0: EFAULT error path (AOK)
}

int main(int argc, char **argv) {
    test_init(argc, argv);

    bad_ptid = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bad_ptid == MAP_FAILED) { perror("mmap"); return 2; }
    int is_ish = (access("/proc/ish", F_OK) == 0);

    const int rounds = 8;
    const int per_round = 64;
    int faulted_total = 0;

    for (int r = 0; r < rounds; r++) {
        // Pile dangling freed group nodes into our session/pgroup lists.
        for (int i = 0; i < per_round; i++)
            faulted_total += bad_clone_once();

        // Walk the (possibly corrupted) lists: a child exit checks session/pgroup
        // membership, and killpg walks the process group for the permission check.
        for (int i = 0; i < per_round; i++) {
            pid_t c = fork();
            if (c == 0)
                _exit(0);
            if (c < 0) { printf("FAIL fork: errno=%d\n", errno); failures_total++; break; }
            waitpid(c, NULL, 0);
            killpg(getpgrp(), 0);
        }
        test_logf("ok   round %d survived (%d bad clones + %d list walks)\n",
                  r, per_round, per_round);
    }

    // On AOK the bad clones must reach the post-linking error path; if none did,
    // the trigger has regressed and the test is no longer exercising the bug.
    if (is_ish && faulted_total == 0) {
        printf("FAIL: no clone reached the error path (trigger ineffective)\n");
        failures_total++;
    }
    test_logf("error-path clones: %d\n", faulted_total);

    // Reaching here means none of the list walks faulted on a freed group node.
    return finish_suite("clone_error_cleanup");
}
