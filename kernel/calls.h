#ifndef CALLS_H
#define CALLS_H

#include "kernel/task.h"
#include "kernel/errno.h"
#include "fs/fd.h"
#include "fs/dev.h"
#include "kernel/fs.h"
#include "misc.h"

#include "kernel/signal.h"
#include "fs/sock.h"
#include "kernel/time.h"
#include "kernel/resource.h"
#include "kernel/ptrace.h"

void handle_interrupt(int interrupt);
void amd64_trace_track_exec(pid_t_ pid, pid_t_ tgid, const char *file);
bool amd64_trace_is_lineage_tgid(pid_t_ tgid);

int must_check user_read(guest_addr_t addr, void *buf, size_t count);
int must_check user_write(guest_addr_t addr, const void *buf, size_t count);
int must_check user_read_task(struct task *task, guest_addr_t addr, void *buf, size_t count);
int must_check user_read_task_mem(struct task *task, struct mem *mem, guest_addr_t addr, void *buf, size_t count);
int must_check user_write_task_mem(struct task *task, struct mem *mem, guest_addr_t addr, const void *buf, size_t count);
int must_check user_write_task(struct task *task, guest_addr_t addr, const void *buf, size_t count);
int must_check user_write_task_ptrace(struct task *task, guest_addr_t addr, const void *buf, size_t count);
int must_check user_write_task_ptrace_mem(struct task *task, struct mem *mem, guest_addr_t addr, const void *buf, size_t count);
int must_check user_read_string(guest_addr_t addr, char *buf, size_t max);
int must_check user_read_path(guest_addr_t addr, char *buf, size_t max);
int must_check user_write_string(guest_addr_t addr, const char *buf);
#define user_get(addr, var) user_read(addr, &(var), sizeof(var))
#define user_put(addr, var) user_write(addr, &(var), sizeof(var))
#define user_get_task(task, addr, var) user_read_task(task, addr, &(var), sizeof(var))
#define user_put_task(task, addr, var) user_write_task(task, addr, &(var), sizeof(var))

// process lifecycle
dword_t sys_clone(dword_t flags, addr_t stack, addr_t ptid, addr_t tls, addr_t ctid);
dword_t sys_clone_guest(qword_t flags, guest_addr_t stack, guest_addr_t ptid, guest_addr_t tls, guest_addr_t ctid);
dword_t sys_clone3(addr_t uargs_addr, dword_t size);
dword_t sys_clone3_guest(guest_addr_t uargs_addr, dword_t size);
dword_t sys_unshare(dword_t flags);
dword_t sys_fork(void);
dword_t sys_vfork(void);
ssize_t sys_execve(addr_t file, addr_t argv, addr_t envp);
ssize_t sys_execve_guest(guest_addr_t file, guest_addr_t argv, guest_addr_t envp);
ssize_t sys_execveat(fd_t dirfd, addr_t file, addr_t argv, addr_t envp, int_t flags);
ssize_t sys_execveat_guest(fd_t dirfd, guest_addr_t file, guest_addr_t argv, guest_addr_t envp, int_t flags);
int do_execve(const char *file, size_t argc, const char *argv, const char *envp);
dword_t sys_exit(dword_t status);
noreturn void do_exit(struct task *task, int status);
noreturn void do_exit_group(int status);
dword_t sys_exit_group(dword_t status);
dword_t sys_wait4(pid_t_ pid, addr_t status_addr, dword_t options, addr_t rusage_addr);
dword_t sys_wait4_guest(pid_t_ pid, guest_addr_t status_addr, dword_t options, guest_addr_t rusage_addr);
int wait_child_status(pid_t_ pid, int *status_out, bool nonblock);
dword_t sys_waitid(int_t idtype, pid_t_ id, addr_t info_addr, int_t options);
dword_t sys_waitid_guest(int_t idtype, pid_t_ id, guest_addr_t info_addr, int_t options);
dword_t sys_waitpid(pid_t_ pid, addr_t status_addr, dword_t options);
dword_t sys_process_vm_readv(pid_t_ pid, addr_t local_iov_addr, dword_t liovcnt,
                             addr_t remote_iov_addr, dword_t riovcnt, dword_t flags);
dword_t sys_process_vm_readv_guest(pid_t_ pid, guest_addr_t local_iov_addr, dword_t liovcnt,
                             guest_addr_t remote_iov_addr, dword_t riovcnt, dword_t flags);

// memory management
addr_t sys_brk(addr_t new_brk);
guest_addr_t sys_brk_guest(guest_addr_t new_brk);

#define MMAP_SHARED 0x1
#define MMAP_PRIVATE 0x2
#define MMAP_FIXED 0x10
#define MMAP_ANONYMOUS 0x20
#define MMAP_FIXED_NOREPLACE 0x100000
addr_t sys_mmap(addr_t args_addr);
addr_t sys_mmap_amd64(addr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, dword_t offset);
guest_addr_t sys_mmap_guest(guest_addr_t addr, qword_t len, dword_t prot, dword_t flags, fd_t fd_no, qword_t offset);
addr_t sys_mmap2(addr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, dword_t offset);
int_t sys_munmap(addr_t addr, uint_t len);
int_t sys_munmap_guest(guest_addr_t addr, qword_t len);
int_t sys_mprotect(addr_t addr, uint_t len, int_t prot);
int_t sys_mprotect_guest(guest_addr_t addr, qword_t len, int_t prot);
int_t sys_mremap(addr_t addr, dword_t old_len, dword_t new_len, dword_t flags, addr_t new_addr);
guest_addr_t sys_mremap_guest(guest_addr_t addr, qword_t old_len, qword_t new_len, dword_t flags,
        guest_addr_t new_addr);
dword_t sys_madvise(addr_t addr, dword_t len, dword_t advice);
dword_t sys_madvise_guest(guest_addr_t addr, qword_t len, dword_t advice);
dword_t sys_mincore(addr_t addr, dword_t len, addr_t vec_addr);
dword_t sys_mincore_guest(guest_addr_t addr, qword_t len, guest_addr_t vec_addr);
dword_t sys_mbind(addr_t addr, dword_t len, int_t mode, addr_t nodemask, dword_t maxnode, uint_t flags);
dword_t sys_mbind_guest(guest_addr_t addr, qword_t len, int_t mode, guest_addr_t nodemask, qword_t maxnode, uint_t flags);
long sys_get_mempolicy(int *mode, unsigned long *nodemask, unsigned long maxnode, void *addr, unsigned long flags);
long sys_get_mempolicy_guest(guest_addr_t mode_addr, guest_addr_t nodemask_addr, qword_t maxnode, guest_addr_t addr, qword_t flags);
long sys_set_mempolicy(int mode, const unsigned long *nodemask, unsigned long maxnode);
long sys_set_mempolicy_guest(int mode, guest_addr_t nodemask_addr, qword_t maxnode);

int_t sys_mlock(addr_t addr, dword_t len);
int_t sys_mlock_guest(guest_addr_t addr, qword_t len);
int_t sys_munlock(addr_t addr, dword_t len);
int_t sys_munlock_guest(guest_addr_t addr, qword_t len);
int_t sys_mlockall(dword_t flags);
int_t sys_mlockall_guest(qword_t flags);
int_t sys_munlockall(void);
int_t sys_munlockall_guest(void);
int_t sys_msync(addr_t addr, dword_t len, int_t flags);
int_t sys_msync_guest(guest_addr_t addr, qword_t len, int_t flags);
dword_t sys_membarrier(dword_t cmd, dword_t flags, dword_t cpuid);

// file descriptor things
#define LOCK_SH_ 1
#define LOCK_EX_ 2
#define LOCK_NB_ 4
#define LOCK_UN_ 8
// Legacy 32-bit guest iovec layout still used by socket message marshalling.
// sendmsg/recvmsg need their own ABI-aware conversion later.
struct iovec_ {
    addr_t base;
    uint_t len;
};
struct guest_iovec_ {
    guest_addr_t base;
    size_t len;
};
struct guest_iovec_ *user_read_iovecs_abi(struct task *task, enum guest_abi abi, guest_addr_t iov_addr, dword_t iov_count);
dword_t sys_read(fd_t fd_no, addr_t buf_addr, dword_t size);
dword_t sys_read_guest(fd_t fd_no, guest_addr_t buf_addr, dword_t size);
dword_t sys_readv(fd_t fd_no, addr_t iovec_addr, dword_t iovec_count);
dword_t sys_readv_guest(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count);
dword_t sys_readv_amd64(fd_t fd_no, addr_t iovec_addr, dword_t iovec_count);
dword_t sys_readv_amd64_guest(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count);
dword_t sys_write(fd_t fd_no, addr_t buf_addr, dword_t size);
dword_t sys_write_guest(fd_t fd_no, guest_addr_t buf_addr, dword_t size);
dword_t sys_writev(fd_t fd_no, addr_t iovec_addr, dword_t iovec_count);
dword_t sys_writev_guest(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count);
dword_t sys_writev_amd64(fd_t fd_no, addr_t iovec_addr, dword_t iovec_count);
dword_t sys_writev_amd64_guest(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count);
dword_t sys__llseek(fd_t f, dword_t off_high, dword_t off_low, addr_t res_addr, dword_t whence);
dword_t sys_lseek(fd_t f, dword_t off, dword_t whence);
off_t_ sys_lseek_guest(fd_t f, off_t_ off, dword_t whence);
dword_t sys_lseek_amd64(fd_t f, dword_t off, dword_t whence);
off_t_ sys_lseek_amd64_guest(fd_t f, off_t_ off, dword_t whence);
dword_t sys_pread(fd_t f, addr_t buf_addr, dword_t buf_size, off_t_ off);
dword_t sys_pread_guest(fd_t f, guest_addr_t buf_addr, dword_t buf_size, off_t_ off);
dword_t sys_pwrite(fd_t f, addr_t buf_addr, dword_t size, off_t_ off);
dword_t sys_pwrite_guest(fd_t f, guest_addr_t buf_addr, dword_t size, off_t_ off);
dword_t sys_preadv_guest(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count, off_t_ off);
dword_t sys_pwritev_guest(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count, off_t_ off);
dword_t sys_preadv2_guest(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count, off_t_ off, uint_t flags);
dword_t sys_pwritev2_guest(fd_t fd_no, guest_addr_t iovec_addr, dword_t iovec_count, off_t_ off, uint_t flags);
dword_t sys_ioctl(fd_t f, dword_t cmd, dword_t arg);
dword_t sys_ioctl_guest(fd_t f, dword_t cmd, guest_addr_t arg);
dword_t sys_fcntl(fd_t f, dword_t cmd, dword_t arg);
dword_t sys_fcntl_guest(fd_t f, dword_t cmd, guest_addr_t arg);
dword_t sys_fcntl_amd64(fd_t f, dword_t cmd, dword_t arg);
dword_t sys_fcntl_amd64_guest(fd_t f, dword_t cmd, guest_addr_t arg);
dword_t sys_fcntl32(fd_t fd, dword_t cmd, dword_t arg);
dword_t sys_dup(fd_t fd);
dword_t sys_dup2(fd_t fd, fd_t new_fd);
dword_t sys_dup3(fd_t f, fd_t new_f, int_t flags);
dword_t sys_close_range(dword_t first, dword_t last, dword_t flags);
dword_t sys_close(fd_t fd);
dword_t sys_fsync(fd_t f);
dword_t sys_syncfs(fd_t f);
dword_t sys_flock(fd_t fd, dword_t operation);
int_t sys_pipe(guest_addr_t pipe_addr);
int_t sys_pipe2(guest_addr_t pipe_addr, int_t flags);
struct pollfd_ {
    fd_t fd;
    word_t events;
    word_t revents;
};
dword_t sys_poll(addr_t fds, dword_t nfds, int_t timeout);
dword_t sys_poll_guest(guest_addr_t fds, dword_t nfds, int_t timeout);
dword_t sys_poll_common(guest_addr_t fds, dword_t nfds, const struct timespec *timeout_ts_ptr, int_t timeout_trace);
dword_t sys_select(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, addr_t timeout_addr);
dword_t sys_select_guest(fd_t nfds, guest_addr_t readfds_addr, guest_addr_t writefds_addr, guest_addr_t exceptfds_addr, guest_addr_t timeout_addr);
dword_t sys_select_amd64(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, addr_t timeout_addr);
dword_t sys_select_amd64_guest(fd_t nfds, guest_addr_t readfds_addr, guest_addr_t writefds_addr, guest_addr_t exceptfds_addr, guest_addr_t timeout_addr);
dword_t sys_pselect(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, addr_t timeout_addr, addr_t sigmask_addr);
dword_t sys_pselect_guest(fd_t nfds, guest_addr_t readfds_addr, guest_addr_t writefds_addr, guest_addr_t exceptfds_addr, guest_addr_t timeout_addr, guest_addr_t sigmask_addr);
dword_t sys_pselect_amd64(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, addr_t timeout_addr, addr_t sigmask_addr);
dword_t sys_pselect_amd64_guest(fd_t nfds, guest_addr_t readfds_addr, guest_addr_t writefds_addr, guest_addr_t exceptfds_addr, guest_addr_t timeout_addr, guest_addr_t sigmask_addr);
dword_t sys_pselect_time64(fd_t nfds, addr_t readfds_addr, addr_t writefds_addr, addr_t exceptfds_addr, addr_t timeout_addr, addr_t sigmask_addr);
dword_t sys_ppoll(addr_t fds, dword_t nfds, addr_t timeout_addr, addr_t sigmask_addr, dword_t sigsetsize);
dword_t sys_ppoll_guest(guest_addr_t fds, dword_t nfds, guest_addr_t timeout_addr, guest_addr_t sigmask_addr, dword_t sigsetsize);
dword_t sys_ppoll_amd64(addr_t fds, dword_t nfds, addr_t timeout_addr, addr_t sigmask_addr, dword_t sigsetsize);
dword_t sys_ppoll_amd64_guest(guest_addr_t fds, dword_t nfds, guest_addr_t timeout_addr, guest_addr_t sigmask_addr, dword_t sigsetsize);
fd_t sys_epoll_create(int_t flags);
fd_t sys_epoll_create0(void);
int_t sys_epoll_ctl(fd_t epoll, int_t op, fd_t fd, addr_t event_addr);
int_t sys_epoll_ctl_guest(fd_t epoll, int_t op, fd_t fd, guest_addr_t event_addr);
int_t sys_epoll_wait(fd_t epoll, addr_t events_addr, int_t max_events, int_t timeout);
int_t sys_epoll_wait_guest(fd_t epoll, guest_addr_t events_addr, int_t max_events, int_t timeout);
int_t sys_epoll_pwait(fd_t epoll_f, addr_t events_addr, int_t max_events, int_t timeout, addr_t sigmask_addr, dword_t sigsetsize);
int_t sys_epoll_pwait_guest(fd_t epoll_f, guest_addr_t events_addr, int_t max_events, int_t timeout, guest_addr_t sigmask_addr, dword_t sigsetsize);
int_t sys_epoll_pwait2(fd_t epoll_f, addr_t events_addr, int_t max_events, addr_t timeout_addr, addr_t sigmask_addr, dword_t sigsetsize);
int_t sys_epoll_pwait2_guest(fd_t epoll_f, guest_addr_t events_addr, int_t max_events, guest_addr_t timeout_addr, guest_addr_t sigmask_addr, dword_t sigsetsize);

int_t sys_eventfd2(uint_t initval, int_t flags);
int_t sys_eventfd(uint_t initval);
// pidfd (kernel/pidfd.c). pidfd_create takes its own reference on `task`;
// pidfd_notify_exit must be called with pids_lock already held.
struct fd *pidfd_create(struct task *task);
void pidfd_notify_exit(struct task *task);
int_t sys_pidfd_open(pid_t_ pid, dword_t flags);
int_t sys_pidfd_send_signal(fd_t pidfd, dword_t sig, addr_t info_addr, dword_t flags);
int_t sys_inotify_init(void);
int_t sys_inotify_init1(int_t flags);
int_t sys_inotify_add_watch(fd_t fd, addr_t pathname_addr, uint_t mask);
int_t sys_inotify_add_watch_guest(fd_t fd, guest_addr_t pathname_addr, uint_t mask);
int_t sys_inotify_rm_watch(fd_t fd, int_t wd);
int_t sys_memfd_create(addr_t name_addr, uint_t flags);
int_t sys_memfd_create_guest(guest_addr_t name_addr, uint_t flags);
struct fd;
int_t memfd_add_seals(struct fd *fd, uint_t arg);
int_t memfd_get_seals(struct fd *fd);

// file management
fd_t sys_open(addr_t path_addr, dword_t flags, mode_t_ mode);
fd_t sys_open_guest(guest_addr_t path_addr, dword_t flags, mode_t_ mode);
fd_t sys_creat(addr_t path_addr, mode_t_ mode);
fd_t sys_creat_guest(guest_addr_t path_addr, mode_t_ mode);
fd_t sys_openat(fd_t at, addr_t path_addr, dword_t flags, mode_t_ mode);
fd_t sys_openat_guest(fd_t at, guest_addr_t path_addr, dword_t flags, mode_t_ mode);
fd_t sys_openat2(fd_t at, addr_t path_addr, addr_t how_addr, dword_t size);
fd_t sys_openat2_guest(fd_t at, guest_addr_t path_addr, guest_addr_t how_addr, dword_t size);
// aarch64 open(2) flags -> internal (i386-valued) flags; defined in calls.c
// (see the comment there for the four relocated O_ constants). Exported for
// sys_openat2_guest's arm64 open_how.flags translation.
dword_t arm64_open_flags_to_internal(qword_t flags);
dword_t sys_close(fd_t fd);
dword_t sys_link(addr_t src_addr, addr_t dst_addr);
dword_t sys_link_guest(guest_addr_t src_addr, guest_addr_t dst_addr);
dword_t sys_linkat(fd_t src_at_f, addr_t src_addr, fd_t dst_at_f, addr_t dst_addr);
dword_t sys_linkat_guest(fd_t src_at_f, guest_addr_t src_addr, fd_t dst_at_f, guest_addr_t dst_addr);
dword_t sys_unlink(addr_t path_addr);
dword_t sys_unlink_guest(guest_addr_t path_addr);
dword_t sys_unlinkat(fd_t at_f, addr_t path_addr, int_t flags);
dword_t sys_unlinkat_guest(fd_t at_f, guest_addr_t path_addr, int_t flags);
dword_t sys_rmdir(addr_t path_addr);
dword_t sys_rmdir_guest(guest_addr_t path_addr);
dword_t sys_rename(addr_t src_addr, addr_t dst_addr);
dword_t sys_rename_guest(guest_addr_t src_addr, guest_addr_t dst_addr);
dword_t sys_renameat(fd_t src_at_f, addr_t src_addr, fd_t dst_at_f, addr_t dst_addr);
dword_t sys_renameat_guest(fd_t src_at_f, guest_addr_t src_addr, fd_t dst_at_f, guest_addr_t dst_addr);
dword_t sys_renameat2(fd_t src_at_f, addr_t src_addr, fd_t dst_at_f, addr_t dst_addr, int_t flags);
dword_t sys_renameat2_guest(fd_t src_at_f, guest_addr_t src_addr, fd_t dst_at_f, guest_addr_t dst_addr, int_t flags);
dword_t sys_symlink(addr_t target_addr, addr_t link_addr);
dword_t sys_symlink_guest(guest_addr_t target_addr, guest_addr_t link_addr);
dword_t sys_symlinkat(addr_t target_addr, fd_t at_f, addr_t link_addr);
dword_t sys_symlinkat_guest(guest_addr_t target_addr, fd_t at_f, guest_addr_t link_addr);
dword_t sys_mknod(addr_t path_addr, mode_t_ mode, dev_t_ dev);
dword_t sys_mknod_guest(guest_addr_t path_addr, mode_t_ mode, dev_t_ dev);
dword_t sys_mknodat(fd_t at_f, addr_t path_addr, mode_t_ mode, dev_t_ dev);
dword_t sys_mknodat_guest(fd_t at_f, guest_addr_t path_addr, mode_t_ mode, dev_t_ dev);
dword_t sys_access(addr_t path_addr, dword_t mode);
dword_t sys_access_guest(guest_addr_t path_addr, dword_t mode);
dword_t sys_faccessat(fd_t at_f, addr_t path, mode_t_ mode, dword_t flags);
dword_t sys_faccessat_guest(fd_t at_f, guest_addr_t path, mode_t_ mode, dword_t flags);
dword_t sys_readlink(addr_t path, addr_t buf, dword_t bufsize);
dword_t sys_readlink_guest(guest_addr_t path, guest_addr_t buf, dword_t bufsize);
dword_t sys_readlinkat(fd_t at_f, addr_t path, addr_t buf, dword_t bufsize);
dword_t sys_readlinkat_guest(fd_t at_f, guest_addr_t path, guest_addr_t buf, dword_t bufsize);
int_t sys_getdents(fd_t f, addr_t dirents, dword_t count);
int_t sys_getdents_guest(fd_t f, guest_addr_t dirents, dword_t count);
int_t sys_getdents_amd64(fd_t f, addr_t dirents, dword_t count);
int_t sys_getdents_amd64_guest(fd_t f, guest_addr_t dirents, dword_t count);
int_t sys_getdents64(fd_t f, addr_t dirents, dword_t count);
int_t sys_getdents64_guest(fd_t f, guest_addr_t dirents, dword_t count);
dword_t sys_stat64(addr_t path_addr, addr_t statbuf_addr);
dword_t sys_lstat64(addr_t path_addr, addr_t statbuf_addr);
dword_t sys_fstat64(fd_t fd_no, addr_t statbuf_addr);
dword_t sys_fstatat64(fd_t at, addr_t path_addr, addr_t statbuf_addr, dword_t flags);
dword_t sys_stat(addr_t path_addr, addr_t statbuf_addr);
dword_t sys_lstat(addr_t path_addr, addr_t statbuf_addr);
dword_t sys_fstat(fd_t fd_no, addr_t statbuf_addr);
dword_t sys_stat_amd64(addr_t path_addr, addr_t statbuf_addr);
dword_t sys_stat_amd64_guest(guest_addr_t path_addr, guest_addr_t statbuf_addr);
dword_t sys_lstat_amd64(addr_t path_addr, addr_t statbuf_addr);
dword_t sys_lstat_amd64_guest(guest_addr_t path_addr, guest_addr_t statbuf_addr);
dword_t sys_fstat_amd64(fd_t fd_no, addr_t statbuf_addr);
dword_t sys_fstat_amd64_guest(fd_t fd_no, guest_addr_t statbuf_addr);
dword_t sys_newfstatat_amd64(fd_t at, addr_t path_addr, addr_t statbuf_addr, dword_t flags);
dword_t sys_newfstatat_amd64_guest(fd_t at, guest_addr_t path_addr, guest_addr_t statbuf_addr, dword_t flags);
dword_t sys_newfstatat_arm64_guest(fd_t at, guest_addr_t path_addr, guest_addr_t statbuf_addr, dword_t flags);
dword_t sys_fstat_arm64_guest(fd_t fd_no, guest_addr_t statbuf_addr);
dword_t sys_statx(fd_t at_f, addr_t path_addr, dword_t flags, dword_t mask, addr_t statxbuf_addr);
dword_t sys_statx_guest(fd_t at_f, guest_addr_t path_addr, dword_t flags, dword_t mask, guest_addr_t statxbuf_addr);
dword_t sys_statx_amd64(fd_t at_f, addr_t path_addr, dword_t flags, dword_t mask, addr_t statxbuf_addr);
dword_t sys_statx_amd64_guest(fd_t at_f, guest_addr_t path_addr, dword_t flags, dword_t mask, guest_addr_t statxbuf_addr);
dword_t sys_fchmod(fd_t f, dword_t mode);
dword_t sys_fchmodat(fd_t at_f, addr_t path_addr, dword_t mode);
dword_t sys_fchmodat_guest(fd_t at_f, guest_addr_t path_addr, dword_t mode);
dword_t sys_fchmodat2(fd_t at_f, addr_t path_addr, dword_t mode, dword_t flags);
dword_t sys_fchmodat2_guest(fd_t at_f, guest_addr_t path_addr, dword_t mode, dword_t flags);
dword_t sys_chmod(addr_t path_addr, dword_t mode);
dword_t sys_chmod_guest(guest_addr_t path_addr, dword_t mode);
dword_t sys_fchown32(fd_t f, dword_t owner, dword_t group);
dword_t sys_fchown_amd64(fd_t f, dword_t owner, dword_t group);
dword_t sys_fchownat(fd_t at_f, addr_t path_addr, dword_t owner, dword_t group, int flags);
dword_t sys_fchownat_guest(fd_t at_f, guest_addr_t path_addr, dword_t owner, dword_t group, int flags);
dword_t sys_chown32(addr_t path_addr, uid_t_ owner, uid_t_ group);
dword_t sys_chown32_guest(guest_addr_t path_addr, uid_t_ owner, uid_t_ group);
dword_t sys_chown_amd64(addr_t path_addr, uid_t_ owner, uid_t_ group);
dword_t sys_chown_amd64_guest(guest_addr_t path_addr, uid_t_ owner, uid_t_ group);
dword_t sys_lchown(addr_t path_addr, uid_t_ owner, uid_t_ group);
dword_t sys_lchown_guest(guest_addr_t path_addr, uid_t_ owner, uid_t_ group);
dword_t sys_lchown_amd64(addr_t path_addr, uid_t_ owner, uid_t_ group);
dword_t sys_lchown_amd64_guest(guest_addr_t path_addr, uid_t_ owner, uid_t_ group);
dword_t sys_truncate64(addr_t path_addr, dword_t size_low, dword_t size_high);
dword_t sys_truncate64_guest(guest_addr_t path_addr, dword_t size_low, dword_t size_high);
dword_t sys_ftruncate64(fd_t f, dword_t size_low, dword_t size_high);
dword_t sys_ftruncate(fd_t f, dword_t size);
dword_t sys_fallocate(fd_t f, dword_t mode, dword_t offset_low, dword_t offset_high, dword_t len_low, dword_t len_high);
dword_t sys_mkdir(addr_t path_addr, mode_t_ mode);
dword_t sys_mkdir_guest(guest_addr_t path_addr, mode_t_ mode);
dword_t sys_mkdirat(fd_t at_f, addr_t path_addr, mode_t_ mode);
dword_t sys_mkdirat_guest(fd_t at_f, guest_addr_t path_addr, mode_t_ mode);
dword_t sys_futimesat(fd_t at_f, addr_t path_addr, addr_t times_addr);
dword_t sys_futimesat_guest(fd_t at_f, guest_addr_t path_addr, guest_addr_t times_addr);
dword_t sys_futimesat_amd64(fd_t at_f, addr_t path_addr, addr_t times_addr);
dword_t sys_futimesat_amd64_guest(fd_t at_f, guest_addr_t path_addr, guest_addr_t times_addr);
dword_t sys_utimensat64(fd_t at_f, addr_t path_addr, addr_t times_addr, dword_t flags);
dword_t sys_utimensat(fd_t at_f, addr_t path_addr, addr_t times_addr, dword_t flags);
dword_t sys_utimensat_guest(fd_t at_f, guest_addr_t path_addr, guest_addr_t times_addr, dword_t flags);
dword_t sys_utimensat_amd64(fd_t at_f, addr_t path_addr, addr_t times_addr, dword_t flags);
dword_t sys_utimensat_amd64_guest(fd_t at_f, guest_addr_t path_addr, guest_addr_t times_addr, dword_t flags);
dword_t sys_utimes(addr_t path_addr, addr_t times_addr);
dword_t sys_utimes_guest(guest_addr_t path_addr, guest_addr_t times_addr);
dword_t sys_utimes_amd64(addr_t path_addr, addr_t times_addr);
dword_t sys_utimes_amd64_guest(guest_addr_t path_addr, guest_addr_t times_addr);
dword_t sys_utime(addr_t path_addr, addr_t times_addr);
dword_t sys_utime_guest(guest_addr_t path_addr, guest_addr_t times_addr);
dword_t sys_utime_amd64(addr_t path_addr, addr_t times_addr);
dword_t sys_utime_amd64_guest(guest_addr_t path_addr, guest_addr_t times_addr);
dword_t sys_times( addr_t tbuf);
dword_t sys_times_guest(guest_addr_t tbuf);
dword_t sys_umask(dword_t mask);

dword_t sys_sendfile(fd_t out_fd, fd_t in_fd, addr_t offset_addr, dword_t count);
dword_t sys_sendfile64(fd_t out_fd, fd_t in_fd, addr_t offset_addr, dword_t count);
dword_t sys_sendfile_guest(fd_t out_fd, fd_t in_fd, guest_addr_t offset_addr, uint64_t count);
dword_t sys_splice(fd_t in_fd, addr_t in_off_addr, fd_t out_fd, addr_t out_off_addr, dword_t count, dword_t flags);
dword_t sys_copy_file_range(fd_t in_fd, addr_t in_off, fd_t out_fd, addr_t out_off, dword_t len, uint_t flags);
dword_t sys_copy_file_range_guest(fd_t in_fd, guest_addr_t in_off, fd_t out_fd, guest_addr_t out_off, uint64_t len, uint_t flags);

dword_t sys_statfs(addr_t path_addr, addr_t buf_addr);
dword_t sys_statfs_amd64(addr_t path_addr, addr_t buf_addr);
dword_t sys_statfs_amd64_guest(guest_addr_t path_addr, guest_addr_t buf_addr);
dword_t sys_statfs64(addr_t path_addr, dword_t buf_size, addr_t buf_addr);
dword_t sys_fstatfs(fd_t f, addr_t buf_addr);
dword_t sys_fstatfs_amd64(fd_t f, addr_t buf_addr);
dword_t sys_fstatfs_amd64_guest(fd_t f, guest_addr_t buf_addr);
dword_t sys_fstatfs64(fd_t f, dword_t buf_size, addr_t buf_addr);

#define MS_READONLY_ (1 << 0)
#define MS_NOSUID_ (1 << 1)
#define MS_NODEV_ (1 << 2)
#define MS_NOEXEC_ (1 << 3)
#define MS_SYNCHRONOUS_ (1 << 4)
#define MS_REMOUNT_ (1 << 5)
#define MS_MANDLOCK_ (1 << 6)
#define MS_DIRSYNC_ (1 << 7)
#define MS_NOSYMFOLLOW_ (1 << 8)
#define MS_NOATIME_ (1 << 10)
#define MS_NODIRATIME_ (1 << 11)
#define MS_BIND_ (1 << 12)
#define MS_MOVE_ (1 << 13)
#define MS_REC_ (1 << 14)
#define MS_SILENT_ (1 << 15)
#define MS_POSIXACL_ (1 << 16)
#define MS_UNBINDABLE_ (1 << 17)
#define MS_PRIVATE_ (1 << 18)
#define MS_SLAVE_ (1 << 19)
#define MS_SHARED_ (1 << 20)
#define MS_RELATIME_ (1 << 21)
#define MS_KERNMOUNT_ (1 << 22)
#define MS_I_VERSION_ (1 << 23)
#define MS_STRICTATIME_ (1 << 24)
#define MS_LAZYTIME_ (1 << 25)
dword_t sys_mount(addr_t source_addr, addr_t target_addr, addr_t type_addr, dword_t flags, addr_t data_addr);
dword_t sys_mount_guest(guest_addr_t source_addr, guest_addr_t target_addr, guest_addr_t type_addr, dword_t flags, guest_addr_t data_addr);
dword_t sys_umount2(addr_t target_addr, dword_t flags);
dword_t sys_umount2_guest(guest_addr_t target_addr, dword_t flags);

dword_t sys_xattr_stub(addr_t path_addr, addr_t name_addr, addr_t value_addr, dword_t size, dword_t flags);
dword_t sys_setxattr_guest(guest_addr_t path_addr, guest_addr_t name_addr, guest_addr_t value_addr, dword_t size, dword_t flags);
dword_t sys_fsetxattr_guest(fd_t fd, guest_addr_t name_addr, guest_addr_t value_addr, dword_t size, dword_t flags);
dword_t sys_getxattr_guest(guest_addr_t path_addr, guest_addr_t name_addr, guest_addr_t value_addr, dword_t size);
dword_t sys_fgetxattr_guest(fd_t fd, guest_addr_t name_addr, guest_addr_t value_addr, dword_t size);
dword_t sys_listxattr_guest(guest_addr_t path_addr, guest_addr_t list_addr, dword_t size);
dword_t sys_flistxattr_guest(fd_t fd, guest_addr_t list_addr, dword_t size);
dword_t sys_removexattr_guest(guest_addr_t path_addr, guest_addr_t name_addr);
dword_t sys_fremovexattr_guest(fd_t fd, guest_addr_t name_addr);

// process information
pid_t_ sys_getpid(void);
pid_t_ sys_gettid(void);
pid_t_ sys_getppid(void);
pid_t_ sys_getpgid(pid_t_ pid);
dword_t sys_setpgid(pid_t_ pid, pid_t_ pgid);
pid_t_ sys_getpgrp(void);
dword_t sys_setpgrp(void);
uid_t_ sys_getuid32(void);
uid_t_ sys_getuid(void);
int_t sys_setuid(uid_t uid);
uid_t_ sys_geteuid32(void);
uid_t_ sys_geteuid(void);
int_t sys_setgid(uid_t gid);
uid_t_ sys_getgid32(void);
uid_t_ sys_getgid(void);
uid_t_ sys_getegid32(void);
uid_t_ sys_getegid(void);
dword_t sys_setresuid(uid_t_ ruid, uid_t_ euid, uid_t_ suid);
dword_t sys_setresgid(uid_t_ rgid, uid_t_ egid, uid_t_ sgid);
int_t sys_setreuid(uid_t_ ruid, uid_t_ euid);
int_t sys_setregid(uid_t_ rgid, uid_t_ egid);
uid_t_ sys_setfsuid(uid_t_ uid);
uid_t_ sys_setfsgid(uid_t_ gid);
int_t sys_getresuid(addr_t ruid_addr, addr_t euid_addr, addr_t suid_addr);
int_t sys_getresgid(addr_t rgid_addr, addr_t egid_addr, addr_t sgid_addr);
int_t sys_getresuid_guest(guest_addr_t ruid_addr, guest_addr_t euid_addr, guest_addr_t suid_addr);
int_t sys_getresgid_guest(guest_addr_t rgid_addr, guest_addr_t egid_addr, guest_addr_t sgid_addr);
int_t sys_getgroups(dword_t size, addr_t list);
int_t sys_setgroups(dword_t size, addr_t list);
int_t sys_getgroups_guest(dword_t size, guest_addr_t list);
int_t sys_setgroups_guest(dword_t size, guest_addr_t list);
int_t sys_capget(addr_t header_addr, addr_t data_addr);
int_t sys_capset(addr_t header_addr, addr_t data_addr);
int_t sys_capget_guest(guest_addr_t header_addr, guest_addr_t data_addr);
int_t sys_capset_guest(guest_addr_t header_addr, guest_addr_t data_addr);
int_t sys_keyctl(dword_t cmd, dword_t arg2, dword_t arg3, dword_t arg4, dword_t arg5);
dword_t sys_getcwd(addr_t buf_addr, dword_t size);
dword_t sys_getcwd_guest(guest_addr_t buf_addr, dword_t size);
dword_t sys_chdir(addr_t path_addr);
dword_t sys_chdir_guest(guest_addr_t path_addr);
dword_t sys_chroot(addr_t path_addr);
dword_t sys_chroot_guest(guest_addr_t path_addr);
dword_t sys_fchdir(fd_t f);
int_t sys_personality(dword_t pers);
int task_set_thread_area(struct task *task, addr_t u_info);
int sys_set_thread_area(addr_t u_info);
int sys_set_tid_address(addr_t blahblahblah);
int sys_set_tid_address_guest(guest_addr_t tid);
dword_t sys_setsid(void);
dword_t sys_getsid(pid_t_ pid);

int_t sys_sched_yield(void);
int_t sys_prctl(dword_t option, uint_t arg2, uint_t arg3, uint_t arg4, uint_t arg5);
int_t sys_prctl_guest(dword_t option, qword_t arg2, qword_t arg3, qword_t arg4, qword_t arg5);
int_t sys_arch_prctl(int_t code, addr_t addr);
int_t sys_arch_prctl_guest(int_t code, guest_addr_t addr);
int_t sys_rseq(addr_t rseq_addr, dword_t rseq_len, dword_t flags, dword_t sig);
int_t sys_rseq_guest(guest_addr_t rseq_addr, dword_t rseq_len, dword_t flags, dword_t sig);
int_t sys_reboot(int_t magic, int_t magic2, int_t cmd);

// system information
#define UNAME_LENGTH 65
struct uname {
    char system[UNAME_LENGTH];   // Linux
    char hostname[UNAME_LENGTH]; // my-compotar
    char release[UNAME_LENGTH];  // 1.2.3-ish
    char version[UNAME_LENGTH];  // SUPER AWESOME
    char arch[UNAME_LENGTH];     // i686
    char domain[UNAME_LENGTH];   // lol
};
void do_uname(struct uname *uts);
dword_t sys_uname(addr_t uts_addr);
dword_t sys_uname_guest(guest_addr_t uts_addr);
dword_t sys_sethostname(addr_t hostname_addr, dword_t hostname_len);
dword_t sys_sethostname_guest(guest_addr_t hostname_addr, dword_t hostname_len);
dword_t sys_setdomainname(addr_t domainname_addr, dword_t domainname_len);
dword_t sys_setdomainname_guest(guest_addr_t domainname_addr, dword_t domainname_len);

struct sys_info {
    dword_t uptime;
    dword_t loads[3];
    dword_t totalram;
    dword_t freeram;
    dword_t sharedram;
    dword_t bufferram;
    dword_t totalswap;
    dword_t freeswap;
    word_t procs;
    dword_t totalhigh;
    dword_t freehigh;
    dword_t mem_unit;
    char pad;
};
// amd64 struct sysinfo: 64-bit (__kernel_ulong_t) fields, natural alignment.
// Do NOT pack — the 4-byte gap after procs/pad (placing totalhigh at offset 88)
// and the trailing pad to 112 bytes are part of the ABI and come for free from
// the compiler's natural alignment of the qword_t members.
struct amd64_sys_info {
    sqword_t uptime;
    qword_t loads[3];
    qword_t totalram;
    qword_t freeram;
    qword_t sharedram;
    qword_t bufferram;
    qword_t totalswap;
    qword_t freeswap;
    word_t procs;
    word_t pad;
    qword_t totalhigh;
    qword_t freehigh;
    dword_t mem_unit;
};
dword_t sys_sysinfo(addr_t info_addr);
dword_t sys_sysinfo_guest(guest_addr_t info_addr);

// futexes
dword_t sys_futex(addr_t uaddr, dword_t op, dword_t val, addr_t timeout_or_val2, addr_t uaddr2, dword_t val3);
dword_t sys_futex_time64(addr_t uaddr, dword_t op, dword_t val, addr_t timeout_or_val2, addr_t uaddr2, dword_t val3);
dword_t sys_futex_guest(guest_addr_t uaddr, dword_t op, dword_t val, guest_addr_t timeout_or_val2, guest_addr_t uaddr2, dword_t val3);
dword_t sys_futex_time64_guest(guest_addr_t uaddr, dword_t op, dword_t val, guest_addr_t timeout_or_val2, guest_addr_t uaddr2, dword_t val3);
int_t sys_set_robust_list(addr_t robust_list, dword_t len);
int_t sys_set_robust_list_guest(guest_addr_t robust_list, dword_t len);
int_t sys_set_robust_list_amd64(addr_t robust_list, dword_t len);
int_t sys_set_robust_list_amd64_guest(guest_addr_t robust_list, dword_t len);
int_t sys_get_robust_list(pid_t_ pid, addr_t robust_list_ptr, addr_t len_ptr);
int_t sys_get_robust_list_guest(pid_t_ pid, guest_addr_t robust_list_ptr, guest_addr_t len_ptr);
int_t sys_get_robust_list_amd64(addr_t pid, addr_t robust_list_ptr, addr_t len_ptr);
int_t sys_get_robust_list_amd64_guest(pid_t_ pid, guest_addr_t robust_list_ptr, guest_addr_t len_ptr);

// misc
dword_t sys_getrandom(addr_t buf_addr, dword_t len, dword_t flags);
dword_t sys_getrandom_guest(guest_addr_t buf_addr, dword_t len, dword_t flags);
size_t sys_syslog(int_t type, addr_t buf_addr, int_t len);
size_t sys_syslog_guest(int_t type, guest_addr_t buf_addr, int_t len);
int_t sys_ipc(uint_t call, int_t first, int_t second, addr_t third, addr_t ptr, int_t fifth);
int_t sys_ipc_guest(uint_t call, int_t first, int_t second, guest_addr_t third, guest_addr_t ptr, int_t fifth);
int_t sys_shmget(dword_t key, dword_t size, dword_t shmflg);
int_t sys_shmget_guest(dword_t key, qword_t size, dword_t shmflg);
addr_t sys_shmat(int_t shmid, addr_t shmaddr, int_t shmflg);
guest_addr_t sys_shmat_guest(int_t shmid, guest_addr_t shmaddr, int_t shmflg);
int_t sys_shmdt(addr_t shmaddr);
int_t sys_shmdt_guest(guest_addr_t shmaddr);
int_t sys_shmctl(int_t shmid, int_t cmd, addr_t buf);
int_t sys_shmctl_guest(int_t shmid, int_t cmd, guest_addr_t buf);

// Syscall dispatch is selected from current->abi. The i386 path is live today;
// amd64 keeps a separate bring-up path because it needs different syscall
// numbers and a different register ABI.
typedef dword_t (*syscall_t)(dword_t, dword_t, dword_t, dword_t, dword_t, dword_t);

#endif
