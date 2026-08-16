#!/usr/bin/env python3
"""Finish the TLS rewrite wherever the compiler can point at the gap.

tools/bash-tls-rewrite.py works from clang's AST over the compiled translation
units, so it cannot reach the .def files: what the compiler sees is the C that
mkbuiltins generates from them into the build directory, and rewriting that
would be undone by the next build.

It does not need to reach them by itself, because the compiler does the mapping.
mkbuiltins emits #line directives pointing back at the .def, so a declaration
that disagrees with a now-thread-local definition is reported AT THE .def FILE
AND LINE. This walks those diagnostics and inserts the missing `__thread`.

Only same-TU mismatches are diagnosed this way, which is why
tools/check-bash-tls.py exists as well: across translation units the mismatch is
silent, links cleanly, and reads the wrong memory.
"""

import re
import subprocess
import sys
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Two directions, and they want opposite locations. When a NON-thread-local
# declaration follows a thread-local one, the reported line is the one to fix.
# When a THREAD-LOCAL one follows a non-thread-local one, the reported line is
# already right and the fix belongs at the "previous declaration is here" note.
DIAG_HERE = re.compile(
    r"^(?P<file>\S+):(?P<line>\d+):\d+: error: non-thread-local declaration of "
    r"'(?P<name>[^']+)' follows thread-local declaration")
DIAG_THERE = re.compile(
    r"^(?P<file>\S+):(?P<line>\d+):\d+: error: thread-local declaration of "
    r"'(?P<name>[^']+)' follows non-thread-local declaration")
NOTE_PREV = re.compile(
    r"^(?P<file>\S+):(?P<line>\d+):\d+: note: previous declaration is here")


BUILD = os.path.join(REPO, "build")
VENDOR = os.path.realpath(os.path.join(REPO, "deps", "bash"))
generated = set()


def record(sites, m):
    """Note a site, resolving the compiler's relative paths against the build
    directory and refusing to touch generated sources: build/shopt.c and
    friends are rewritten from builtins/*.def on every build, so an edit there
    would vanish and the diagnostic would come back forever."""
    path = os.path.realpath(os.path.join(BUILD, m.group("file")))
    if not path.startswith(VENDOR):
        generated.add(path)
        return
    sites.setdefault(path, set()).add(int(m.group("line")))


def build_errors():
    r = subprocess.run(["ninja", "-C", os.path.join(REPO, "build"), "-k", "0"],
                       capture_output=True, text=True)
    sites = {}
    want_note = False
    for line in (r.stdout + r.stderr).splitlines():
        line = line.strip()
        m = DIAG_HERE.match(line)
        if m:
            record(sites, m)
            want_note = False
            continue
        if DIAG_THERE.match(line):
            want_note = True
            continue
        if want_note:
            m = NOTE_PREV.match(line)
            if m:
                record(sites, m)
                want_note = False
    return sites, r.returncode


def patch(sites):
    changed = 0
    for path, lines in sites.items():
        with open(path, "r", encoding="utf-8", errors="surrogateescape") as fh:
            text = fh.read().split("\n")
        # One insertion per LINE, not per name: `extern int breaking, continuing;`
        # is reported once for each declarator and needs one __thread.
        for ln in sorted(lines, reverse=True):
            i = ln - 1
            if i >= len(text):
                continue
            stripped = text[i].lstrip()
            if stripped.startswith("__thread "):
                continue
            indent = text[i][:len(text[i]) - len(stripped)]
            text[i] = indent + "__thread " + stripped
            changed += 1
        with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
            fh.write("\n".join(text))
    return changed


def main():
    for round_no in range(1, 21):
        sites, rc = build_errors()
        if generated:
            print("declarations in GENERATED sources, which must be fixed in "
                  "their .def instead:", file=sys.stderr)
            for g in sorted(generated):
                print(f"  {g}", file=sys.stderr)
        if not sites:
            print(f"round {round_no}: no mismatched declarations left "
                  f"(build rc={rc})", file=sys.stderr)
            return 0 if rc == 0 else 1
        n = patch(sites)
        print(f"round {round_no}: patched {n} declarations in "
              f"{len(sites)} .def files", file=sys.stderr)
        if n == 0:
            print("no progress; stopping", file=sys.stderr)
            return 1
    print("too many rounds", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
