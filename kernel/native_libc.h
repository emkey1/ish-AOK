#ifndef KERNEL_NATIVE_LIBC_H
#define KERNEL_NATIVE_LIBC_H

/* The libc a natively-compiled program sees. Force-included into every
 * translation unit of such a program (see meson.build); SmallCLUE is the first
 * consumer, exsh is the next.
 *
 * A native program is compiled into iSH-AOK as HOST code, so its libc calls
 * bind to the host's libc and resolve against the *iOS* filesystem. A plain
 * open("/etc/passwd") would therefore succeed and return the wrong file --
 * silent wrongness, the worst available failure mode. Every such call is
 * redirected here onto AOK's own VFS (kernel/native_io.h), which resolves
 * against the calling task's cwd and root exactly as the guest sees them.
 *
 * Adding a second program means force-including this header for its sources
 * too and running tools/check-native-libc.py over the result. Nothing here is
 * SmallCLUE-specific; the SmallCLUE-specific parts live in
 * kernel/smallclue_glue.c.
 *
 * This cannot be done the way PSCAL's iOS app does it. That rewrites a virtual
 * path into a real one and then calls plain libc, which works because its
 * filesystem is a real directory tree in an app sandbox. AOK's guest
 * filesystem is a fakefs -- mangled names plus a meta.db -- so there is no real
 * path for libc to open, and the calls have to go through the VFS itself.
 *
 * The system headers are included FIRST, deliberately: the macros below must
 * not be in effect while the real declarations are parsed, or the prototypes
 * themselves get renamed. Once these headers are in, SmallCLUE's own
 * `#include <fcntl.h>` and friends are no-ops, so the macros only ever rewrite
 * call sites.
 *
 * tools/check-native-libc.py fails the build on anything missed here.
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <grp.h>
#include <pwd.h>
#include <stdnoreturn.h>
#include <termios.h>
#include <sys/utsname.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- paths and descriptors --------------------------------------------- */
int nlibc_open(const char *path, int flags, ...);
int nlibc_openat(int dirfd, const char *path, int flags, ...);
int nlibc_close(int fd);
ssize_t nlibc_read(int fd, void *buf, size_t n);
ssize_t nlibc_write(int fd, const void *buf, size_t n);
off_t nlibc_lseek(int fd, off_t off, int whence);
int nlibc_access(const char *path, int mode);
int nlibc_stat(const char *path, struct stat *st);
int nlibc_lstat(const char *path, struct stat *st);
int nlibc_fstat(int fd, struct stat *st);
ssize_t nlibc_readlink(const char *path, char *buf, size_t bufsize);
char *nlibc_realpath(const char *path, char *resolved);

/* --- directories -------------------------------------------------------- */
DIR *nlibc_opendir(const char *path);
struct dirent *nlibc_readdir(DIR *dir);
int nlibc_closedir(DIR *dir);

/* --- mutation ----------------------------------------------------------- */
int nlibc_unlink(const char *path);
int nlibc_rmdir(const char *path);
int nlibc_mkdir(const char *path, mode_t mode);
int nlibc_rename(const char *from, const char *to);
int nlibc_symlink(const char *target, const char *linkpath);
int nlibc_link(const char *from, const char *to);
int nlibc_chmod(const char *path, mode_t mode);
int nlibc_chown(const char *path, uid_t uid, gid_t gid);
int nlibc_lchown(const char *path, uid_t uid, gid_t gid);
int nlibc_truncate(const char *path, off_t len);

/* --- cwd ---------------------------------------------------------------- */
char *nlibc_getcwd(char *buf, size_t size);
int nlibc_chdir(const char *path);

/* --- stdio -------------------------------------------------------------- */
/* fopen returns a real FILE* built with funopen(), whose callbacks go through
 * the VFS. That is what makes the ~1600 fprintf/fputs/fread/fwrite/fclose call
 * sites work untouched: only the handle's origin changes, not the calls on it.
 */
FILE *nlibc_fopen(const char *path, const char *mode);
FILE *nlibc_freopen(const char *path, const char *mode, FILE *stream);
FILE *nlibc_fdopen(int fd, const char *mode);
FILE *nlibc_stdout(void);
FILE *nlibc_stderr(void);
FILE *nlibc_stdin(void);
int nlibc_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int nlibc_puts(const char *s);
int nlibc_putchar(int c);
void nlibc_perror(const char *s);
/* Flush the wrapped standard streams; see the note in nlibc_std_stream. */
void nlibc_flush_std(void);

/* The host's exit() would end the whole app; a native program is a task. */
noreturn void nlibc_exit(int status);

/* Identity: the GUEST's, not the device's. whoami said "mobile" before this. */
uid_t nlibc_getuid(void);
uid_t nlibc_geteuid(void);
gid_t nlibc_getgid(void);
gid_t nlibc_getegid(void);
pid_t nlibc_getpid(void);
pid_t nlibc_getppid(void);
int nlibc_getgroups(int size, gid_t list[]);
int nlibc_setuid(uid_t uid);
int nlibc_setgid(gid_t gid);
int nlibc_initgroups(const char *user, gid_t group);
struct passwd *nlibc_getpwuid(uid_t uid);
struct passwd *nlibc_getpwnam(const char *name);
struct group *nlibc_getgrgid(gid_t gid);
struct group *nlibc_getgrnam(const char *name);

/* Signals. A handler is host code, so it cannot be given to the kernel to jump
 * to; SIG_DFL/SIG_IGN reach the kernel and a real handler is held here and run
 * at the next syscall. See the block comment in the .c for what that costs. */
int nlibc_sigaction(int sig, const struct sigaction *act, struct sigaction *oact);
void (*nlibc_signal(int sig, void (*handler)(int)))(int);
int nlibc_sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int nlibc_sigpending(sigset_t *set);
int nlibc_sigwait(const sigset_t *set, int *sig);
/* Runs whatever handlers are pending. Called from native_checkpoint. */
void nlibc_deliver_signals(void);

/* Session and process group: plain kernel state, so plain syscalls. */
pid_t nlibc_setsid(void);
int nlibc_setpgid(pid_t pid, pid_t pgid);
pid_t nlibc_getpgid(pid_t pid);
pid_t nlibc_getpgrp(void);
pid_t nlibc_getsid(pid_t pid);
/* A terminal's foreground process group -- two tty ioctls, as on Linux. Left
 * to the host libc these would ask the Mac's terminal about a guest fd. */
pid_t nlibc_tcgetpgrp(int fd);
int nlibc_tcsetpgrp(int fd, pid_t pgrp);

/* Environment: the GUEST's, not the host process's. `env` printed the Mac's
 * before this, and the PATH search for a child walked the Mac's directories. */
char **nlibc_environ(void);
char *nlibc_getenv(const char *name);
int nlibc_setenv(const char *name, const char *value, int overwrite);
int nlibc_unsetenv(const char *name);
int nlibc_putenv(char *entry);

/* --- remaining host-libc holes; see the block comment in the .c ---------- */
int nlibc_dup(int fd);
int nlibc_dup2(int oldfd, int newfd);
int nlibc_pipe(int fds[2]);
int nlibc_fcntl(int fd, int cmd, ...);
int nlibc_ioctl(int fd, unsigned long request, ...);
int nlibc_poll(void *fds, unsigned nfds, int timeout);
int nlibc_select(int nfds, void *r, void *w, void *e, void *timeout);
int nlibc_fork(void);
int nlibc_execl(const char *path, const char *arg0, ...);
int nlibc_chroot(const char *path);
int nlibc_kill(pid_t pid, int sig);
int nlibc_mknod(const char *path, mode_t mode, dev_t dev);
int nlibc_mkstemp(char *template);
int nlibc_utimes(const char *path, const struct timeval times[2]);
int nlibc_futimes(int fd, const struct timeval times[2]);
int nlibc_statfs(const char *path, void *buf);
int nlibc_glob(const char *pattern, int flags, void *errfunc, void *pglob);

/* Host-global state: clock, hostname, mount table, power. See the .c. */
struct timespec;
int nlibc_clock_settime(int clk, const struct timespec *ts);
int nlibc_sethostname(const char *name, size_t len);
int nlibc_reboot(int howto);
int nlibc_mount(const char *src, const char *tgt, const char *type,
             unsigned long flags, const void *data);
struct statfs;
int nlibc_getmntinfo(struct statfs **mntbufp, int flags);
struct utsname;
int nlibc_uname(struct utsname *buf);
int nlibc_sysctl(int *name, unsigned namelen, void *old, size_t *oldlen,
              const void *newp, size_t newlen);
int nlibc_sysctlbyname(const char *name, void *old, size_t *oldlen,
                    const void *newp, size_t newlen);

/* Interruptible waits -- see the block comment in the .c. */
unsigned int nlibc_sleep(unsigned int seconds);
int nlibc_usleep(unsigned int usec);
int nlibc_nanosleep(const struct timespec *req, struct timespec *rem);

/* Terminal. Translated rather than passed through: a native program speaks
 * Darwin's struct termios, AOK's tty speaks Linux's, and the flag values
 * differ. See the .c. */
struct termios;
int nlibc_tcgetattr(int fd, struct termios *out);
int nlibc_tcsetattr(int fd, int action, const struct termios *in);
int nlibc_isatty(int fd);
int nlibc_ioctl_tty(int fd, unsigned long request, void *arg);

/* Networking. AOK's sockets are real host sockets underneath, so what going
 * through the kernel buys is that a socket is a GUEST descriptor -- usable
 * with the same close/poll/select/dup as everything else a native program
 * holds. Every constant and every sockaddr is translated; see the .c. */
int nlibc_socket(int domain, int type, int protocol);
int nlibc_bind(int fd, const void *addr, socklen_t len);
int nlibc_connect(int fd, const void *addr, socklen_t len);
int nlibc_listen(int fd, int backlog);
int nlibc_accept(int fd, void *addr, socklen_t *len);
int nlibc_getsockname(int fd, void *addr, socklen_t *len);
int nlibc_getpeername(int fd, void *addr, socklen_t *len);
ssize_t nlibc_send(int fd, const void *buf, size_t len, int flags);
ssize_t nlibc_sendto(int fd, const void *buf, size_t len, int flags,
        const void *addr, socklen_t addrlen);
ssize_t nlibc_recv(int fd, void *buf, size_t len, int flags);
ssize_t nlibc_recvfrom(int fd, void *buf, size_t len, int flags,
        void *addr, socklen_t *addrlen);
int nlibc_shutdown(int fd, int how);
int nlibc_setsockopt(int fd, int level, int option, const void *value, socklen_t len);
int nlibc_getsockopt(int fd, int level, int option, void *value, socklen_t *len);

/* Resolution, from the guest's files rather than the Mac's. */
int nlibc_getaddrinfo(const char *node, const char *service,
        const struct addrinfo *hints, struct addrinfo **res);
void nlibc_freeaddrinfo(struct addrinfo *res);
const char *nlibc_gai_strerror(int code);
int nlibc_getnameinfo(const void *addr, socklen_t addrlen, char *host, socklen_t hostlen,
        char *serv, socklen_t servlen, int flags);
int nlibc_getifaddrs(struct ifaddrs **ifap);
void nlibc_freeifaddrs(struct ifaddrs *ifa);

int nlibc_tcflush(int fd, int queue);
int nlibc_tcdrain(int fd);

/* The pty. AOK has /dev/ptmx and devpts already (fs/pty.c); this is what makes
 * them reachable from host code, which is what a native shell needs to run a
 * job under a terminal of its own. */
int nlibc_posix_openpt(int flags);
int nlibc_grantpt(int fd);
int nlibc_unlockpt(int fd);
char *nlibc_ptsname(int fd);
char *nlibc_ttyname(int fd);

/* Odds and ends that answered about the host: its /tmp, its CPU count, its
 * page size, its resource usage. */
FILE *nlibc_tmpfile(void);
void nlibc_globfree(void *pglob);
long nlibc_sysconf(int name);
int nlibc_getrusage(int who, void *usage);

/* Threads a native program creates inherit the creating task; see the .c. */
#include <pthread.h>
int nlibc_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                         void *(*fn)(void *), void *arg);

/* --- process ------------------------------------------------------------ */
/* There is one host process, so these create real guest tasks instead. */
int nlibc_execv(const char *path, char *const argv[]);
int nlibc_execvp(const char *file, char *const argv[]);
int nlibc_system(const char *command);
FILE *nlibc_popen(const char *command, const char *mode);
int nlibc_pclose(FILE *stream);
pid_t nlibc_waitpid(pid_t pid, int *status, int options);
pid_t nlibc_wait(int *status);

#ifdef __cplusplus
}
#endif

/* The rewrites. Anything not listed either does not touch the guest's
 * filesystem or is caught by tools/check-native-libc.py.
 *
 * kernel/native_libc.c defines NATIVE_LIBC_NO_REDIRECT before including
 * this: it needs the declarations above but must call the REAL libc, being
 * what the rewrites point at. Without the guard, `fd->ops->lseek` in the
 * implementation becomes `fd->ops->nlibc_lseek`. */
#ifndef NATIVE_LIBC_NO_REDIRECT
#define open        nlibc_open
#define openat      nlibc_openat
#define close       nlibc_close
#define read        nlibc_read
#define write       nlibc_write
#define lseek       nlibc_lseek
#define access      nlibc_access
#define stat(a, b)  nlibc_stat((a), (b))
#define lstat       nlibc_lstat
#define fstat       nlibc_fstat
#define readlink    nlibc_readlink
#define realpath    nlibc_realpath

#define opendir     nlibc_opendir
#define readdir     nlibc_readdir
#define closedir    nlibc_closedir

#define unlink      nlibc_unlink
#define rmdir       nlibc_rmdir
#define mkdir       nlibc_mkdir
#define rename      nlibc_rename
#define symlink     nlibc_symlink
#define link        nlibc_link
#define chmod       nlibc_chmod
#define chown       nlibc_chown
#define lchown      nlibc_lchown
#define truncate    nlibc_truncate

#define getcwd      nlibc_getcwd
#define chdir       nlibc_chdir

#define fopen       nlibc_fopen
#define freopen     nlibc_freopen
#define fdopen      nlibc_fdopen
#undef  stdout
#undef  stderr
#undef  stdin
#define stdout      nlibc_stdout()
#define stderr      nlibc_stderr()
#define stdin       nlibc_stdin()
#define printf      nlibc_printf
#define puts        nlibc_puts
#define putchar     nlibc_putchar
#define perror      nlibc_perror

#define execv       nlibc_execv
#define execvp      nlibc_execvp
#define system      nlibc_system
#define popen       nlibc_popen
#define pclose      nlibc_pclose
#define waitpid     nlibc_waitpid
#define wait        nlibc_wait

#define dup         nlibc_dup
#define dup2        nlibc_dup2
#define pipe        nlibc_pipe
#define fcntl       nlibc_fcntl
#define ioctl       nlibc_ioctl
#define poll        nlibc_poll
#define select      nlibc_select
#define fork        nlibc_fork
#define execl       nlibc_execl
#define chroot      nlibc_chroot
#define kill        nlibc_kill
#define mknod       nlibc_mknod
#define mkstemp     nlibc_mkstemp
#define utimes      nlibc_utimes
#define futimes     nlibc_futimes
/* Function-like, as with stat above: `statfs` names both a function and a
 * struct tag, and an object-like macro would rewrite `struct statfs` too. */
#define statfs(a, b) nlibc_statfs((a), (b))
#define glob        nlibc_glob

/* Function-like: `mount` is also a struct tag in places, and these should only
 * ever rewrite an actual call. */
#define clock_settime(a, b)      nlibc_clock_settime((a), (b))
#define sethostname(a, b)        nlibc_sethostname((a), (b))
#define reboot(a)                nlibc_reboot((a))
#define mount(a, b, c, d, e)     nlibc_mount((a), (b), (c), (d), (e))
#define getmntinfo               nlibc_getmntinfo
#define uname(a)                 nlibc_uname((a))
#define sysctl                   nlibc_sysctl
#define sysctlbyname             nlibc_sysctlbyname
#define sleep                    nlibc_sleep
#define usleep                   nlibc_usleep
#define nanosleep                nlibc_nanosleep
#define tcgetattr                nlibc_tcgetattr
#define tcsetattr                nlibc_tcsetattr
#define isatty                   nlibc_isatty
#define pthread_create           nlibc_pthread_create
#define exit                     nlibc_exit
#define _exit                    nlibc_exit
#define _Exit                    nlibc_exit
#define getuid                   nlibc_getuid
#define geteuid                  nlibc_geteuid
#define getgid                   nlibc_getgid
#define getegid                  nlibc_getegid
#define getpid                   nlibc_getpid
#define getppid                  nlibc_getppid
#define getgroups                nlibc_getgroups
#define setuid                   nlibc_setuid
#define setgid                   nlibc_setgid
#define initgroups               nlibc_initgroups
/* `environ` is a variable, not a call, and it has to stay per-task -- so the
 * rewrite makes it one. SmallCLUE's own `extern char **environ;` becomes a
 * declaration of this function, and every use of `environ` becomes a call to
 * it, which is what keeps two concurrently-running native programs from
 * sharing one environment. */
#define socket                   nlibc_socket
#define bind                     nlibc_bind
#define connect                  nlibc_connect
#define listen                   nlibc_listen
#define accept                   nlibc_accept
#define getsockname              nlibc_getsockname
#define getpeername              nlibc_getpeername
#define send                     nlibc_send
#define sendto                   nlibc_sendto
#define recv                     nlibc_recv
#define recvfrom                 nlibc_recvfrom
#define shutdown                 nlibc_shutdown
#define setsockopt               nlibc_setsockopt
#define getsockopt               nlibc_getsockopt
#define getaddrinfo              nlibc_getaddrinfo
#define freeaddrinfo             nlibc_freeaddrinfo
#define gai_strerror             nlibc_gai_strerror
#define getnameinfo(a,b,c,d,e,f,g) nlibc_getnameinfo((a),(b),(c),(d),(e),(f),(g))
#define getifaddrs               nlibc_getifaddrs
#define freeifaddrs              nlibc_freeifaddrs

#define tcflush                  nlibc_tcflush
#define tcdrain                  nlibc_tcdrain
#define posix_openpt             nlibc_posix_openpt
#define grantpt                  nlibc_grantpt
#define unlockpt                 nlibc_unlockpt
#define ptsname                  nlibc_ptsname
#define ttyname                  nlibc_ttyname
#define tmpfile                  nlibc_tmpfile
#define globfree                 nlibc_globfree
#define sysconf                  nlibc_sysconf
#define getrusage(a, b)          nlibc_getrusage((a), (b))

#define sigaction(a, b, c)       nlibc_sigaction((a), (b), (c))
#define signal                   nlibc_signal
#define sigprocmask              nlibc_sigprocmask
#define sigpending               nlibc_sigpending
#define sigwait                  nlibc_sigwait
#define setsid                   nlibc_setsid
#define setpgid                  nlibc_setpgid
#define getpgid                  nlibc_getpgid
#define getpgrp                  nlibc_getpgrp
#define getsid                   nlibc_getsid
#define tcgetpgrp                nlibc_tcgetpgrp
#define tcsetpgrp                nlibc_tcsetpgrp

#define environ                  nlibc_environ()
#define getenv                   nlibc_getenv
#define setenv                   nlibc_setenv
#define unsetenv                 nlibc_unsetenv
#define putenv                   nlibc_putenv
#define getpwuid                 nlibc_getpwuid
#define getpwnam                 nlibc_getpwnam
#define getgrgid                 nlibc_getgrgid
#define getgrnam                 nlibc_getgrnam

#endif /* !NATIVE_LIBC_NO_REDIRECT */

#endif /* KERNEL_NATIVE_LIBC_H */
