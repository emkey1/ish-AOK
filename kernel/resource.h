#ifndef RESOURCE_H
#define RESOURCE_H
#include "kernel/time.h"

typedef qword_t rlim_t_;
typedef dword_t rlim32_t_;
#define RLIM_INFINITY_ ((rlim_t_) -1)

struct rlimit_ {
    rlim_t_ cur;
    rlim_t_ max;
};

struct rlimit32_ {
    rlim32_t_ cur;
    rlim32_t_ max;
};

#define RLIMIT_CPU_ 0
#define RLIMIT_FSIZE_ 1
#define RLIMIT_DATA_ 2
#define RLIMIT_STACK_ 3
#define RLIMIT_CORE_ 4
#define RLIMIT_RSS_ 5
#define RLIMIT_NPROC_ 6
#define RLIMIT_NOFILE_ 7
#define RLIMIT_MEMLOCK_ 8
#define RLIMIT_AS_ 9
#define RLIMIT_LOCKS_ 10
#define RLIMIT_SIGPENDING_ 11
#define RLIMIT_MSGQUEUE_ 12
#define RLIMIT_NICE_ 13
#define RLIMIT_RTPRIO_ 14
#define RLIMIT_RTTIME_ 15
#define RLIMIT_NLIMITS_ 16

dword_t sys_getrlimit32(dword_t resource, addr_t rlim_addr);
dword_t sys_getrlimit64(dword_t resource, addr_t rlim_addr);
dword_t sys_getrlimit64_guest(dword_t resource, guest_addr_t rlim_addr);
dword_t sys_setrlimit32(dword_t resource, addr_t rlim_addr);
dword_t sys_setrlimit64(dword_t resource, addr_t rlim_addr);
dword_t sys_setrlimit64_guest(dword_t resource, guest_addr_t rlim_addr);
dword_t sys_prlimit64(pid_t_ pid, dword_t resource, addr_t new_limit_addr, addr_t old_limit_addr);
dword_t sys_prlimit64_guest(pid_t_ pid, dword_t resource, guest_addr_t new_limit_addr, guest_addr_t old_limit_addr);
dword_t sys_old_getrlimit32(dword_t resource, addr_t rlim_addr);

rlim_t_ rlimit(int resource);

struct rusage_ {
    struct timeval_ utime;
    struct timeval_ stime;
    dword_t maxrss;
    dword_t ixrss;
    dword_t idrss;
    dword_t isrss;
    dword_t minflt;
    dword_t majflt;
    dword_t nswap;
    dword_t inblock;
    dword_t oublock;
    dword_t msgsnd;
    dword_t msgrcv;
    dword_t nsignals;
    dword_t nvcsw;
    dword_t nivcsw;
};

struct amd64_rusage_ {
    struct amd64_timeval_ utime;
    struct amd64_timeval_ stime;
    int64_t maxrss;
    int64_t ixrss;
    int64_t idrss;
    int64_t isrss;
    int64_t minflt;
    int64_t majflt;
    int64_t nswap;
    int64_t inblock;
    int64_t oublock;
    int64_t msgsnd;
    int64_t msgrcv;
    int64_t nsignals;
    int64_t nvcsw;
    int64_t nivcsw;
};

struct tgroup;
struct rusage_ rusage_get_current(void);
struct rusage_ rusage_get_group(void);
struct rusage_ rusage_get_group_of(struct tgroup *group);
// One live thread's usage, for the per-thread CPU clocks (kernel/time.c).
struct rusage_ rusage_get_task(struct task *task);
void rusage_add(struct rusage_ *dst, struct rusage_ *src);
int write_guest_rusage_abi(enum guest_abi abi, guest_addr_t addr, const struct rusage_ *rusage);
#define RUSAGE_SELF_ 0
#define RUSAGE_CHILDREN_ -1
#define RUSAGE_THREAD_ 1
dword_t sys_getrusage(dword_t who, addr_t rusage_addr);
dword_t sys_getrusage_guest(dword_t who, guest_addr_t rusage_addr);

int_t sys_sched_getaffinity(pid_t_ pid, dword_t cpusetsize, addr_t cpuset_addr);
int_t sys_sched_setaffinity(pid_t_ pid, dword_t cpusetsize, addr_t cpuset_addr);
int_t sys_sched_getaffinity_guest(pid_t_ pid, dword_t cpusetsize, guest_addr_t cpuset_addr);
int_t sys_sched_setaffinity_guest(pid_t_ pid, dword_t cpusetsize, guest_addr_t cpuset_addr);
int_t sys_getpriority(int_t which, pid_t_ who);
int_t sys_setpriority(int_t which, pid_t_ who, int_t prio);

int_t sys_sched_getparam(pid_t_ pid, addr_t param_addr);
int_t sys_sched_getparam_guest(pid_t_ pid, guest_addr_t param_addr);
int_t sys_sched_setparam(pid_t_ pid, addr_t param_addr);
int_t sys_sched_setparam_guest(pid_t_ pid, guest_addr_t param_addr);
int_t sys_sched_getscheduler(pid_t_ UNUSED(pid));
int_t sys_sched_setscheduler(pid_t_ UNUSED(pid), int_t policy, addr_t param_addr);
int_t sys_sched_setscheduler_guest(pid_t_ pid, int_t policy, guest_addr_t param_addr);
int_t sys_sched_rr_get_interval(pid_t_ pid, addr_t tp_addr);
int_t sys_sched_rr_get_interval_guest(pid_t_ pid, guest_addr_t tp_addr);
int_t sys_sched_get_priority_max(int_t policy);
int_t sys_sched_get_priority_min(int_t policy);

int_t sys_ioprio_set(int_t UNUSED(which), int_t UNUSED(who), int_t UNUSED(ioprio));
int_t sys_ioprio_get(int_t UNUSED(which), int_t UNUSED(who), int_t UNUSED(ioprio));

#endif
