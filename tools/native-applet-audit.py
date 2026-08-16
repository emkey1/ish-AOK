#!/usr/bin/env python3
"""Which SmallCLUE applets cannot work as native programs in this build?

opt/AOK/tools/native-links.sh symlinks applets into /usr/local/bin, which
precedes /usr/bin -- so a linked applet shadows the distro's command. Linking
one that does not work replaces a working command with an error, and the
failure then surfaces somewhere unrelated.

Writing that exclusion list by hand was not good enough: `env` was missed, and
because PSCAL's harness runs `env ... bash ...`, installing the links took its
suite from 217 passing to zero. env has to exec, and exec is not implemented.

So it is derived instead. An applet is excluded if its handler can reach any
call the shim answers with ENOSYS (kernel/native_libc.c), following helper
functions transitively -- not just direct calls, since xargs, time, nohup and
chroot all reach exec through helpers.

Deliberately conservative: ANY path counts, even a rare one. `find` works
until -exec, and a command that fails only on certain flags is a nastier trap
than one that is simply absent.

This does not catch applets that fail for reasons other than an unimplemented
call -- missing OpenSSL, zlib, libgit2 or the OpenSSH tree -- because those
report "not built into this iSH-AOK" from kernel/smallclue_glue.c and are
listed separately in the script.

Usage: tools/native-applet-audit.py [smallclue-src-dir]
"""
import glob
import os
import re
import sys
import textwrap

# Everything kernel/native_libc.c still answers with ENOSYS unconditionally.
# Keep in step with that file: the way to re-derive it is to look for the
# nlibc_* functions whose whole body is a _ENOSYS failure.
#
# This list used to be much longer, and every removal was a call becoming real
# rather than a judgement being relaxed: exec/system/waitpid/wait and
# smallclueSpawn once the spawn hook created guest tasks, dup2/poll/select/
# fcntl/kill/chroot/mknod/futimes once the shim went through the syscall
# dispatcher, ioctl once the terminal requests were mapped, popen/pclose once
# there was a pipe and a way to set a child's descriptors up.
#
# fcntl, ioctl and sigaction are PARTIAL rather than complete, and are left off
# deliberately rather than by oversight -- fcntl refuses the lock commands,
# ioctl anything that is not a terminal request, sigaction the SA_SIGINFO form.
# SmallCLUE asks for none of those: F_SETFD/F_GETFL/F_DUPFD/F_DUPFD_CLOEXEC and
# TIOCGWINSZ/TIOCSWINSZ/TIOCSPGRP/TIOCSCTTY is the whole of what it uses.
# Re-check that before assuming it still holds for a new native program.
# getifaddrs came off this list when kernel/native_libc.c stopped refusing it:
# AOK has no interfaces of its own, so the host's list IS the guest's, and the
# kernel already builds /proc/net/dev from the same call.
UNIMPLEMENTED = (
    "fork", "glob",
)


def main():
    src_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "deps", "smallclue", "src")
    sources = sorted(glob.glob(os.path.join(src_dir, "*.c")))
    if not sources:
        print(f"no sources under {src_dir}", file=sys.stderr)
        return 2
    src = "\n".join(open(f, errors="replace").read() for f in sources)

    table = re.search(
        r"static const SmallclueApplet kSmallclueApplets\[\] = \{(.*?)\n\};", src, re.S)
    if table is None:
        print("could not find the applet table", file=sys.stderr)
        return 2
    applets = {}
    for m in re.finditer(r'\{"([^"]+)",\s*([A-Za-z_][A-Za-z0-9_]*)', table.group(1)):
        applets.setdefault(m.group(2), []).append(m.group(1))

    # Function bodies, crudely: a definition at column 0 through the next
    # column-0 closing brace. Good enough for a conservative audit.
    bodies = {}
    for m in re.finditer(r"\n(?:static\s+)?[A-Za-z_][\w \*]*\b(\w+)\s*\([^;{]*\)\s*\{", src):
        end = src.find("\n}", m.end())
        bodies[m.group(1)] = src[m.end():end if end > 0 else len(src)]

    seed = re.compile(r"\b(" + "|".join(UNIMPLEMENTED) + r")\s*\(")
    tainted = {fn for fn, body in bodies.items() if seed.search(body)}
    calls = {fn: set(re.findall(r"\b(\w+)\s*\(", body)) for fn, body in bodies.items()}
    changed = True
    while changed:
        changed = False
        for fn, callees in calls.items():
            if fn not in tainted and (callees & tainted):
                tainted.add(fn)
                changed = True

    bad = sorted({name for fn, names in applets.items() if fn in tainted for name in names})
    total = sum(len(v) for v in applets.values())
    print(f"{len(bad)} of {total} applets reach an unimplemented call:")
    print(textwrap.fill(" ".join(bad), 92, initial_indent="  ", subsequent_indent="  "))
    return 0


sys.exit(main())
