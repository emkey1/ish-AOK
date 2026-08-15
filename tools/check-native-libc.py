#!/usr/bin/env python3
"""Fail the build if natively-compiled guest programs still reference the host
libc for anything filesystem- or process-shaped.

Why this exists
---------------
A program compiled into iSH-AOK to run as host code (kernel/native.h) links
against the HOST libc. If such a program calls open()/stat()/opendir()
directly, it resolves against the iOS filesystem instead of the guest's rootfs
-- and *succeeds*, reading the wrong file. Silent wrongness, not a crash.

That risk cannot be managed by reading the code: smallclue alone is ~120k lines
with several hundred such calls, and a single missed one is invisible until it
corrupts something. So the redirection is enforced mechanically here instead:
every symbol below must be routed through kernel/native_io.h, and anything
still undefined-and-referenced at link time fails the build.

Note this cannot be solved by path rewriting the way PSCAL's iOS app does it
(common/path_truncate.h expands a virtual path into a real one and then uses
plain libc). iSH's guest filesystem is a fakefs -- mangled names plus a
meta.db -- so no real path exists for libc to open. Every call has to go
through the VFS.

Usage: check-native-libc.py <object-or-archive> [...]
"""
import re
import subprocess
import sys

# Anything that resolves a path, touches a descriptor, or asks the OS about a
# file. Deliberately broad: a false positive costs one redirect, a false
# negative costs silent host-filesystem access.
DENIED = {
    # path -> object
    "open", "openat", "creat", "fopen", "freopen", "opendir", "fdopendir",
    "access", "faccessat", "stat", "lstat", "fstat", "fstatat", "statfs",
    "statvfs", "realpath", "readlink", "readlinkat", "glob",
    # directory walk
    "readdir", "readdir_r", "closedir", "rewinddir", "seekdir", "telldir",
    "scandir", "ftw", "nftw", "fts_open",
    # mutation
    "unlink", "unlinkat", "rmdir", "mkdir", "mkdirat", "rename", "renameat",
    "link", "linkat", "symlink", "symlinkat", "chmod", "fchmod", "fchmodat",
    "chown", "lchown", "fchown", "truncate", "ftruncate", "utimes",
    "utimensat", "futimes", "mknod", "mkfifo", "mkstemp", "mkdtemp",
    # descriptor io
    "read", "pread", "readv", "write", "pwrite", "writev", "close", "lseek",
    "dup", "dup2", "fcntl", "ioctl", "select", "poll", "pipe",
    # cwd / process
    "getcwd", "chdir", "fchdir", "chroot", "fork", "vfork", "execv", "execve",
    "execvp", "execl", "execlp", "system", "popen", "pclose", "waitpid",
    "wait", "kill",
    # Host-global state. Not filesystem calls, but the same class of mistake
    # and worse consequences: on a host build these reach the developer's own
    # clock, hostname, mount table and power state.
    "clock_settime", "settimeofday", "adjtime", "sethostname", "setdomainname",
    "reboot", "mount", "unmount", "umount", "swapon", "swapoff",
}

SKIP_PREFIXES = ("__",)


def undefined_symbols(path):
    out = subprocess.run(["nm", "-ju", path], capture_output=True, text=True)
    syms = set()
    for line in out.stdout.splitlines():
        s = line.strip()
        if not s:
            continue
        s = re.sub(r"^_", "", s)
        s = re.sub(r"\$.*$", "", s)  # realpath$DARWIN_EXTSN and friends
        if s.startswith(SKIP_PREFIXES):
            continue
        syms.add(s)
    return syms


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2

    offenders = {}
    for path in sys.argv[1:]:
        for sym in sorted(undefined_symbols(path) & DENIED):
            offenders.setdefault(sym, []).append(path)

    if not offenders:
        print("check-native-libc: clean, no un-redirected host libc calls")
        return 0

    print("check-native-libc: FAIL -- these reach the HOST filesystem, not the "
          "guest's:", file=sys.stderr)
    for sym, paths in sorted(offenders.items()):
        where = ", ".join(p.split("/")[-1] for p in sorted(set(paths))[:4])
        print(f"  {sym:16s} {where}", file=sys.stderr)
    print(f"\n{len(offenders)} symbol(s). Route each through "
          f"kernel/native_io.h.", file=sys.stderr)
    return 1


sys.exit(main())
