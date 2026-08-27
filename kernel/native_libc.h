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
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/uio.h>
#include <dlfcn.h>
#include <sys/un.h>
/* A program that brings its own globbing builds with NATIVE_LIBC_OWN_GLOB and
 * this stays out of its way. bash is the reason: its lib/glob/glob.h uses the
 * include guard _GLOB_H_, which is the SAME guard macOS's <glob.h> uses, so
 * including the system header here silently swallowed bash's own -- leaving
 * glob_filename, glob_pattern_p and the GX_* flags undeclared in three files,
 * with no hint that a header had been skipped.
 *
 * Standing aside is safe rather than a hole: a program in this position calls
 * its own glob, never libc's, and if it ever did call libc's, that would appear
 * as an unresolved `glob` and tools/check-native-libc.py would fail the build
 * on it. */
#ifndef NATIVE_LIBC_OWN_GLOB
#include <glob.h>
#endif
#include <poll.h>
#include <signal.h>
#include <syslog.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
/* Before the `flock(a, b)' macro below: glibc DECLARES flock in <sys/file.h>,
 * and a function-like macro matches that declaration -- rewriting
 * `flock (int __fd, int __operation)' into `nlibc_flock((int __fd), ...)',
 * which gcc reports as "expected declaration specifiers" and clang as
 * "conflicting types for 'nlibc_flock'". Letting the real header land first
 * leaves nothing for the macro to mangle. Darwin declares it elsewhere, which
 * is why this never mattered here. */
#include <sys/file.h>
#include <sys/mount.h>
/* Included for the same reason as sys/mount.h above: the redirect block below
 * rewrites `statvfs`, and a system header that declares it must be seen BEFORE
 * the macro exists -- once its include guard has fired, a later #include is a
 * no-op and its declaration is never macro-expanded into a conflicting one.
 * OpenSSH's sftp.c includes it late and found exactly that conflict. */
#include <sys/statvfs.h>
#if defined(__linux__)
/* struct statfs is declared by <sys/mount.h> on Darwin and by <sys/vfs.h> on
 * glibc, where <sys/mount.h> gives only the mount(2) flags. Without this the
 * type is incomplete everywhere nlibc_statfs touches it. */
#include <sys/vfs.h>
#endif
#include <sys/select.h>
#include <sys/time.h>
/* Not on Linux: glibc dropped <sys/sysctl.h> in 2.32 and musl never had it.
 * Nothing here needs its contents -- nlibc_sysctl and nlibc_sysctlbyname take
 * plain types and only set ENOTSUP, and no CTL_* constant is used anywhere in
 * the tree -- so the include exists to declare the real thing before the
 * `#define sysctl nlibc_sysctl' below renames the callers. Where the header is
 * absent there is no real declaration to precede, and the rename still works. */
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif
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
int nlibc_close_raw(int fd);
ssize_t nlibc_read(int fd, void *buf, size_t n);
ssize_t nlibc_write(int fd, const void *buf, size_t n);
off_t nlibc_lseek(int fd, off_t off, int whence);
int nlibc_access(const char *path, int mode);
int nlibc_stat(const char *path, struct stat *st);
int nlibc_lstat(const char *path, struct stat *st);
int nlibc_fstat(int fd, struct stat *st);
ssize_t nlibc_readlink(const char *path, char *buf, size_t bufsize);
char *nlibc_realpath(const char *path, char *resolved);
int nlibc_fstatat(int dirfd, const char *path, struct stat *st, int flags);
int nlibc_unlinkat(int dirfd, const char *path, int flags);

/* --- directories -------------------------------------------------------- */
DIR *nlibc_opendir(const char *path);
DIR *nlibc_fdopendir(int fd);
int nlibc_readdir_r(DIR *handle, struct dirent *entry, struct dirent **result);
struct dirent *nlibc_readdir(DIR *dir);
int nlibc_closedir(DIR *dir);
/* The descriptor behind a DIR. Ours, necessarily: nlibc_opendir hands back a
 * private struct rather than a host DIR, so the host's dirfd() would read a
 * field out of a layout that is not there. */
int nlibc_dirfd(DIR *dir);

/* --- mutation ----------------------------------------------------------- */
int nlibc_unlink(const char *path);
int nlibc_rmdir(const char *path);
int nlibc_mkdir(const char *path, mode_t mode);
/* The *at forms. Rust's std and rustix prefer them -- they are the ones
 * without a race between resolving a path and acting on it. */
int nlibc_mkdirat(int dirfd, const char *path, mode_t mode);
int nlibc_symlinkat(const char *target, int dirfd, const char *linkpath);
ssize_t nlibc_readlinkat(int dirfd, const char *path, char *buf, size_t bufsize);
int nlibc_fchmodat(int dirfd, const char *path, mode_t mode, int flags);
int nlibc_fchownat(int dirfd, const char *path, uid_t uid, gid_t gid, int flags);
int nlibc_lutimes(const char *path, const struct timeval times[2]);
int nlibc_fstatfs(int fd, void *buf);
int nlibc_statvfs(const char *path, struct statvfs *out);
int nlibc_fstatvfs(int fd, struct statvfs *out);
int nlibc_tcflow(int fd, int action);
int nlibc_tcsendbreak(int fd, int duration);
int nlibc_tcgetsid(int fd);
int nlibc_ttyname_r(int fd, char *buf, size_t buflen);
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
/* Flush without ever waiting on a lock. What a shutdown path wants in place of
 * fflush(NULL), which hangs for ever on a stream whose lock a departed native
 * program's thread still holds. See the .c. */
void nlibc_flush_stream_if_lockable(FILE *stream);
void nlibc_flush_all_streams(void);
/* Just the calling thread's streams -- that is, one native program's. */
void nlibc_flush_thread_streams(void);
/* fflush, except that NULL means "the streams this program owns" rather than
 * "every stream in the process". See the .c. */
int nlibc_fflush(FILE *stream);
/* fclose, plus dropping the stream from the registry fileno() reads. */
int nlibc_fclose(FILE *stream);
/* klogctl(2) against the guest's ring buffer, the one AOK fills. */
int nlibc_klogctl(int type, char *bufp, int len);

/* True when this thread is too close to the end of its stack to recurse
 * again. See the comment on the definition: overrunning it takes the whole
 * app, not one program. */
int nlibc_stack_exhausted(void);
/* The guest fd a stream was built over. NOT the host's fileno, which answers
 * -1 for a funopen stream and sets EBADF -- see the .c. */
int nlibc_fileno(FILE *stream);
/* Falls back to a host locale with the same ENCODING when the guest asks for a
 * name the host does not have -- C.UTF-8 exists on Linux and macOS but not on
 * iOS. See the .c. */
#include <locale.h>
char *nlibc_setlocale(int category, const char *name);

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
/* The EFFECTIVE half of the same family, over the guest's setresuid/setresgid.
 * Not an afterthought: a native "child" is a host THREAD of this app, so a host
 * seteuid that succeeded would move the whole app's credentials. */
int nlibc_seteuid(uid_t uid);
int nlibc_setegid(gid_t gid);
int nlibc_setgroups(int size, const gid_t *list);
int nlibc_initgroups(const char *user, gid_t group);
struct passwd *nlibc_getpwuid(uid_t uid);
struct passwd *nlibc_getpwnam(const char *name);
int nlibc_getpwuid_r(uid_t uid, struct passwd *out, char *buf, size_t buflen,
                     struct passwd **result);
int nlibc_getpwnam_r(const char *name, struct passwd *out, char *buf,
                     size_t buflen, struct passwd **result);
struct group *nlibc_getgrgid(gid_t gid);
struct group *nlibc_getgrnam(const char *name);
int nlibc_getgrouplist(const char *name, int basegid, int *groups, int *ngroups);
/* uid/gid -> name, from the GUEST's /etc/passwd and /etc/group. The pair
 * getpwuid and getgrgid were already routed and these two were not, which is
 * the whole-family failure this round of work keeps finding. */
const char *nlibc_user_from_uid(uid_t uid, int nouser);
const char *nlibc_group_from_gid(gid_t gid, int nogroup);
/* Password hashing. A refusal, not an implementation -- see the .c. */
char *nlibc_crypt(const char *key, const char *salt);

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
// True while the calling thread is inside a host-stdio funopen callback with
// a FILE lock held, meaning native_checkpoint must defer fatal delivery and
// handler delivery rather than risk abandoning the lock. Carries its own
// give-up limit; see the callbacks in native_libc.c.
bool nlibc_stdio_defer_fatal(void);
/* The same, reporting how many handlers ran -- see nlibc_sigsuspend. */
int nlibc_deliver_signals_count(void);

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
/* The environ SLOT. `environ` has to be assignable, not merely readable: bash
 * keeps the C-level environment in step with its export list by writing to it
 * (variables.c), and a call is not an lvalue. */
char ***nlibc_environp(void);
char *nlibc_getenv(const char *name);
int nlibc_setenv(const char *name, const char *value, int overwrite);
int nlibc_unsetenv(const char *name);
int nlibc_putenv(char *entry);

/* --- option parsing ------------------------------------------------------
 * getopt's optind/optarg/opterr/optopt/optreset are ONE copy per process in
 * libSystem, and so is the scanning pointer inside getopt itself. A native
 * program is a function on a task's thread, so two of them parsing argv at
 * once share that copy: six concurrent `smallclue ssh-keygen ... -f /tmp/cN`
 * lost the -f in three of them. These are the per-thread replacements.
 *
 * `struct option` is only forward-declared: <getopt.h> completes it for most
 * consumers and OpenSSH's openbsd-compat/getopt.h for the rest, and including
 * a header here would decide that for them. The layout is the same everywhere
 * this builds, so the pointer is all these need. */
struct option;
int nlibc_getopt(int argc, char *const argv[], const char *optstring);
int nlibc_getopt_long(int argc, char *const argv[], const char *optstring,
        const struct option *longopts, int *longindex);
int nlibc_getopt_long_only(int argc, char *const argv[], const char *optstring,
        const struct option *longopts, int *longindex);
/* The SLOTS, not the values: every one of these has to be assignable -- a
 * program resets optind to restart a parse and clears opterr to silence the
 * error messages -- and a call is not an lvalue. Same shape as environ. */
char **nlibc_optargp(void);
int *nlibc_optindp(void);
int *nlibc_opterrp(void);
int *nlibc_optoptp(void);
int *nlibc_optresetp(void);

/* --- remaining host-libc holes; see the block comment in the .c ---------- */
int nlibc_dup(int fd);
int nlibc_dup2(int oldfd, int newfd);
int nlibc_pipe(int fds[2]);
int nlibc_fcntl(int fd, int cmd, ...);
int nlibc_ioctl(int fd, unsigned long request, ...);
int nlibc_poll(void *fds, unsigned nfds, int timeout);
/* Deliberately NOT redirected from `ppoll`, and not ppoll's signature: POSIX
 * ppoll takes a sigmask this has no use for, and OpenSSH's openbsd-compat
 * ships its own ppoll -- Darwin has none -- which a redirect would rename into
 * a second definition of this. Nothing on an Apple target calls ppoll by name
 * for the same reason, so there is no host to escape to; the gate would say so
 * if that changed. kernel/native_kqueue.c calls it by name. */
struct timespec;
int nlibc_ppoll(void *fds, unsigned nfds, const struct timespec *timeout);
int nlibc_select(int nfds, void *r, void *w, void *e, void *timeout);
int nlibc_fork(void);
int nlibc_execl(const char *path, const char *arg0, ...);
/* execl's PATH-searching twin. execv, execvp, execl and execve were all routed
 * and this one was not -- the single form that BOTH takes varargs and walks
 * PATH, so left alone it resolved an SSH_ASKPASS name against the MAC's PATH
 * and exec'd a device binary over the app. */
int nlibc_execlp(const char *file, const char *arg0, ...);
int nlibc_chroot(const char *path);
int nlibc_kill(pid_t pid, int sig);
/* raise() is kill(getpid()), and the guest's kill ends the TASK where the
 * host's would end the app. Routed rather than tolerated the way abort() is:
 * kill and signal are already here, so there is nothing to trade off. */
int nlibc_raise(int sig);
int nlibc_mknod(const char *path, mode_t mode, dev_t dev);
int nlibc_mkstemp(char *template);
int nlibc_utimes(const char *path, const struct timeval times[2]);
int nlibc_futimes(int fd, const struct timeval times[2]);
int nlibc_statfs(const char *path, void *buf);
#ifndef NATIVE_LIBC_OWN_GLOB
int nlibc_glob(const char *pattern, int flags, void *errfunc, void *pglob);
#endif

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
int nlibc_socketpair(int domain, int type, int protocol, int fds[2]);
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
/* Scatter/gather with ancillary data -- which for OpenSSH means SCM_RIGHTS fd
 * passing over the mux socket. Every fd inside the cmsg is a GUEST descriptor
 * and the cmsghdr layout is the guest's, neither of which the host's sendmsg
 * could have got right. */
ssize_t nlibc_sendmsg(int fd, const struct msghdr *msg, int flags);
ssize_t nlibc_recvmsg(int fd, struct msghdr *msg, int flags);
int nlibc_setsockopt(int fd, int level, int option, const void *value, socklen_t len);
int nlibc_sendfile(int in_fd, int out_fd, off_t offset, off_t *len,
                   void *hdtr, int flags);
int nlibc_getsockopt(int fd, int level, int option, void *value, socklen_t *len);
/* The peer's credentials on a guest AF_UNIX socket, over the guest's
 * SO_PEERCRED (fs/sock.c). The host's getpeereid answers about a host
 * descriptor with the same number, i.e. about the iOS account. */
int nlibc_getpeereid(int fd, uid_t *euid, gid_t *egid);

/* Resolution, from the guest's files rather than the Mac's. */
int nlibc_getaddrinfo(const char *node, const char *service,
        const struct addrinfo *hints, struct addrinfo **res);
void nlibc_freeaddrinfo(struct addrinfo *res);
const char *nlibc_gai_strerror(int code);
int nlibc_getnameinfo(const void *addr, socklen_t addrlen, char *host, socklen_t hostlen,
        char *serv, socklen_t servlen, int flags);
int nlibc_getifaddrs(struct ifaddrs **ifap);
void nlibc_freeifaddrs(struct ifaddrs *ifa);

/* The older resolver interface, over the same guest /etc/hosts, guest
 * /etc/resolv.conf and guest sockets getaddrinfo already uses -- one binary
 * must not carry two resolvers free to disagree.
 *
 * getipnodebyname and freehostent are ONE entry, not two. The caller frees what
 * getipnodebyname returned; routing either alone hands a pointer from one
 * allocator to the other's free path, which is corruption rather than a wrong
 * answer. See the .c. */
struct hostent *nlibc_gethostbyname(const char *name);
struct hostent *nlibc_gethostbyname2(const char *name, int af);
struct hostent *nlibc_gethostbyaddr(const void *addr, socklen_t len, int af);
struct hostent *nlibc_getipnodebyname(const char *name, int af, int flags,
        int *error_num);
struct hostent *nlibc_getipnodebyaddr(const void *addr, size_t len, int af,
        int *error_num);
void nlibc_freehostent(struct hostent *he);
/* /etc/protocols, the neighbour of /etc/services below. */
void nlibc_setprotoent(int stayopen);
void nlibc_endprotoent(void);
struct protoent *nlibc_getprotoent(void);
struct protoent *nlibc_getprotobyname(const char *name);
struct protoent *nlibc_getprotobynumber(int proto);

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
/* The whole dance in one call, which is what OpenSSH's sshpty.c asks for. Left
 * to the host it would allocate a DEVICE pty and hand back host descriptors. */
struct winsize;
int nlibc_openpty(int *amaster, int *aslave, char *name,
        struct termios *termp, struct winsize *winp);

/* --- terminal capabilities -----------------------------------------------
 * The termcap API, answered from the GUEST's terminfo database rather than a
 * host library reading a host database. kernel/native_termcap.c has the long
 * version; the short one is that these six names were missing here, so on a
 * device -- which ships no terminfo at all -- every capability came back
 * empty and ZLE could not move the cursor.
 *
 * NATIVE_LIBC_OWN_TERMCAP is the same escape hatch NATIVE_LIBC_OWN_GLOB is,
 * for a program that DEFINES these names itself -- rewriting them there would
 * rename the definitions and collide with kernel/native_termcap.c. Nothing
 * uses it today. bash did, while it still compiled its bundled GNU termcap
 * (deps/bash/lib/termcap); that copy reads /etc/termcap, which none of the
 * rootfs images ship, so readline resolved nothing and its editor could not
 * move the cursor either. Those two files are out of bash's build now
 * (meson.build) and readline comes through here like zsh does. */
#ifndef NATIVE_LIBC_OWN_TERMCAP
/* The historical termcap signatures, `char *` throughout rather than
 * `const char *`. Not a style choice: zsh's Src/prototypes.h declares these
 * itself for every translation unit that does no terminal handling, and a
 * declaration we cannot edit is the one the rest has to agree with. */
int nlibc_tgetent(char *bp, char *name);
int nlibc_tgetflag(char *id);
int nlibc_tgetnum(char *id);
char *nlibc_tgetstr(char *id, char **area);
char *nlibc_tgoto(char *cap, int col, int row);
int nlibc_tputs(char *str, int affcnt, int (*putc_fn)(int));
#endif

/* Odds and ends that answered about the host: its /tmp, its CPU count, its
 * page size, its resource usage. */
FILE *nlibc_tmpfile(void);
#ifndef NATIVE_LIBC_OWN_GLOB
void nlibc_globfree(void *pglob);
#endif
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
/* wait3/wait4: what a native SHELL reaps its children with. The rusage is
 * zeroed -- AOK has no per-task accounting -- but the reaping is real. */
struct rusage;
pid_t nlibc_wait3(int *status, int options, struct rusage *rusage);
pid_t nlibc_wait4(pid_t pid, int *status, int options, struct rusage *rusage);

/* --- the rest of the kernel surface ------------------------------------- */
/* Added by walking the syscall table rather than by waiting for a program to
 * miss one; see the block comment in the .c. */
int nlibc_fchmod(int fd, mode_t mode);
int nlibc_fchown(int fd, uid_t uid, gid_t gid);
int nlibc_fchdir(int fd);
int nlibc_fsync(int fd);
int nlibc_fdatasync(int fd);
int nlibc_ftruncate(int fd, off_t len);
int nlibc_syncfs(int fd);
/* sync(2) and the extended-attribute family. Both answer with a constant the
 * guest's own syscall table already holds -- sync is a success stub there and
 * every xattr entry is _ENOTSUP -- rather than reaching the DEVICE's buffers
 * and the DEVICE's filesystem. Neither number reaches the generated header;
 * the .c says why for each. */
void nlibc_sync(void);
/* Darwin's xattr calls take a position and an options word; Linux's take
 * neither, and split "do not follow symlinks" into separate l-prefixed calls.
 * Every one of these is an ENOTSUP stub -- see the note above -- so only the
 * SHAPE has to match the platform, or the `#define getxattr nlibc_getxattr'
 * below turns glibc's own declaration into a conflicting one. */
#if defined(__linux__)
ssize_t nlibc_getxattr(const char *path, const char *name, void *value, size_t size);
ssize_t nlibc_fgetxattr(int fd, const char *name, void *value, size_t size);
int nlibc_setxattr(const char *path, const char *name, const void *value,
        size_t size, int flags);
int nlibc_fsetxattr(int fd, const char *name, const void *value,
        size_t size, int flags);
ssize_t nlibc_listxattr(const char *path, char *names, size_t size);
ssize_t nlibc_flistxattr(int fd, char *names, size_t size);
int nlibc_removexattr(const char *path, const char *name);
int nlibc_fremovexattr(int fd, const char *name);
#else
ssize_t nlibc_getxattr(const char *path, const char *name, void *value,
        size_t size, uint32_t position, int options);
ssize_t nlibc_fgetxattr(int fd, const char *name, void *value,
        size_t size, uint32_t position, int options);
int nlibc_setxattr(const char *path, const char *name, const void *value,
        size_t size, uint32_t position, int options);
int nlibc_fsetxattr(int fd, const char *name, const void *value,
        size_t size, uint32_t position, int options);
ssize_t nlibc_listxattr(const char *path, char *names, size_t size, int options);
ssize_t nlibc_flistxattr(int fd, char *names, size_t size, int options);
int nlibc_removexattr(const char *path, const char *name, int options);
int nlibc_fremovexattr(int fd, const char *name, int options);
#endif
/* nice(3): the host's renices the thread the EMULATOR runs on. */
int nlibc_nice(int incr);
pid_t nlibc_gettid(void);
int nlibc_sched_yield(void);
mode_t nlibc_umask(mode_t mask);
int nlibc_setreuid(uid_t r, uid_t e);
int nlibc_setregid(gid_t r, gid_t e);
int nlibc_flock(int fd, int operation);
int nlibc_killpg(pid_t pgrp, int sig);
int nlibc_faccessat(int dirfd, const char *path, int mode, int flags);
struct rlimit;
int nlibc_getrlimit(int resource, struct rlimit *out);
int nlibc_setrlimit(int resource, const struct rlimit *in);
int nlibc_getdtablesize(void);
struct itimerval;
int nlibc_setitimer(int which, const struct itimerval *in, struct itimerval *out);
int nlibc_getitimer(int which, struct itimerval *out);
unsigned int nlibc_alarm(unsigned int seconds);
struct tms;
clock_t nlibc_times(struct tms *out);
struct iovec;
ssize_t nlibc_readv(int fd, const struct iovec *iov, int count);
ssize_t nlibc_writev(int fd, const struct iovec *iov, int count);
ssize_t nlibc_pread(int fd, void *buf, size_t n, off_t off);
ssize_t nlibc_pwrite(int fd, const void *buf, size_t n, off_t off);
int nlibc_gethostname(char *name, size_t len);
int nlibc_getentropy(void *buf, size_t len);
char *nlibc_mkdtemp(char *template);
char *nlibc_mktemp(char *template);
long nlibc_pathconf(const char *path, int name);
long nlibc_fpathconf(int fd, int name);
size_t nlibc_confstr(int name, char *buf, size_t len);
void nlibc_setpwent(void);
void nlibc_endpwent(void);
struct passwd *nlibc_getpwent(void);
/* readline completes service names out of /etc/services; the host's is the
 * Mac's list, not the guest's. */
void nlibc_setservent(int stayopen);
void nlibc_endservent(void);
struct servent *nlibc_getservent(void);
/* The LOOKUPS over that same guest database. ssh calls default_ssh_port() on
 * essentially every run, and it is getservbyname("ssh", "tcp"). */
struct servent *nlibc_getservbyname(const char *name, const char *proto);
struct servent *nlibc_getservbyport(int port, const char *proto);
void nlibc_setgrent(void);
void nlibc_endgrent(void);
struct group *nlibc_getgrent(void);
/* The login database. The host's utmpx is the DEVICE's session list, and with
 * $WATCH set a guest shell printed it by name. The guest's /var/run/utmp is
 * LINUX's 384-byte layout, which Darwin's struct utmpx cannot be pointed at --
 * so the file is parsed by offset in the .c and a struct utmpx is built from
 * it. `struct utmpx` is named rather than defined here: <utmpx.h> stays out of
 * this header (nothing but these six need it) and a pointer to an incomplete
 * type is all a rewrite of a call site requires. */
struct utmpx;
void nlibc_setutxent(void);
void nlibc_endutxent(void);
struct utmpx *nlibc_getutxent(void);
struct utmpx *nlibc_getutxline(const struct utmpx *want);
struct utmpx *nlibc_getutxid(const struct utmpx *want);
struct utmpx *nlibc_pututxline(const struct utmpx *line);
/* Which guest user this is, from that database and then from the guest's
 * /etc/passwd -- not the Mac's login session, which is what $LOGNAME held. */
char *nlibc_getlogin(void);
int nlibc_getlogin_r(char *buf, size_t len);
int nlibc_execve(const char *path, char *const argv[], char *const envp[]);
int nlibc_pselect(int nfds, void *r, void *w, void *e,
        const struct timespec *timeout, const sigset_t *sigmask);
/* sigsuspend, over pselect. See the .c for why an unrouted one hangs a shell
 * that is waiting for a child. */
int nlibc_sigsuspend(const sigset_t *mask);
/* posix_spawn: the general answer to "a native program cannot fork". A program
 * that starts children this way needs no patch at all, which is what retires
 * the per-program spawn seams SmallCLUE and Nextvi both carry. The file-actions
 * and attributes objects are ours -- Darwin declares both as void* -- so the
 * whole family is redirected together or not at all.
 *
 * Spelled `void **` rather than `posix_spawn_file_actions_t *`, and <spawn.h>
 * deliberately NOT included here, because that name does not reliably reach the
 * system header: SmallCLUE ships its own src/spawn.h and its include path comes
 * first. Same collision class as glob.h above, and the same remedy -- do not
 * depend on which header wins. Darwin's typedefs are `void *` for both, which
 * kernel/native_libc.c asserts against the real <spawn.h>, so a caller passing
 * posix_spawn_file_actions_t * is passing exactly this. */

/* The one attribute flag native_spawn_opts acts on, spelled here because
 * <spawn.h> is deliberately not included (above) and a caller that needs the
 * flag would otherwise have to include it and lose to whichever spawn.h wins.
 * The value is Darwin's, which kernel/native_libc.c checks against the system
 * header. pgroup 0 means "a group of its own, led by the child". */
#define NLIBC_SPAWN_SETPGROUP 0x02
#define NLIBC_SPAWN_SETSIGDEF 0x04

int nlibc_posix_spawn_file_actions_init(void **fa);
int nlibc_posix_spawn_file_actions_destroy(void **fa);
int nlibc_posix_spawn_file_actions_adddup2(void **fa, int from, int to);
int nlibc_posix_spawn_file_actions_addclose(void **fa, int fd);
int nlibc_posix_spawn_file_actions_addopen(void **fa, int fd,
        const char *path, int flags, mode_t mode);
int nlibc_posix_spawnattr_init(void **attr);
int nlibc_posix_spawnattr_destroy(void **attr);
int nlibc_posix_spawnattr_setflags(void **attr, short flags);
int nlibc_posix_spawnattr_getflags(void **attr, short *out);
int nlibc_posix_spawnattr_setpgroup(void **attr, pid_t pgroup);
int nlibc_posix_spawnattr_getpgroup(void **attr, pid_t *out);
int nlibc_posix_spawnattr_setsigdefault(void **attr, const sigset_t *set);
int nlibc_posix_spawnattr_setsigmask(void **attr, const sigset_t *set);
int nlibc_posix_spawn(pid_t *pid, const char *path,
        void **fa, void **attr,
        char *const argv[], char *const envp[]);
int nlibc_posix_spawnp(pid_t *pid, const char *file,
        void **fa, void **attr,
        char *const argv[], char *const envp[]);

/* Logging. The device's log is not the guest's: openlog/syslog land in os_log
 * or ASL on iOS and in the Mac's syslog on the CLI, where the guest has its own
 * /dev/log that AOK already serves (fs/sock.c). openlog also sets a
 * PROCESS-WIDE identity on the host, so one guest task running ssh would rename
 * the log identity of the whole app. */
void nlibc_openlog(const char *ident, int option, int facility);
void nlibc_syslog(int priority, const char *fmt, ...)
        __attribute__((format(printf, 2, 3)));
void nlibc_vsyslog(int priority, const char *fmt, va_list ap);
void nlibc_closelog(void);

/* mmap is the one entry here that must give two DIFFERENT answers in the same
 * build, so it is a wrapper rather than a rename -- see the .c. */
void *nlibc_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
int nlibc_munmap(void *addr, size_t len);
/* msync exists to stay COUPLED to that branch: every mapping nlibc_mmap hands
 * back is host anonymous memory with nothing to write back, and the day
 * file-backed MAP_SHARED is allowed it stops being a no-op. */
int nlibc_msync(void *addr, size_t len, int flags);

/* The running executable's path, which on the host is the app's. There is no
 * guest answer available to give, so this reports failure -- see the .c for why
 * that is the right answer rather than a gap, and what a caller falls back to. */
int nlibc_NSGetExecutablePath(char *buf, uint32_t *bufsize);
/* Darwin has no linkable `environ`/`argv` symbols: <crt_externs.h> hands out
 * these accessors, and a runtime built for Apple calls them instead. Same
 * per-task storage as nlibc_environ. */
char ***nlibc_NSGetEnviron(void);
char ***nlibc_NSGetArgv(void);
int *nlibc_NSGetArgc(void);
int nlibc_mkfifo(const char *path, mode_t mode);
int nlibc_linkat(int oldfd, const char *from, int newfd, const char *to, int flags);
/* Darwin's copy fast paths, which std::fs::copy uses in place of read/write. */
int nlibc_fclonefileat(int srcfd, int dstdirfd, const char *dst, int flags);
int nlibc_fcopyfile(int from_fd, int to_fd, void *state, uint32_t flags);
void *nlibc_copyfile_state_alloc(void);
int nlibc_copyfile_state_free(void *state);
int nlibc_copyfile_state_get(void *state, uint32_t flag, void *dst);
int nlibc_setattrlist(const char *path, void *attrs, void *buf, size_t size, unsigned long options);
int nlibc_fsetattrlist(int fd_no, void *attrs, void *buf, size_t size, unsigned long options);

void *nlibc_dlopen(const char *path, int mode);
void *nlibc_dlsym(void *handle, const char *symbol);
int nlibc_dlclose(void *handle);
const char *nlibc_dlerror(void);

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
/* Object-like, unlike stat above: `fstatat` is only a function name, and
 * OpenSSH's openbsd-compat/bsd-misc.h declares it with UNNAMED parameters --
 * `int fstatat(int, const char *, struct stat *, int);` -- which a
 * function-like macro would try to expand as a call and mangle into implicit
 * ints. Prototypes are the reason to prefer the object-like form wherever the
 * name is not also a struct tag. */
#define fstatat     nlibc_fstatat
#define unlinkat    nlibc_unlinkat

#define fdopendir   nlibc_fdopendir
#define readdir_r   nlibc_readdir_r
#define opendir     nlibc_opendir
#define readdir     nlibc_readdir
#define closedir    nlibc_closedir
#define dirfd       nlibc_dirfd

#define unlink      nlibc_unlink
#define rmdir       nlibc_rmdir
#define mkdir       nlibc_mkdir
#define mkdirat     nlibc_mkdirat
#define symlinkat   nlibc_symlinkat
#define readlinkat  nlibc_readlinkat
#define fchmodat    nlibc_fchmodat
#define fchownat    nlibc_fchownat
#define lutimes     nlibc_lutimes
#define fstatfs     nlibc_fstatfs
/* Function-like for the same reason statfs is: `statvfs` is a struct tag too. */
#define statvfs(a, b)  nlibc_statvfs((a), (b))
#define fstatvfs(a, b) nlibc_fstatvfs((a), (b))
#define tcflow      nlibc_tcflow
#define tcsendbreak nlibc_tcsendbreak
#define tcgetsid    nlibc_tcgetsid
#define ttyname_r   nlibc_ttyname_r
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
#define fileno      nlibc_fileno
/* The GUEST's kernel ring buffer. There is no host klogctl on Darwin at all,
 * and on Linux the host's would be the wrong kernel; see the .c. */
#define klogctl     nlibc_klogctl
/* fflush(f) needs no help; fflush(NULL) does -- it is a whole-process
 * operation in a place where a "process" is one thread. See the .c. */
#define fflush      nlibc_fflush
/* fclose reaches nothing on the host, but it does have to drop the stream from
 * the shim's registry -- otherwise the entry outlives the FILE. See the .c. */
#define fclose      nlibc_fclose
#define setlocale   nlibc_setlocale

#define execv       nlibc_execv
#define execvp      nlibc_execvp
#define system      nlibc_system
#define popen       nlibc_popen
#define pclose      nlibc_pclose
#define waitpid     nlibc_waitpid
#define wait        nlibc_wait
#define wait3       nlibc_wait3
#define wait4       nlibc_wait4

#define dup         nlibc_dup
#define dup2        nlibc_dup2
#define pipe        nlibc_pipe
#define fcntl       nlibc_fcntl
#define ioctl       nlibc_ioctl
#define poll        nlibc_poll
/* kqueue and kevent, for a runtime built for Apple. See kernel/native_kqueue.h.
 * kevent is function-like on purpose: `kevent` is a STRUCT tag as well as a
 * function, and an object-like macro would rewrite `struct kevent` too. */
#define kqueue      nlibc_kqueue
#define kevent(a, b, c, d, e, f) nlibc_kevent((a), (b), (c), (d), (e), (f))
#define select      nlibc_select
#define fork        nlibc_fork
#define execl       nlibc_execl
#define execlp      nlibc_execlp
#define chroot      nlibc_chroot
#define kill        nlibc_kill
#define raise       nlibc_raise
#define mknod       nlibc_mknod
#define mkstemp     nlibc_mkstemp
/* OpenSSH's openbsd-compat/openbsd-compat.h carries an UNCONDITIONAL
 * `#define mkstemp(x) _ssh_mkstemp(x)`, which is included after this header and
 * simply overwrites the line above -- so every ssh call site expanded to a
 * name that was NOT routed, and openbsd-compat/mktemp.c then opened with
 * `#ifdef mkstemp / #undef mkstemp` to make sure its own call reached the host.
 * The rewrite below catches the rescan: mkstemp(x) -> _ssh_mkstemp(x) ->
 * nlibc_mkstemp(x). mktemp.c is dropped from the build in meson.build so
 * nothing defines the host-reaching wrapper; see the note there.
 *
 * There is no HAVE_* guard on that macro, so config.h could not have been the
 * lever, and a -D could not either -- the #undef would have removed it. */
#define _ssh_mkstemp nlibc_mkstemp
#define utimes      nlibc_utimes
#define futimes     nlibc_futimes
/* Function-like, as with stat above: `statfs` names both a function and a
 * struct tag, and an object-like macro would rewrite `struct statfs` too. */
#define statfs(a, b) nlibc_statfs((a), (b))
#ifndef NATIVE_LIBC_OWN_GLOB
#define glob        nlibc_glob
#endif

/* Function-like: `mount` is also a struct tag in places, and these should only
 * ever rewrite an actual call. */
#define clock_settime(a, b)      nlibc_clock_settime((a), (b))
#define sethostname(a, b)        nlibc_sethostname((a), (b))
#define reboot(a)                nlibc_reboot((a))
#define mount(a, b, c, d, e)     nlibc_mount((a), (b), (c), (d), (e))
#define getmntinfo               nlibc_getmntinfo
#define uname(a)                 nlibc_uname((a))
/* Not in libSystem below iOS 18.4 / macOS 15.4, so a weak import that is NULL
 * on older devices. See the definition for the crash it caused. */
char *nlibc_strchrnul(const char *s, int c);
#define strchrnul                nlibc_strchrnul

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
#define seteuid                  nlibc_seteuid
#define setegid                  nlibc_setegid
#define setgroups                nlibc_setgroups
#define initgroups               nlibc_initgroups
/* `environ` is a variable, not a call, and it has to stay per-task -- so the
 * rewrite makes it one. SmallCLUE's own `extern char **environ;` becomes a
 * declaration of this function, and every use of `environ` becomes a call to
 * it, which is what keeps two concurrently-running native programs from
 * sharing one environment. */
#define socket                   nlibc_socket
#define socketpair               nlibc_socketpair
#define sendfile                 nlibc_sendfile
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
#define sendmsg                  nlibc_sendmsg
#define recvmsg                  nlibc_recvmsg
#define setsockopt               nlibc_setsockopt
#define getsockopt               nlibc_getsockopt
#define getpeereid               nlibc_getpeereid
#define getaddrinfo              nlibc_getaddrinfo
#define freeaddrinfo             nlibc_freeaddrinfo
#define gai_strerror             nlibc_gai_strerror
#define getnameinfo(a,b,c,d,e,f,g) nlibc_getnameinfo((a),(b),(c),(d),(e),(f),(g))
#define getifaddrs               nlibc_getifaddrs
#define freeifaddrs              nlibc_freeifaddrs
#define gethostbyname            nlibc_gethostbyname
#define gethostbyname2           nlibc_gethostbyname2
#define gethostbyaddr            nlibc_gethostbyaddr
#define getipnodebyname          nlibc_getipnodebyname
#define getipnodebyaddr          nlibc_getipnodebyaddr
#define freehostent              nlibc_freehostent
#define setprotoent              nlibc_setprotoent
#define endprotoent              nlibc_endprotoent
#define getprotoent              nlibc_getprotoent
#define getprotobyname           nlibc_getprotobyname
#define getprotobynumber         nlibc_getprotobynumber

#define tcflush                  nlibc_tcflush
#define tcdrain                  nlibc_tcdrain
#define posix_openpt             nlibc_posix_openpt
#define grantpt                  nlibc_grantpt
#define unlockpt                 nlibc_unlockpt
#define ptsname                  nlibc_ptsname
#define ttyname                  nlibc_ttyname
#define openpty                  nlibc_openpty
#ifndef NATIVE_LIBC_OWN_TERMCAP
#define tgetent                  nlibc_tgetent
#define tgetflag                 nlibc_tgetflag
#define tgetnum                  nlibc_tgetnum
#define tgetstr                  nlibc_tgetstr
#define tgoto                    nlibc_tgoto
#define tputs                    nlibc_tputs
#endif
#define tmpfile                  nlibc_tmpfile
#ifndef NATIVE_LIBC_OWN_GLOB
#define globfree                 nlibc_globfree
#endif
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

#define environ                  (*nlibc_environp())
#define getenv                   nlibc_getenv
#define setenv                   nlibc_setenv
#define unsetenv                 nlibc_unsetenv
#define putenv                   nlibc_putenv
#define getpwuid                 nlibc_getpwuid
#define getpwnam                 nlibc_getpwnam
#define getpwuid_r               nlibc_getpwuid_r
#define getpwnam_r               nlibc_getpwnam_r
#define getgrgid                 nlibc_getgrgid
#define getgrnam                 nlibc_getgrnam
#define getgrouplist             nlibc_getgrouplist
#define user_from_uid            nlibc_user_from_uid
#define group_from_gid           nlibc_group_from_gid
#define crypt                    nlibc_crypt

/* --- the rest of the kernel surface ------------------------------------- */
#define fchmod                   nlibc_fchmod
#define fchown                   nlibc_fchown
#define fchdir                   nlibc_fchdir
#define fsync                    nlibc_fsync
#define fdatasync                nlibc_fdatasync
#define ftruncate                nlibc_ftruncate
#define syncfs                   nlibc_syncfs
#define sync                     nlibc_sync
#define getxattr                 nlibc_getxattr
#define fgetxattr                nlibc_fgetxattr
#define setxattr                 nlibc_setxattr
#define fsetxattr                nlibc_fsetxattr
#define listxattr                nlibc_listxattr
#define flistxattr               nlibc_flistxattr
#define removexattr              nlibc_removexattr
#define fremovexattr             nlibc_fremovexattr
#define nice                     nlibc_nice
#define gettid                   nlibc_gettid
#define sched_yield              nlibc_sched_yield
#define umask                    nlibc_umask
#define setreuid                 nlibc_setreuid
#define setregid                 nlibc_setregid
/* Function-like, for the reason spelled out at statfs above: `flock` names
 * both a function and a STRUCT TAG (<fcntl.h>'s record-locking struct), and an
 * object-like macro rewrites `struct flock lck;` into `struct nlibc_flock`,
 * which does not exist. zsh's Src/Modules/files.c is where that surfaced. Two
 * arguments always, in every caller and in POSIX. */
#define flock(a, b)              nlibc_flock((a), (b))
#define killpg                   nlibc_killpg
#define faccessat                nlibc_faccessat
/* Function-like: `rlimit`, `itimerval`, `tms` and `iovec` are struct tags as
 * well as parts of these names, and an object-like macro would rewrite a
 * declaration as readily as a call. */
#define getrlimit(a, b)          nlibc_getrlimit((a), (b))
#define setrlimit(a, b)          nlibc_setrlimit((a), (b))
#define getdtablesize            nlibc_getdtablesize
#define setitimer(a, b, c)       nlibc_setitimer((a), (b), (c))
#define getitimer(a, b)          nlibc_getitimer((a), (b))
#define alarm                    nlibc_alarm
#define times(a)                 nlibc_times((a))
#define readv                    nlibc_readv
#define writev                   nlibc_writev
#define pread                    nlibc_pread
#define pwrite                   nlibc_pwrite
#define gethostname              nlibc_gethostname
#define getentropy               nlibc_getentropy
#define mkdtemp                  nlibc_mkdtemp
#define mktemp                   nlibc_mktemp
/* Darwin's own name for the same function, and the one zsh actually calls:
 * config.h defines HAVE__MKTEMP, so Src/utils.c's gettempname() -- every
 * here-string, process substitution, named pipe and `fc` edit -- declares
 * `extern char *_mktemp(char *)` and calls THAT. Routing `mktemp` alone left it
 * behind: the uniqueness check ran against the DEVICE's filesystem while the
 * file was then created in the guest, so the guest-side collision check never
 * happened at all. The rewrite catches the extern declaration too, which is
 * why the signature above has to match it exactly. */
#define _mktemp                  nlibc_mktemp
#define pathconf                 nlibc_pathconf
#define fpathconf                nlibc_fpathconf
#define confstr                  nlibc_confstr
#define setpwent                 nlibc_setpwent
#define endpwent                 nlibc_endpwent
#define getpwent                 nlibc_getpwent
#define setservent               nlibc_setservent
#define endservent               nlibc_endservent
#define getservent               nlibc_getservent
#define getservbyname            nlibc_getservbyname
#define getservbyport            nlibc_getservbyport
#define setgrent                 nlibc_setgrent
#define endgrent                 nlibc_endgrent
#define getgrent                 nlibc_getgrent
#define setutxent                nlibc_setutxent
#define endutxent                nlibc_endutxent
#define getutxent                nlibc_getutxent
#define getutxline               nlibc_getutxline
#define getutxid                 nlibc_getutxid
#define pututxline               nlibc_pututxline
#define getlogin                 nlibc_getlogin
#define getlogin_r               nlibc_getlogin_r
#define execve                   nlibc_execve
#define pselect(a,b,c,d,e,f)     nlibc_pselect((a),(b),(c),(d),(e),(f))
#define sigsuspend               nlibc_sigsuspend
#define posix_spawn_file_actions_init    nlibc_posix_spawn_file_actions_init
#define posix_spawn_file_actions_destroy nlibc_posix_spawn_file_actions_destroy
#define posix_spawn_file_actions_adddup2 nlibc_posix_spawn_file_actions_adddup2
#define posix_spawn_file_actions_addclose nlibc_posix_spawn_file_actions_addclose
#define posix_spawn_file_actions_addopen nlibc_posix_spawn_file_actions_addopen
#define posix_spawnattr_init             nlibc_posix_spawnattr_init
#define posix_spawnattr_destroy          nlibc_posix_spawnattr_destroy
#define posix_spawnattr_setflags         nlibc_posix_spawnattr_setflags
#define posix_spawnattr_getflags         nlibc_posix_spawnattr_getflags
#define posix_spawnattr_setpgroup        nlibc_posix_spawnattr_setpgroup
#define posix_spawnattr_getpgroup        nlibc_posix_spawnattr_getpgroup
#define posix_spawnattr_setsigdefault    nlibc_posix_spawnattr_setsigdefault
#define posix_spawnattr_setsigmask       nlibc_posix_spawnattr_setsigmask
#define posix_spawn                      nlibc_posix_spawn
#define posix_spawnp                     nlibc_posix_spawnp

#define openlog                  nlibc_openlog
#define syslog                   nlibc_syslog
#define vsyslog                  nlibc_vsyslog
#define closelog                 nlibc_closelog

#define mmap                     nlibc_mmap
#define munmap                   nlibc_munmap
#define msync                    nlibc_msync
#define _NSGetExecutablePath     nlibc_NSGetExecutablePath
#define _NSGetEnviron            nlibc_NSGetEnviron
#define _NSGetArgv               nlibc_NSGetArgv
#define _NSGetArgc               nlibc_NSGetArgc
#define mkfifo                   nlibc_mkfifo
#define linkat                   nlibc_linkat
#define fclonefileat             nlibc_fclonefileat
#define fcopyfile                nlibc_fcopyfile
#define copyfile_state_alloc     nlibc_copyfile_state_alloc
#define copyfile_state_free      nlibc_copyfile_state_free
#define copyfile_state_get       nlibc_copyfile_state_get
#define setattrlist              nlibc_setattrlist
#define fsetattrlist             nlibc_fsetattrlist

/* Option parsing. The five variables become accessor calls the way `environ`
 * does, and the scanning functions become ours -- routing only the
 * variables would leave getopt's own `place` pointer, and getopt_long's
 * permutation bookkeeping, shared across every native program in the app.
 *
 * A later `#include <getopt.h>` (OpenSSH's includes.h reaches one) is fine:
 * the macros rewrite its declarations into declarations of these, which are
 * compatible with the definitions above. */
#define getopt                   nlibc_getopt
#define getopt_long              nlibc_getopt_long
#define getopt_long_only         nlibc_getopt_long_only
#define optarg                   (*nlibc_optargp())
#define optind                   (*nlibc_optindp())
#define opterr                   (*nlibc_opterrp())
#define optopt                   (*nlibc_optoptp())
#define optreset                 (*nlibc_optresetp())

#define dlopen                   nlibc_dlopen
#define dlsym                    nlibc_dlsym
#define dlclose                  nlibc_dlclose
#define dlerror                  nlibc_dlerror

#endif /* !NATIVE_LIBC_NO_REDIRECT */

#endif /* KERNEL_NATIVE_LIBC_H */
