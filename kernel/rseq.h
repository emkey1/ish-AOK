#ifndef KERNEL_RSEQ_H
#define KERNEL_RSEQ_H

#include "misc.h"

struct task;

// The CPU a thread is on, as getcpu(2), its rseq area and /proc/<pid>/stat all
// report it (kernel/rseq.c).
int task_current_cpu(struct task *task);

// The registration's life: a thread sharing its creator's address space starts
// unregistered, a forked process keeps its parent's; an exec and an exit end
// it. Each gives the thread's CPU number back to its address space.
void rseq_fork(struct task *child, bool shares_mm);
void rseq_exec(struct task *task);
void rseq_exit(struct task *task);

// Called as a signal is about to be delivered, before the handler's frame
// saves the interrupted state: a thread inside its registered critical section
// is sent to the section's abort handler. False for a descriptor that is not a
// valid one, which the caller answers with SIGSEGV as Linux does.
bool rseq_signal_deliver(void);

#endif
