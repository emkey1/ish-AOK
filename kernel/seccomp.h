#ifndef KERNEL_SECCOMP_H
#define KERNEL_SECCOMP_H

#include <stdbool.h>
#include "misc.h"
#include "kernel/abi.h"

// seccomp(2): a process restricting the system calls it (and everything it
// forks or execs) may make. Strict mode allows read, write, exit and
// sigreturn and kills on anything else; filter mode runs classic BPF
// programs over each call's number, architecture, instruction pointer and
// arguments, and does what the most restrictive of them says.
//
// It used to be a stub that reported success and did nothing, so every
// sandbox built on it -- OpenSSH's pre-authentication child, systemd's
// SystemCallFilter=, container runtimes, browsers -- believed it was confined
// and was not.

#define SECCOMP_MODE_DISABLED_ 0
#define SECCOMP_MODE_STRICT_   1
#define SECCOMP_MODE_FILTER_   2
// A thread a seccomp action is killing: anything it still tries to do on its
// way out is refused. Linux's SECCOMP_MODE_DEAD.
#define SECCOMP_MODE_DEAD_     3

struct task;
struct cpu_state;
struct seccomp_filter;

// Filters are shared: a fork or clone hands the child the parent's chain, an
// exec keeps it, and a new filter points at the one before it. Counted.
void seccomp_filter_retain(struct seccomp_filter *filter);
void seccomp_filter_release(struct seccomp_filter *filter);
// Number of filters in a chain, for /proc/<pid>/status.
unsigned seccomp_filter_count(const struct seccomp_filter *filter);

// What the syscall entry does after asking.
enum seccomp_verdict {
    SECCOMP_RUN,        // go ahead with the call
    SECCOMP_ANSWERED,   // not run; *result is its return value
    SECCOMP_SKIPPED,    // not run, and the registers are already as they
                        // should be left: a SIGSYS is on its way, or the task
                        // is dying
    SECCOMP_TRACED,     // a tracer had a PTRACE_EVENT_SECCOMP stop and may
                        // have rewritten the call: re-read it from the
                        // registers, skip it if its number is now -1, and
                        // ask again with recheck set
};

// The check at syscall entry, after any ptrace entry stop: `nr` and `args`
// are what the guest passed, `ip` the address after its syscall instruction.
// Only for a task whose seccomp_mode is not DISABLED; the caller tests that,
// and exempts nothing else. Does not return for a call that kills.
enum seccomp_verdict seccomp_syscall_enter(enum guest_abi abi, qword_t nr,
        const qword_t args[6], qword_t ip, sqword_t *result, bool recheck);

// seccomp(2) itself and the prctl interface to it.
int_t sys_seccomp_guest(uint_t op, uint_t flags, guest_addr_t uargs);
int_t seccomp_prctl_set(qword_t mode, guest_addr_t filter);
int_t seccomp_prctl_get(void);

// Checkpoint support: a chain as a flat list of programs, oldest first, and
// back. The image records each task's chain; tasks that shared one are
// matched by content on the way back in, which keeps SECCOMP_FILTER_FLAG_TSYNC
// working across a restore.
struct seccomp_ckpt_prog {
    bool log;
    unsigned len;
    // `len` classic BPF instructions, as the guest supplied them.
    const void *insns;
};
unsigned seccomp_ckpt_export(const struct seccomp_filter *filter,
        struct seccomp_ckpt_prog *out, unsigned max);
// NULL on a malformed program. Takes the programs oldest first, and hands
// back one reference to the chain. Equal chains imported during one restore
// come back as the same filters; seccomp_ckpt_import_done() ends the restore.
struct seccomp_filter *seccomp_ckpt_import(const struct seccomp_ckpt_prog *progs,
        unsigned count);
void seccomp_ckpt_import_done(void);

#endif
