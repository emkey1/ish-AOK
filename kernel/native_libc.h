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

#include <dirent.h>
#include <fcntl.h>
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

/* --- remaining host-libc holes; see the block comment in the .c ---------- */
int nlibc_dup(int fd);
int nlibc_dup2(int oldfd, int newfd);
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
#define getpwuid                 nlibc_getpwuid
#define getpwnam                 nlibc_getpwnam
#define getgrgid                 nlibc_getgrgid
#define getgrnam                 nlibc_getgrnam

#endif /* !NATIVE_LIBC_NO_REDIRECT */

#endif /* KERNEL_NATIVE_LIBC_H */
