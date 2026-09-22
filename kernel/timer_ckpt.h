#ifndef KERNEL_TIMER_CKPT_H
#define KERNEL_TIMER_CKPT_H

// Timers across a checkpoint: how one is described when the image is written
// and armed again when it is read. POSIX timers (timer_create), the interval
// timers (setitimer, which alarm() is too), a timerfd's timer
// (kernel/anonfd_ckpt.h), and the deadline a timed wait interrupted by the
// freeze carries into the call that re-executes it. kernel/time.c owns all of
// them, so the rules live there; kernel/checkpoint.c carries the records.
//
// Before these existed an image carried no timer but a timerfd's: a restored
// process that had armed one never got its signal, and a program using
// SIGALRM as a timeout waited for ever.
//
// WHAT A TIMER KEEPS is its deadline on the clock Linux counts it on, which is
// what a hibernation preserves. CLOCK_MONOTONIC does not count the time the
// machine was stopped; CLOCK_BOOTTIME and the wall clock do. So a timer on
// MONOTONIC -- and every RELATIVE arming on CLOCK_REALTIME, which Linux keeps
// on MONOTONIC (hrtimer_init), and ITIMER_REAL -- comes due when the continued
// MONOTONIC (guest_clock_resume) reaches its deadline, while one on BOOTTIME,
// or armed with TIMER_ABSTIME on a wall clock, comes due that much sooner: at
// once, if the stop was longer. A CPU-time timer keeps the CPU time it has
// left, which only running consumes. The deadline travels, not the time left,
// so that a relative timer and an absolute one armed for the same instant
// still agree after the restore.

#include <stdbool.h>
#include <stdint.h>
#include "misc.h"

enum timer_ckpt_clock {
    TIMER_CKPT_MONOTONIC = 0,   // the guest's CLOCK_MONOTONIC, which goes on across a restore
    TIMER_CKPT_BOOTTIME = 1,    // the guest's CLOCK_BOOTTIME, which also counts the stop
    TIMER_CKPT_REALTIME = 2,    // the wall clock
    TIMER_CKPT_CPU = 3,         // CPU time: what is left, not an instant
};

struct timer_ckpt {
    uint32_t armed;
    uint32_t clock;             // enum timer_ckpt_clock
    int64_t value_ns;           // the next expiry, on `clock`; CPU time left for TIMER_CKPT_CPU
    int64_t interval_ns;        // kept whether or not it is armed, as getitimer reports it
};

// Which clock a timer or timed wait on guest clock `clock` (a CLOCK_*_ id, or
// a dynamic CPU-clock id) keeps its deadline on; `abstime` is whether it was
// armed with TIMER_ABSTIME.
enum timer_ckpt_clock timer_ckpt_clock_for(uint_t clock, bool abstime);
// A deadline `left_ns` from now, expressed on `kind`'s clock -- and back: how
// long is left now until a value carried on `kind`'s clock. The second may be
// negative, for a deadline the stop outlasted.
int64_t timer_ckpt_carry(enum timer_ckpt_clock kind, int64_t left_ns);
int64_t timer_ckpt_left(enum timer_ckpt_clock kind, int64_t value_ns);

// One of a process's POSIX timers: struct posix_timer, with its timer.
struct posix_timer_ckpt {
    int32_t timer_id;           // its slot, which is the id the guest holds
    uint32_t clock;             // the guest clockid it was created on
    uint32_t real_clockid;      // the host clock under it
    int32_t signal;
    uint64_t sig_value;         // union sigval_, as its bytes
    int32_t thread_pid;         // SIGEV_THREAD_ID's thread, or 0
    uint32_t notify;            // 0 for SIGEV_NONE, which signals nobody
    int32_t cpu_clock_pid;      // CLOCK_THREAD_CPUTIME_ID's thread
    int32_t last_overrun;
    uint32_t abstime;
    uint32_t reserved;
    struct timer_ckpt t;
};

// A process's timers, which travel with the first task of its thread group in
// the image. `n_posix` struct posix_timer_ckpt follow.
struct group_timers_ckpt {
    struct timer_ckpt real;     // ITIMER_REAL, and so alarm()
    struct timer_ckpt virt;     // ITIMER_VIRTUAL: user CPU time left
    struct timer_ckpt prof;     // ITIMER_PROF: user and system CPU time left
    uint32_t n_posix;
    uint32_t reserved;
};

struct tgroup;
// Describe everything `group` has armed, the POSIX timers into `posix`, which
// has room for TIMERS_MAX. The machine must be frozen: the timers' own threads
// are the only thing that may still be moving. Takes group->lock and each
// timer's lock, so call it holding neither, nor pids_lock.
void group_timers_ckpt_describe(struct tgroup *group, struct group_timers_ckpt *out,
                                struct posix_timer_ckpt *posix);
// Build them again on `group`, a restored thread group whose timers are all
// unset, and arm them. A timer that comes due at once signals a task, so every
// task the group has must have its host thread by now. Returns how many could
// not be made.
unsigned group_timers_ckpt_arm(struct tgroup *group, const struct group_timers_ckpt *d,
                               const struct posix_timer_ckpt *posix);

#endif
