#!/usr/bin/env python3
"""Make zsh's address-taking static tables thread-local, by deferring their
initialisers to first use.

The other three tools put `__thread` on declarations. This one exists because
of what that then breaks: a table whose initialiser contains `&some_variable`
stops compiling the moment that variable becomes thread-local, because the
address of a thread-local is not a compile-time constant.

    static __thread struct paramdef partab[] = {
        PARAMDEF(".sh.edchar", PM_SCALAR|PM_SPECIAL, &sh_edchar, ...),
    };
    ../deps/zsh/Src/Modules/ksh93.c:120:5: error: initializer element is not
                                                  a compile-time constant

bash hit the same wall and answered it with tools/bash-tls-fix-tables.py, which
converts seven hand-listed tables into per-thread `aok_fix_*()` functions
called from the glue at shell start. zsh has thirty-odd of them -- every module
has a `module_features` pointing at its own `bintab` and `partab` -- and no
single place that runs before all of them, since a module's tables are reached
through its `setup_`/`features_`/`enables_` entry points and those are called
per module, per shell, in an order module.c chooses.

So this defers instead of scheduling: the storage stays thread-local, the
initialiser moves into a function that runs on first touch, and the name
becomes a macro for the dereferenced accessor.

    typedef struct paramdef aok_tt_partab[9];
    static __thread aok_tt_partab aok_tv_partab;
    static __thread char aok_ti_partab;
    static aok_tt_partab *aok_tf_partab(void) { ...; return &aok_tv_partab; }
    #define partab (*aok_tf_partab())

Returning a POINTER TO ARRAY rather than a pointer to the first element is the
part that matters, and it is not stylistic. `#define partab aok_tf_partab()`
would turn `sizeof(partab)/sizeof(*partab)` -- which is how every module tells
its features struct how long its table is -- into `sizeof(pointer)/sizeof(elem)`,
silently reporting a nine-entry table as zero entries and registering none of
its builtins. Dereferencing a pointer-to-array gives back an lvalue of array
type, so sizeof, indexing and decay all behave exactly as they did before.

The element count is counted from the source text, which is the one thing here
that could be wrong quietly -- so the generated function carries a
_Static_assert against the initialiser's real length. A miscount is a build
failure rather than a buffer overrun.

Usage: run it with no arguments after the other tools. It builds, reads clang's
own "initializer element is not a compile-time constant" errors, transforms the
declaration each one sits inside, and repeats until the class is gone.
"""

import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ERROR = re.compile(r"^(\S+?):(\d+):\d+: error: initializer element is not a "
                   r"compile-time constant", re.M)


def build():
    p = subprocess.run(["ninja", "-C", os.path.join(REPO, "build")],
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def strip_noise(text):
    """The same text with comments and string/char literals blanked out.

    Counting braces and commas over the raw source counts the ones inside
    `BUILTIN("unlimit", 0, ...)`'s strings and inside the comment that says
    `/* SPECIALPMDEF(".sh.value", 0, NULL, NULL, NULL), */`, and a table whose
    entries are macro calls full of both is exactly what this walks.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(" " * (j - i))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def entry_count(body):
    """Top-level entries in a brace-initialiser body."""
    clean = strip_noise(body)
    depth, commas, seen = 0, 0, False
    for ch in clean:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "," and depth == 0:
            commas += 1
        elif not ch.isspace():
            seen = True
    if not seen:
        return 0
    # A trailing comma means the last comma closes an entry rather than
    # separating two, so it must not add one. zsh writes both forms.
    tail = clean.rstrip()
    return commas if tail.endswith(",") else commas + 1


DECL = re.compile(r"^static\s+__thread\s+(?P<type>.+?)\s*(?P<star>\**)"
                  r"(?P<name>[A-Za-z_]\w*)\s*(?P<arr>\[\s*\])?\s*=", re.S)


def find_decl(text, off):
    """The file-scope `static __thread ... = ...;` declaration containing OFF.

    Backwards to the nearest `static __thread` at column 0, forwards to the
    `;` that closes it at depth 0.
    """
    # The last declaration that begins at or before OFF. Not rfind: clang
    # reports the error on the declaration's OWN first line as often as on a
    # later one, and rfind's `end` bound requires the whole needle to fit
    # before OFF -- so the declaration being complained about was skipped and
    # the previous one, already converted, was picked up instead.
    start = -1
    for m in re.finditer(r"^static\s+__thread\b", text, re.M):
        if m.start() > off:
            break
        start = m.start()
    if start < 0:
        return None
    clean = strip_noise(text)
    depth, i, n = 0, start, len(text)
    while i < n:
        ch = clean[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == ";" and depth == 0:
            break
        i += 1
    if i >= n or i < off:
        return None
    return start, i + 1


def transform(path, off):
    with open(path, encoding="utf-8", errors="surrogateescape") as fh:
        text = fh.read()
    span = find_decl(text, off)
    if span is None:
        return None
    start, end = span
    decl = text[start:end]
    m = DECL.match(decl)
    if not m:
        return None
    # The type must look like a type. DECL's `.+?` spans newlines, so if the
    # `static __thread` this landed on is a FUNCTION -- which happens while the
    # other tools have a bad conversion still outstanding -- the match runs
    # through the function's body to the first `=` inside it and rewrites the
    # function into nonsense. Ask for the shape instead of trusting the walk:
    # a declarator's type has no parentheses, braces or semicolons in it.
    if re.search(r"[(){};]", m.group("type")):
        return None
    name = m.group("name")
    ctype = (m.group("type") + " " + m.group("star")).strip()
    init = decl[m.end():].rstrip()
    assert init.endswith(";")
    init = init[:-1].strip()

    if m.group("arr"):
        body = init[1:-1] if init.startswith("{") else init
        count = entry_count(body)
        if count == 0:
            return None
        new = (
            f"/* AOK: {name}'s initialiser takes the address of a thread-local,\n"
            f"   so it cannot be a static initialiser any more. See\n"
            f"   tools/zsh-tls-fix-tables.py. */\n"
            f"typedef {ctype} aok_tt_{name}[{count}];\n"
            f"static __thread aok_tt_{name} aok_tv_{name};\n"
            f"static __thread char aok_ti_{name};\n"
            f"static aok_tt_{name} *aok_tf_{name}(void) {{\n"
            f"    if (!aok_ti_{name}) {{\n"
            f"        {ctype} aok_tmp[] = {init};\n"
            f"        _Static_assert(sizeof(aok_tmp)/sizeof(aok_tmp[0]) == {count},\n"
            f"                       \"{name}: zsh-tls-fix-tables miscounted\");\n"
            f"        memcpy(aok_tv_{name}, aok_tmp, sizeof(aok_tmp));\n"
            f"        aok_ti_{name} = 1;\n"
            f"    }}\n"
            f"    return &aok_tv_{name};\n"
            f"}}\n"
            f"#define {name} (*aok_tf_{name}())\n")
    else:
        new = (
            f"/* AOK: see tools/zsh-tls-fix-tables.py. */\n"
            f"static __thread {ctype} aok_tv_{name};\n"
            f"static __thread char aok_ti_{name};\n"
            f"static {ctype} *aok_tf_{name}(void) {{\n"
            f"    if (!aok_ti_{name}) {{\n"
            f"        {ctype} aok_tmp = {init};\n"
            f"        aok_tv_{name} = aok_tmp;\n"
            f"        aok_ti_{name} = 1;\n"
            f"    }}\n"
            f"    return &aok_tv_{name};\n"
            f"}}\n"
            f"#define {name} (*aok_tf_{name}())\n")

    with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.write(text[:start] + new + text[end:])
    return name


def main():
    for _ in range(40):
        rc, out = build()
        errs = {}
        for m in ERROR.finditer(out):
            f = m.group(1)
            path = f if os.path.isabs(f) else os.path.normpath(
                os.path.join(REPO, "build", f))
            # One declaration can produce several errors; take the first line
            # in each file and let the next round find the rest.
            errs.setdefault(path, int(m.group(2)))
        if not errs:
            print("zsh-tls-fix-tables: no non-constant-initialiser errors left",
                  file=sys.stderr)
            return 0 if rc == 0 else 2
        done = 0
        for path, line in errs.items():
            with open(path, encoding="utf-8", errors="surrogateescape") as fh:
                off = sum(len(l) for l in fh.readlines()[:line - 1])
            name = transform(path, off)
            if name:
                print(f"  {os.path.relpath(path, REPO)}: deferred {name}",
                      file=sys.stderr)
                done += 1
            else:
                print(f"  STUCK {os.path.relpath(path, REPO)}:{line}",
                      file=sys.stderr)
        if done == 0:
            return 1
    return 1


if __name__ == "__main__":
    sys.exit(main())
