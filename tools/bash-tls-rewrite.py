#!/usr/bin/env python3
"""Make bash's mutable globals thread-local, so more than one native bash can
be live at once.

WHY THIS EXISTS
---------------
A native program in iSH-AOK is a C function on a guest task's thread, inside the
app's one address space (docs/bash_native_plan.md). Two live native bash
instances therefore share bash's globals, which is the same wall fork hits --
so kernel/bash_glue.c used to allow only one, and every other shell in the app
silently got the emulated bash instead.

The state that actually has to differ per shell is small: 50 KB of __data,
__bss and __common across the whole bash+readline archive. Making each of those
variables `__thread` gives every task its own copy and removes the restriction
outright. Measured on this platform, TLS access costs nothing in a loop --
clang hoists the resolution -- and bash's own malloc is not compiled in, so
there is no large arena to duplicate.

WHAT IT DOES
------------
Runs clang over every bash translation unit with `-ast-dump=json`, finds every
variable with static storage duration that lives in the vendored tree, and
inserts `__thread ` in front of its declaration. Definitions and `extern`
declarations alike, because a TU that declares one without `__thread` generates
non-TLS access code for it.

It is a build-time tool rather than a patch so that moving to a later bash is a
re-run rather than a re-do. It is idempotent: a declaration that already says
__thread is left alone.

WHAT IT DELIBERATELY SKIPS
--------------------------
  const objects            shared safely, and read-only memory is worth keeping
  macro expansions         the insertion point would be the use, not the text
  generated builtins       build/*.c comes from builtins/*.def; see --report
"""

import argparse
import json
import re
import os
import shlex
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def tu_flags(entry):
    """The compile command, minus the bits that write output."""
    args = shlex.split(entry["command"])
    out, skip = [], 0
    for a in args[1:]:
        if skip:
            skip -= 1
            continue
        if a in ("-o", "-MQ", "-MF"):
            skip = 1
            continue
        if a in ("-c", "-MD"):
            continue
        out.append(a)
    return args[0], out


def walk(node, cur_file, want, out, base):
    """Collect (file, offset, name) for every static-duration VarDecl.

    clang's JSON omits a location's file when it repeats, so the current file
    has to be carried down the walk. It must be updated in EXACTLY the order
    clang emits locations -- loc, then range.begin, then range.end, then the
    children -- because the omission means "same as the last one printed". A
    first version skipped range.end, drifted out of step, and attributed
    offsets from variables.c to pcomplete.h; the word-boundary guard below is
    what caught it.
    """
    if not isinstance(node, dict):
        return cur_file

    def take(where):
        nonlocal cur_file
        if isinstance(where, dict) and "file" in where:
            cur_file = where["file"]

    loc = node.get("loc") or {}
    rng = node.get("range") or {}
    begin = rng.get("begin") or {}
    end = rng.get("end") or {}

    take(loc)
    decl_file, decl_offset = None, None
    take(begin)
    if "offset" in begin:
        decl_file, decl_offset = cur_file, begin["offset"]
    take(end)

    if node.get("kind") == "VarDecl" and decl_offset is not None:
        # Block-scope variables are per-call unless static; file-scope ones
        # always have static duration.
        storage = node.get("storageClass")
        qual = (node.get("type") or {}).get("qualType", "")
        is_const = qual.startswith("const ") or " const " in qual
        parent_is_fn = node.get("_in_function", False)
        static_duration = (not parent_is_fn) or storage == "static"
        expansion = begin.get("expansionLoc") or loc.get("expansionLoc")
        if static_duration and not is_const and not expansion and decl_file and want(decl_file):
            # Resolve against the TU's own directory: clang reports paths as
            # the compiler saw them ("../deps/bash/x.h"), and resolving those
            # against this process's cwd invents files that do not exist.
            out.append((os.path.realpath(os.path.join(base, decl_file)),
                        decl_offset, node.get("name", "?")))

    in_fn = node.get("kind") in ("FunctionDecl", "CompoundStmt") or node.get("_in_function", False)
    for child in node.get("inner", []) or []:
        if isinstance(child, dict):
            child["_in_function"] = in_fn
        cur_file = walk(child, cur_file, want, out, base)
    return cur_file


def collect_one(job):
    entry, roots = job
    cc, flags = tu_flags(entry)
    cmd = [cc] + flags + ["-fsyntax-only", "-Xclang", "-ast-dump=json"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=entry["directory"], timeout=600)
    except subprocess.TimeoutExpired:
        return entry["file"], [], "timeout"
    if r.returncode != 0 or not r.stdout:
        return entry["file"], [], (r.stderr or "")[:200]
    try:
        tree = json.loads(r.stdout)
    except json.JSONDecodeError as exc:
        return entry["file"], [], f"json: {exc}"

    found = []

    def want(path):
        rp = os.path.realpath(os.path.join(entry["directory"], path))
        # mkbuiltins and friends are BUILD tools, compiled for this Mac and run
        # during the build. They are not part of any shell and must not be
        # given thread-local state.
        if os.path.basename(rp) in ("mkbuiltins.c", "mksignames.c", "mksyntax.c",
                                    "psize.c", "man2html.c"):
            return False
        return any(rp.startswith(root) for root in roots)

    walk(tree, None, want, found, entry["directory"])
    return entry["file"], found, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compdb", default=os.path.join(REPO, "build", "compile_commands.json"))
    ap.add_argument("--root", action="append", default=None,
                    help="only rewrite files under this directory (repeatable)")
    ap.add_argument("--report", action="store_true", help="list what would change, change nothing")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    roots = [os.path.realpath(r) for r in (args.root or [os.path.join(REPO, "deps", "bash")])]

    with open(args.compdb) as fh:
        db = json.load(fh)
    tus = [e for e in db
           if os.path.realpath(os.path.join(e["directory"], e["file"])).startswith(
               os.path.realpath(os.path.join(REPO, "deps", "bash")))
           or "/builtins/" in e["file"] or e["file"].endswith(("shopt.c", "set.c"))]
    # Generated builtins live in the build directory; keep them so --report can
    # say so, but they are never rewritten (the .def file is the source).
    print(f"translation units: {len(tus)}", file=sys.stderr)

    sites, failures = {}, []
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for name, found, err in pool.map(collect_one, [(e, roots) for e in tus]):
            if err:
                failures.append((name, err))
                continue
            for path, off, var in found:
                sites.setdefault(path, {})[off] = var

    if failures:
        print(f"{len(failures)} translation units could not be parsed:", file=sys.stderr)
        for name, err in failures[:5]:
            print(f"  {name}: {err}", file=sys.stderr)

    total = sum(len(v) for v in sites.values())
    print(f"declarations to make thread-local: {total} across {len(sites)} files", file=sys.stderr)

    if args.report:
        for path in sorted(sites, key=lambda p: -len(sites[p]))[:40]:
            print(f"  {len(sites[path]):5d}  {os.path.relpath(path, REPO)}")
        return 0

    changed, skipped = 0, []
    for path, offsets in sites.items():
        with open(path, "r", encoding="utf-8", errors="surrogateescape") as fh:
            text = fh.read()
        # Right to left, so earlier offsets stay valid.
        for off in sorted(offsets, reverse=True):
            if text[off:off + 9] == "__thread ":
                continue
            # REFUSE to insert inside a word. clang omits a location's file
            # when it repeats, and a node whose range.begin crosses an include
            # boundary can therefore be attributed to the file being tracked
            # rather than its own -- which lands an offset from one file in the
            # middle of another. The first run of this tool turned ITEMLIST
            # into ITE__thread MLIST in pcomplete.h, 152 errors deep. A
            # declaration always starts at a word boundary, so this cannot
            # reject a legitimate site.
            name = offsets[off]
            if off >= len(text):
                skipped.append((path, off, name, "offset past end of file"))
                continue
            before = text[off - 1] if off > 0 else "\n"
            after = text[off:off + 1]
            if (before.isalnum() or before == "_") or not (after.isalpha() or after == "_"):
                skipped.append((path, off, name, repr(text[max(0, off - 12):off + 12])))
                continue
            # And the site must actually declare the variable clang named. This
            # is the check that makes the whole tool safe: clang's JSON omits a
            # location's file when it repeats, and reconstructing that by
            # walking the tree is evidently not reliable -- offsets from
            # variables.c arrived attributed to pcomplete.h. A declaration
            # names its variable before the semicolon that ends it, so a site
            # that does not is not the site clang meant.
            decl = text[off:off + 600].split(";")[0]
            if not re.search(r"\b" + re.escape(name) + r"\b", decl):
                skipped.append((path, off, name, "declaration does not name it: " + repr(decl[:60])))
                continue
            text = text[:off] + "__thread " + text[off:]
            changed += 1
        with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
            fh.write(text)
    print(f"inserted {changed} __thread specifiers", file=sys.stderr)
    if skipped:
        print(f"REFUSED {len(skipped)} sites that were not at a word boundary "
              f"(see the comment above); these need checking by hand:", file=sys.stderr)
        for path, off, name, ctx in skipped[:15]:
            print(f"  {os.path.relpath(path, REPO)}@{off} {name}: {ctx}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
