#!/usr/bin/env python3
"""Make bash's file-local statics thread-local, driven by the linker's answer.

tools/bash-tls-rewrite.py works from clang's JSON AST, and clang omits a
location's file whenever it repeats the previous one. Emulating that
"same file as last time" state exactly turned out to be a losing game -- macro
locations carry their file inside nested spellingLoc/expansionLoc dicts, so the
walk drifts and starts attributing variables.c's offsets to pcomplete.h. The
word-boundary guard there rejects the bad ones rather than corrupting a header,
which is why this whole class -- 129 file-local statics -- was quietly skipped.

So this asks the object files instead. `nm -m` says, per symbol, exactly which
section it landed in, and a non-external symbol in __DATA,__data/__bss/__common
is precisely a mutable static that is still shared between threads. No offsets,
no location tracking, nothing to drift: the name and the verdict come from the
linker's own view of what was compiled.

That is also what makes it checkable. After patching, the same query should
come back empty apart from the deliberate exceptions in SHARED below --
tools/check-bash-tls.py enforces that, and unlike its mismatch checks it covers
the whole class rather than the cases where two files happen to disagree.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VENDOR = os.path.join(REPO, "deps", "bash")

# Statics that must stay shared, shared with the externs tool so the two cannot
# disagree -- each entry there carries its justification. Without this, running
# the two tools in either order undoes the other's deliberate decisions:
# default_prefixes went back to __thread and stopped compiling, because
# tilde.c initialises a global with its address.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from importlib import import_module
SHARED = import_module("bash-tls-fix-externs").SKIP


def source_for(obj):
    """Object file name -> the source to edit.

    The builtins are the interesting case: what compiled was build/shopt.c,
    generated from builtins/shopt.def by mkbuiltins. Editing the generated file
    would be undone by the next build, so the .def is the only real target --
    its C sections are copied through verbatim, so the same text is there.
    """
    name = obj[:-2] if obj.endswith(".o") else obj
    m = re.match(r"meson-generated_\.\._(.+)\.c$", name)
    if m:
        cand = os.path.join(REPO, "deps", "bash", "builtins", m.group(1) + ".def")
        return cand if os.path.exists(cand) else None
    # Any vendored tree, not just bash. smallclue has the same problem for the
    # same reason -- its applets are native programs too, so two live ones share
    # every global -- and its objects are named deps_smallclue_src_awk_interp.c.o
    # in exactly the same scheme.
    if not name.startswith("deps_"):
        return None
    # Underscores in the object name stand for path separators, but there are
    # source files with underscores in them too (awk_interp.c, runtime_support.c),
    # so guessing where the path stops and the filename starts is not safe.
    # Try every split and let the filesystem decide.
    parts = name[len("deps_"):].split("_")
    for i in range(len(parts), 0, -1):
        cand = os.path.join(REPO, "deps", *parts[:i - 1], "_".join(parts[i - 1:]))
        if os.path.exists(cand):
            return cand
    return None


def statics(archive):
    """{source path: {symbol names}} for every still-shared mutable static."""
    out = {}
    with tempfile.TemporaryDirectory() as tmp:
        # Absolute: `ar x` runs with cwd=tmp, so a relative archive path
        # resolves against the temp directory, finds nothing, and -- because the
        # output is captured -- reports "nothing shared" instead of failing.
        subprocess.run(["ar", "x", os.path.abspath(archive)], cwd=tmp,
                       capture_output=True)
        for entry in sorted(os.listdir(tmp)):
            if not entry.endswith(".o"):
                continue
            nm = subprocess.run(["nm", "-m", os.path.join(tmp, entry)],
                                capture_output=True, text=True).stdout
            for line in nm.splitlines():
                # See the note in bash-tls-fix-externs.py: an alignment field
                # can sit between the section and the linkage word.
                if not re.search(r"\((__DATA,__(data|bss|common)|common)\)", line):
                    continue
                if "non-external" not in line:
                    continue
                sym = line.split()[-1]
                if "$tlv$" in sym or not sym.startswith("_"):
                    continue
                name = sym[1:]
                if name in SHARED or name.startswith(("l_", "L", "ltmp")):
                    continue
                out.setdefault(entry, set()).add(name)
    return out


DEFN = re.compile(r"^(?P<pre>\s*)static(?P<post>\s+)(?!__thread\b)")


def in_parens(window, name):
    """True if `name` sits inside an unclosed `(` -- i.e. it is a parameter.

    `static void do_chop(line, delim)` names `delim`, and mapfile.def really
    does have a static called `delim` as well. Patching the function put
    `static __thread void` on a function definition, which does not compile.
    Counting the parens is the whole distinction: a declarator is at depth 0.

    The occurrence has to be found the same way the caller found it -- on a word
    boundary. A plain .index() lands on the first SUBSTRING, and OpenSSH's
    `set_next_interval(..., struct timespec *next_interval, ...)` contains the
    static `next_interval` inside the function's own name, at depth 0. The
    guard passed, the function got `static __thread void`, and the build
    stopped on "'__thread' is only allowed on variable declarations".
    """
    m = re.search(r"\b" + re.escape(name) + r"\b", window)
    if m is None:
        return False
    head = window[:m.start()]
    return head.count("(") > head.count(")")


def patch(path, names):
    """Insert `__thread` after the `static` that defines each name.

    Two shapes, because bash writes its variables two ways:

      static int export_env_index;          the `static` is on the name's line
      static struct { ... } shopt_vars[] =  the `static` is ten lines above,
                                            opening an anonymous struct

    The second shape is not a curiosity -- shopt_vars, o_options, long_args,
    posix_vars and boolean_varlist all have it, and they are exactly the tables
    whose entries aok_fix_*() fills in per thread. Left shared, every thread
    would write its own variables' addresses into one table and the last writer
    would win, which is the race this whole conversion exists to remove.
    """
    with open(path, encoding="utf-8", errors="surrogateescape") as fh:
        lines = fh.read().split("\n")

    # nm reports a static inside a function as `func.var`; only the tail is
    # what the source calls it.
    wanted = {n.rsplit(".", 1)[-1]: n for n in names}
    done, hit = set(), set()

    def is_function(window):
        """`static void scanparamvals(HashNode hn, int flags)` is not a variable.

        in_parens() only rejects a name found INSIDE the parameter list; it says
        nothing when some other wanted name matches the return type or the
        function's own name, and then `static __thread void` lands on a function
        and the file stops compiling. This asks the shape of the whole
        declarator instead: no `=` anywhere and a `name(` in it is a function,
        unless the `(` opens a function-POINTER declarator, which is a variable.
        """
        if "=" in window:
            return False
        if re.search(r"\(\s*\*", window):
            return False
        return re.search(r"\w\s*\(", window) is not None

    def mark(i, window=None):
        m = DEFN.match(lines[i])
        if not m or i in done:
            return False
        if window is not None and is_function(window):
            return False
        lines[i] = m.group("pre") + "static __thread" + m.group("post") + lines[i][m.end():]
        done.add(i)
        return True

    for i, line in enumerate(lines):
        if not DEFN.match(line):
            continue
        # A definition can wrap: `static struct foo\n  name[10] = {`.
        window, j = line, i
        while j + 1 < len(lines) and not re.search(r"[;=(]", window) and j - i < 3:
            j += 1
            window += " " + lines[j]
        found = [s for s in wanted
                 if re.search(r"\b" + re.escape(s) + r"\b", window)
                 and not in_parens(window, s)]
        if found and mark(i, window):
            hit.update(wanted[s] for s in found)

    # Second shape: find the name's own line, then walk back to the `static`
    # that opens the declaration it belongs to, balancing braces on the way.
    for short, full in wanted.items():
        if full in hit:
            continue
        for i, line in enumerate(lines):
            if not re.search(r"\b" + re.escape(short) + r"\s*\[\s*\]\s*=", line):
                continue
            bal = line.count("}") - line.count("{")
            for j in range(i, -1, -1):
                if j < i:
                    bal += lines[j].count("}") - lines[j].count("{")
                if bal <= 0 and DEFN.match(lines[j]):
                    if mark(j):
                        hit.add(full)
                    break
            break

    if hit:
        with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
            fh.write("\n".join(lines))
    return hit


def is_function_decl(window):
    """See is_function() inside patch(); the .pro walk needs the same test."""
    if "=" in window:
        return False
    if re.search(r"\(\s*\*", window):
        return False
    return re.search(r"\w\s*\(", window) is not None


def patch_pro(path, names):
    """zsh's generated `.pro` re-declares the file's statics; patch it too.

    zsh runs makepro.awk over every source and writes Src/jobs.pro, which
    jobs.c includes -- and which carries `static struct rusage child_usage;`
    for a variable jobs.c itself has just defined. Converting only the
    definition is not a silent wrong-memory read here, because both live in the
    one translation unit and clang says so:

        error: thread-local declaration of 'child_usage' follows
               non-thread-local declaration

    That is a nicer failure than the cross-TU one, but it still has to be
    fixed, and there is no point making the human do 600 of them. The .pro is
    a generated file that is checked in (deps/zsh is a CONFIGURED tree), so
    editing it is as legitimate as editing the .c -- and `make prep` would undo
    both together.
    """
    pro = path[:-2] + ".pro" if path.endswith(".c") else None
    if not pro or not os.path.exists(pro):
        return
    wanted = {n.rsplit(".", 1)[-1] for n in names}
    with open(pro, encoding="utf-8", errors="surrogateescape") as fh:
        lines = fh.read().split("\n")
    touched = False
    for i, line in enumerate(lines):
        m = DEFN.match(line)
        if not m:
            continue
        # A prototype is `static void foo _((int));` -- the name is followed by
        # a paren. A variable declarator is followed by `[`, `;`, `,` or `=`.
        # Function POINTERS are variables and look like `static void
        # (*f)(void);`, so they are matched by their own shape.
        for n in wanted:
            # `static int getargspec(int argc, int marg, int evset);` is a
            # prototype, and `evset` is also the name of a static in the same
            # file -- matched inside the parameter list, it put `__thread` on a
            # function and the file stopped compiling. Same guard, same reason,
            # as in_parens() above.
            if in_parens(line, n) or is_function_decl(line):
                continue
            if (re.search(r"\b" + re.escape(n) + r"\b\s*(\[[^\]]*\])*\s*[;=,]", line)
                    or re.search(r"\(\s*\*\s*" + re.escape(n) + r"\s*\)", line)):
                lines[i] = (m.group("pre") + "static __thread" + m.group("post")
                            + line[m.end():])
                touched = True
                break
    if touched:
        with open(pro, "w", encoding="utf-8", errors="surrogateescape") as fh:
            fh.write("\n".join(lines))


def main():
    archive = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build", "libbash.a")
    found = statics(archive)
    total = sum(len(v) for v in found.values())
    patched, missed = 0, []
    for obj, names in sorted(found.items()):
        path = source_for(obj)
        if path is None:
            missed.append(f"{obj}: no source (symbols: {sorted(names)[:3]})")
            continue
        hit = patch(path, names)
        patch_pro(path, names)
        patched += len(hit)
        for n in sorted(names - hit):
            missed.append(f"{os.path.relpath(path, REPO)}: {n} not found textually")
    print(f"bash-tls-fix-statics: {patched}/{total} statics made thread-local",
          file=sys.stderr)
    for m in missed:
        print(f"  MISSED {m}", file=sys.stderr)
    return 1 if missed else 0


if __name__ == "__main__":
    sys.exit(main())
