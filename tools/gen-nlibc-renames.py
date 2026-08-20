#!/usr/bin/env python3
"""Emit llvm-objcopy --redefine-sym flags routing a foreign object's libc
imports onto AOK's shim.

Why a generator rather than a list. kernel/native_libc.h redirects libc by
#define, which only reaches translation units AOK compiles. An object from
another toolchain -- Rust, Go, anything with its own build -- imports the real
names, and the only way to reach it is to rewrite the symbols in the archive
before it is linked. That rewrite has to name exactly the same set the header
redirects, and a hand-kept second list would drift the first time someone adds
a route. So it is read from the header, which is the one place that decides.

Why --redefine-sym and not the linker's -alias. An alias is global: it would
also redirect AOK's OWN calls, and the kernel must reach the real host open().
Rewriting symbols in one archive is the only version of this that is scoped.

Usage:
    gen-nlibc-renames.py [--format=objcopy|list] [native_libc.h]
"""
import os
import re
import sys

# Names the header redirects that must NOT be rewritten in a foreign object.
# Each one is a deliberate exception with a reason.
SKIP = {
    # Rust's std reaches these through its own thread-local machinery before
    # the shim's per-task state exists; routing them would run shim code on a
    # thread that has no current task.
    "__tls_get_addr",
}


# Darwin exports several libc entry points under a suffixed symbol as well as
# the bare name, and the compiler picks the suffixed one: <stdlib.h> makes
# realpath() emit _realpath$DARWIN_EXTSN, and on x86_64 the stat family emits
# $INODE64. A rename list keyed only on the bare name misses those, and misses
# them SILENTLY -- Rust's fs::canonicalize was resolving against the host's
# filesystem with every visible check passing.
#
# Routing the suffixed symbol to the same nlibc_ function is right rather than
# merely expedient: the shim is compiled against these same headers, so its
# realpath() and struct stat are the suffixed variant's, not the legacy one's.
DARWIN_VARIANTS = ("$DARWIN_EXTSN", "$INODE64", "$UNIX2003", "$NOCANCEL")


def renames(header_path):
    text = open(header_path).read()
    names = []
    for m in re.finditer(r"^#define\s+(\w+)\s*(?:\([^)]*\))?\s+.*\bnlibc_(\w+)", text, re.M):
        guest_name, impl = m.group(1), m.group(2)
        if guest_name in SKIP:
            continue
        names.append((guest_name, "nlibc_" + impl))
    return sorted(set(names))


def main():
    fmt = "objcopy"
    args = [a for a in sys.argv[1:]]
    for a in list(args):
        if a.startswith("--format="):
            fmt = a.split("=", 1)[1]
            args.remove(a)
    header = args[0] if args else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "kernel", "native_libc.h")

    pairs = renames(header)
    if not pairs:
        print("gen-nlibc-renames: no redirects found in " + header, file=sys.stderr)
        return 1
    for guest, impl in pairs:
        for guest_sym in [guest] + [guest + v for v in DARWIN_VARIANTS]:
            if fmt == "objcopy":
                # Mach-O symbols carry a leading underscore.
                print("--redefine-sym")
                print("_%s=_%s" % (guest_sym, impl))
            else:
                print("%s %s" % (guest_sym, impl))
    return 0


sys.exit(main())
