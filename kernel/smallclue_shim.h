#ifndef KERNEL_SMALLCLUE_SHIM_H
#define KERNEL_SMALLCLUE_SHIM_H

/* Force-included into every SmallCLUE translation unit (see meson.build).
 *
 * SmallCLUE is compiled into iSH-AOK as HOST code, which means its libc calls
 * bind to the host's libc and resolve against the *iOS* filesystem. A plain
 * open("/etc/passwd") in an applet would therefore succeed and return the
 * wrong file -- silent wrongness, the worst available failure mode. Every such
 * call is redirected here onto AOK's own VFS (kernel/native_io.h), which
 * resolves against the calling task's cwd and root exactly as the guest sees
 * them.
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
int sc_open(const char *path, int flags, ...);
int sc_openat(int dirfd, const char *path, int flags, ...);
int sc_close(int fd);
ssize_t sc_read(int fd, void *buf, size_t n);
ssize_t sc_write(int fd, const void *buf, size_t n);
off_t sc_lseek(int fd, off_t off, int whence);
int sc_access(const char *path, int mode);
int sc_stat(const char *path, struct stat *st);
int sc_lstat(const char *path, struct stat *st);
int sc_fstat(int fd, struct stat *st);
ssize_t sc_readlink(const char *path, char *buf, size_t bufsize);
char *sc_realpath(const char *path, char *resolved);

/* --- directories -------------------------------------------------------- */
DIR *sc_opendir(const char *path);
struct dirent *sc_readdir(DIR *dir);
int sc_closedir(DIR *dir);

/* --- mutation ----------------------------------------------------------- */
int sc_unlink(const char *path);
int sc_rmdir(const char *path);
int sc_mkdir(const char *path, mode_t mode);
int sc_rename(const char *from, const char *to);
int sc_symlink(const char *target, const char *linkpath);
int sc_link(const char *from, const char *to);
int sc_chmod(const char *path, mode_t mode);
int sc_chown(const char *path, uid_t uid, gid_t gid);
int sc_lchown(const char *path, uid_t uid, gid_t gid);
int sc_truncate(const char *path, off_t len);

/* --- cwd ---------------------------------------------------------------- */
char *sc_getcwd(char *buf, size_t size);
int sc_chdir(const char *path);

/* --- stdio -------------------------------------------------------------- */
/* fopen returns a real FILE* built with funopen(), whose callbacks go through
 * the VFS. That is what makes the ~1600 fprintf/fputs/fread/fwrite/fclose call
 * sites work untouched: only the handle's origin changes, not the calls on it.
 */
FILE *sc_fopen(const char *path, const char *mode);
FILE *sc_freopen(const char *path, const char *mode, FILE *stream);
FILE *sc_fdopen(int fd, const char *mode);
FILE *sc_stdout(void);
FILE *sc_stderr(void);
FILE *sc_stdin(void);
int sc_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int sc_puts(const char *s);
int sc_putchar(int c);
void sc_perror(const char *s);
/* Flush the wrapped standard streams; see the note in sc_std_stream. */
void sc_flush_std(void);

/* --- remaining host-libc holes; see the block comment in the .c ---------- */
int sc_dup(int fd);
int sc_dup2(int oldfd, int newfd);
int sc_fcntl(int fd, int cmd, ...);
int sc_ioctl(int fd, unsigned long request, ...);
int sc_poll(void *fds, unsigned nfds, int timeout);
int sc_select(int nfds, void *r, void *w, void *e, void *timeout);
int sc_fork(void);
int sc_execl(const char *path, const char *arg0, ...);
int sc_chroot(const char *path);
int sc_kill(pid_t pid, int sig);
int sc_mknod(const char *path, mode_t mode, dev_t dev);
int sc_mkstemp(char *template);
int sc_utimes(const char *path, const struct timeval times[2]);
int sc_futimes(int fd, const struct timeval times[2]);
int sc_statfs(const char *path, void *buf);
int sc_glob(const char *pattern, int flags, void *errfunc, void *pglob);

/* Host-global state: clock, hostname, mount table, power. See the .c. */
struct timespec;
int sc_clock_settime(int clk, const struct timespec *ts);
int sc_sethostname(const char *name, size_t len);
int sc_reboot(int howto);
int sc_mount(const char *src, const char *tgt, const char *type,
             unsigned long flags, const void *data);

/* --- process ------------------------------------------------------------ */
/* There is one host process, so these create real guest tasks instead. */
int sc_execv(const char *path, char *const argv[]);
int sc_execvp(const char *file, char *const argv[]);
int sc_system(const char *command);
FILE *sc_popen(const char *command, const char *mode);
int sc_pclose(FILE *stream);
pid_t sc_waitpid(pid_t pid, int *status, int options);
pid_t sc_wait(int *status);

#ifdef __cplusplus
}
#endif

/* The rewrites. Anything not listed either does not touch the guest's
 * filesystem or is caught by tools/check-native-libc.py.
 *
 * kernel/smallclue_shim.c defines SMALLCLUE_SHIM_NO_REDIRECT before including
 * this: it needs the declarations above but must call the REAL libc, being
 * what the rewrites point at. Without the guard, `fd->ops->lseek` in the
 * implementation becomes `fd->ops->sc_lseek`. */
#ifndef SMALLCLUE_SHIM_NO_REDIRECT
#define open        sc_open
#define openat      sc_openat
#define close       sc_close
#define read        sc_read
#define write       sc_write
#define lseek       sc_lseek
#define access      sc_access
#define stat(a, b)  sc_stat((a), (b))
#define lstat       sc_lstat
#define fstat       sc_fstat
#define readlink    sc_readlink
#define realpath    sc_realpath

#define opendir     sc_opendir
#define readdir     sc_readdir
#define closedir    sc_closedir

#define unlink      sc_unlink
#define rmdir       sc_rmdir
#define mkdir       sc_mkdir
#define rename      sc_rename
#define symlink     sc_symlink
#define link        sc_link
#define chmod       sc_chmod
#define chown       sc_chown
#define lchown      sc_lchown
#define truncate    sc_truncate

#define getcwd      sc_getcwd
#define chdir       sc_chdir

#define fopen       sc_fopen
#define freopen     sc_freopen
#define fdopen      sc_fdopen
#undef  stdout
#undef  stderr
#undef  stdin
#define stdout      sc_stdout()
#define stderr      sc_stderr()
#define stdin       sc_stdin()
#define printf      sc_printf
#define puts        sc_puts
#define putchar     sc_putchar
#define perror      sc_perror

#define execv       sc_execv
#define execvp      sc_execvp
#define system      sc_system
#define popen       sc_popen
#define pclose      sc_pclose
#define waitpid     sc_waitpid
#define wait        sc_wait

#define dup         sc_dup
#define dup2        sc_dup2
#define fcntl       sc_fcntl
#define ioctl       sc_ioctl
#define poll        sc_poll
#define select      sc_select
#define fork        sc_fork
#define execl       sc_execl
#define chroot      sc_chroot
#define kill        sc_kill
#define mknod       sc_mknod
#define mkstemp     sc_mkstemp
#define utimes      sc_utimes
#define futimes     sc_futimes
/* Function-like, as with stat above: `statfs` names both a function and a
 * struct tag, and an object-like macro would rewrite `struct statfs` too. */
#define statfs(a, b) sc_statfs((a), (b))
#define glob        sc_glob

/* Function-like: `mount` is also a struct tag in places, and these should only
 * ever rewrite an actual call. */
#define clock_settime(a, b)      sc_clock_settime((a), (b))
#define sethostname(a, b)        sc_sethostname((a), (b))
#define reboot(a)                sc_reboot((a))
#define mount(a, b, c, d, e)     sc_mount((a), (b), (c), (d), (e))

#endif /* !SMALLCLUE_SHIM_NO_REDIRECT */

#endif /* KERNEL_SMALLCLUE_SHIM_H */
