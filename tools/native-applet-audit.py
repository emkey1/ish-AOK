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
call the shim answers with ENOSYS (kernel/smallclue_shim.c), following helper
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

# Everything kernel/smallclue_shim.c answers with ENOSYS, plus the spawn family
# that has no guest-task routing yet. Keep in step with that file.
UNIMPLEMENTED = (
    "execvp", "execv", "execl", "execlp", "system", "popen", "pclose",
    "smallclueSpawn", "fork", "waitpid", "wait",
    "dup2", "fcntl", "ioctl", "poll", "select",
    "glob", "mknod", "futimes", "kill", "chroot",
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
