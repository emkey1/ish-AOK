#!/usr/bin/env python3
"""Fail the build if any variable is thread-local in one place and not another.

bash's globals are __thread so that more than one native bash can be live in
the app at once (docs/bash_native_plan.md). The failure mode when that is done
incompletely is the worst kind available: a translation unit that declares one
of them WITHOUT __thread compiles clean, links clean, and then reads the wrong
memory. Measured on a two-file test, a variable holding 1 read back as
-1882127480; in bash it jumped to address 0 through what it thought was a
thread-vector descriptor.

The compiler catches the mismatch only when both declarations are visible in
one translation unit. Across translation units nothing does -- so this does.

It compares, per object file:

  definitions   __DATA,__thread_vars  says thread-local
                __DATA,__data/__bss/__common  says it is not
  references    ARM64_RELOC_TLVP_*    accesses it as thread-local
                everything else       accesses it as ordinary data

and reports any symbol that appears on both sides of either line.
"""

import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# One source of truth for what is allowed to stay shared: the tool that would
# otherwise convert it. A name added here without being added there would be
# converted on the next run and the justification lost.
from importlib import import_module
ALLOW = import_module("bash-tls-fix-externs").ALLOW

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ARCHIVES = [os.path.join(REPO, "build", "libbash.a"),
                    os.path.join(REPO, "build", "libzsh.a"),
                    os.path.join(REPO, "build", "libish.a")]

# Archives of vendored native programs -- the ones where "mutable and shared
# between threads" is a bug rather than a design choice, so the whole-class
# question in shared_mutable() applies. Anything not listed here gets only the
# mismatch checks.
VENDORED_NATIVE = ("libbash.a", "libzsh.a", "libsmallclue.a", "libopenssh.a",
                   "libopenssh_scp.a", "libopenssh_stubs.a",
                   "libopenssh_smult_curve25519_ref.a")

# In a relocatable object a tentative definition (`int x;` with no initialiser)
# is printed as `(common)`, not `(__DATA,__common)` -- that is only its section
# once linked. Missing it let `int cdable_vars;` in cd.def stay ordinary data
# while shopt.c reached it as thread-local, which jumped to address 0.
# Const objects count as ordinary data too. A definition can be
# `const char * const dist_version` -- correctly left alone, since const objects
# are shared -- while its extern declaration says plain `char *dist_version` and
# so got __thread. Reading that as thread-local treats the string's first bytes
# as a thread-vector descriptor: bash called the address of the text "5.2".
DATA_SECTIONS = ("__DATA,__data", "__DATA,__bss", "__DATA,__common", "(common)",
                 "__DATA,__const", "__TEXT,__const")
TLS_SECTIONS = ("__DATA,__thread_vars", "__DATA,__thread_data", "__DATA,__thread_bss")


def ignorable(name):
    """Assembler temporaries and the thread-vector machinery's own symbols.
    `_x$tlv$init` is the descriptor clang emits FOR a thread-local x, so seeing
    it reached as ordinary data says nothing about x."""
    return "$tlv$" in name or name.startswith(("ltmp", "l_", "L"))


def run(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def classify(objdir):
    """(tls_defs, data_defs, tls_refs, data_refs), each name -> set of objects."""
    tls_defs, data_defs, tls_refs, data_refs = {}, {}, {}, {}
    for entry in sorted(os.listdir(objdir)):
        if not entry.endswith(".o"):
            continue
        obj = os.path.join(objdir, entry)

        # Names this object defines for itself. A relocation naming one of them
        # is resolved inside the object and says nothing about anyone else's
        # variable of the same name -- bash's test.c has a `static term()` (the
        # test builtin's recursive-descent parser) and zsh has a thread-local
        # `char *term` for $TERM, and once both archives are folded into
        # libish.a the calls to bash's function were reported as ordinary-data
        # reads of zsh's variable. They never meet: bash's is local.
        local = set()
        for line in run("nm", "-m", obj).splitlines():
            if "non-external" in line and " (" in line:
                local.add(line.split()[-1])

        for line in run("nm", "-m", obj).splitlines():
            # EXTERNAL definitions only. Two files may each have their own
            # `static int foo;` and those are genuinely different variables --
            # comparing them would report a mismatch that does not exist.
            if "non-external" in line or " external" not in line:
                continue
            name = line.split()[-1]
            if ignorable(name):
                continue
            for sec, table in ((TLS_SECTIONS, tls_defs), (DATA_SECTIONS, data_defs)):
                if any(s in line for s in sec):
                    table.setdefault(name, set()).add(entry)

        for line in run("xcrun", "llvm-objdump", "--reloc", obj).splitlines():
            parts = line.split()
            if len(parts) < 3 or not parts[1].startswith("ARM64_RELOC"):
                continue
            kind, sym = parts[1], parts[2]
            if not sym.startswith("_") or ignorable(sym) or sym in local:
                continue
            (tls_refs if "TLVP" in kind else data_refs).setdefault(sym, set()).add(entry)
    return tls_defs, data_defs, tls_refs, data_refs


def shared_mutable(objdir):
    """Every mutable variable that is still shared between threads.

    The mismatch checks below only fire when two files DISAGREE. A variable no
    file ever made thread-local is something they all agree on -- consistently
    shared, and consistently wrong once two shells are live. That is not a
    corner case: it was 129 file-local statics and 13 externals, including
    o_options, one of the tables each thread fills in with its own addresses.

    So this asks the whole question instead: what is in a writable data section
    and not thread-local? Anything that legitimately belongs there is in ALLOW,
    where it has to be justified in writing.
    """
    out = {}
    for entry in sorted(os.listdir(objdir)):
        if not entry.endswith(".o"):
            continue
        for line in run("nm", "-m", os.path.join(objdir, entry)).splitlines():
            if not re.search(r"\((__DATA,__(data|bss|common)|common)\)", line):
                continue
            name = line.split()[-1]
            if ignorable(name) or not name.startswith("_"):
                continue
            if name[1:] in ALLOW:
                continue
            out.setdefault(name, set()).add(entry)
    return out


def main():
    archives = sys.argv[1:] or DEFAULT_ARCHIVES
    archives = [a for a in archives if os.path.exists(a)]
    if not archives:
        print("check-bash-tls: no archives to check (build first)", file=sys.stderr)
        return 0

    bad = []
    for archive in archives:
        with tempfile.TemporaryDirectory() as tmp:
            # Absolute, because `ar x` runs with cwd=tmp. A relative archive
            # path extracts nothing, and a gate that examined no objects prints
            # "clean" -- the exact failure this whole file exists to prevent.
            subprocess.run(["ar", "x", os.path.abspath(archive)], cwd=tmp,
                           capture_output=True)
            tls_defs, data_defs, tls_refs, data_refs = classify(tmp)

            # libish.a is AOK's own kernel, whose globals are process-wide by
            # design, so the whole-class question only makes sense for the
            # vendored native programs. smallclue is NOT in DEFAULT_ARCHIVES:
            # name it explicitly to gate it, which keeps the default run -- the
            # one wired into everyone's habits -- meaning exactly what it did.
            # The openssh* ones are the ssh/scp/sftp/ssh-keygen family, split
            # across archives only because two files need their own compiler
            # flags; all of them are native programs with the same exposure.
            if os.path.basename(archive) in VENDORED_NATIVE:
                for name, objs in sorted(shared_mutable(tmp).items()):
                    bad.append(f"{name}: mutable and shared between shells, in "
                               f"{sorted(objs)[:2]} -- make it __thread, or add it "
                               f"to ALLOW in tools/bash-tls-fix-externs.py with a reason")

            # A name that is BOTH a thread-local definition and an ordinary one
            # is two different variables to two different files.
            for name in sorted(set(tls_defs) & set(data_defs)):
                bad.append(f"{name}: defined thread-local in {sorted(tls_defs[name])[:2]} "
                           f"and as ordinary data in {sorted(data_defs[name])[:2]}")

            # A thread-local variable reached without __thread reads the
            # descriptor as if it were the value -- silently.
            for name in sorted(set(tls_defs) & set(data_refs)):
                bad.append(f"{name}: thread-local, but {sorted(data_refs[name])[:3]} "
                           f"reach it as ordinary data")

            # And the reverse: a declaration that says __thread for something
            # that is not.
            for name in sorted(set(data_defs) & set(tls_refs)):
                bad.append(f"{name}: ordinary data, but {sorted(tls_refs[name])[:3]} "
                           f"reach it as thread-local")

    if bad:
        print(f"check-bash-tls: {len(bad)} mismatches", file=sys.stderr)
        for b in bad:
            print(f"  {b}")
        return 1
    print("check-bash-tls: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
