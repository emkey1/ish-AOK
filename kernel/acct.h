// BSD process accounting: acct(2), and the per-process exit record it writes.
//
// The format is acct_v3 (64 bytes, little-endian), which is what a kernel
// built with CONFIG_BSD_PROCESS_ACCT_V3 produces -- Debian's default, and what
// atopacctd reads. Verified field-by-field against Linux 6.12 rather than
// against the header alone: a `sleep 2` there records ac_etime 200, which is
// what pins the units as AHZ (centiseconds) held in an IEEE float, and a
// `false` records ac_exitcode 256, which pins it as the WAIT-encoded status
// rather than the exit code.
#ifndef KERNEL_ACCT_H
#define KERNEL_ACCT_H

#include <stdbool.h>
#include "misc.h"

struct task;
struct rusage_;

// acct(2). A NULL path turns accounting off; anything else opens that file for
// append and turns it on. Needs CAP_SYS_PACCT.
dword_t sys_acct(addr_t path_addr);

// True while a file is open. One relaxed atomic load, so the exit path can ask
// on every process without paying for the lock when accounting is off -- which
// is every system that has never called acct(2).
bool acct_is_on(void);

// One record, opaque here so do_exit can hold it on its stack without this
// file's layout leaking into exit.c.
struct acct_record { char bytes[64]; };

// Split in two because of WHERE each half can run.
//
// acct_collect touches only the dying process and never blocks, so it runs
// inside do_exit's locked region, where the task is certainly still alive. It
// takes no locks of its own: the group is already dead when it is called, so
// no thread of it survives to mutate comm or the controlling terminal.
//
// acct_write does filesystem work and must therefore run with NO task or pid
// lock held -- a filesystem operation under one of those is how AOK deadlocks.
//
// Called ONCE per process, when its last thread goes, not once per thread:
// Linux reaches acct_process() from do_exit only under `if (group_dead)`.
//
// Returns false when accounting is off, which is the usual answer and costs
// one relaxed load.
bool acct_collect(struct task *leader, const struct rusage_ *group_rusage,
                  dword_t status, struct acct_record *out);
void acct_write(const struct acct_record *rec);

#endif
