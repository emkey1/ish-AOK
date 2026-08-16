#!/usr/bin/env python3
"""Make bash's remaining shared globals thread-local, by name, everywhere.

The companion tools each miss this set for their own reason.
bash-tls-rewrite.py drives off clang's JSON AST, whose file attribution drifts
(see the comment there) so it silently skipped a batch. bash-tls-fix-statics.py
only looks at non-external symbols. check-bash-tls.py only reports symbols two
files DISAGREE about, and a variable no file made thread-local is something
every file agrees on -- consistently shared, consistently wrong.

What was left behind mattered. o_options is one of the tables aok_fix_*() fills
in per thread: shared, every shell writes its own variables' addresses into the
one table and the last writer wins, which is precisely the race this conversion
exists to remove. xpg_echo, localvar_unset and shell_function_defs are ordinary
per-shell settings that one shell would have changed under another.

So this takes the names from `nm` -- external, mutable, not thread-local -- and
inserts __thread at every file-scope declaration of them in the vendored tree,
definitions and externs alike. A declaration missed in one header is not a
compile error, it is a silent wrong-memory read, which is why it goes by name
across every file rather than per translation unit.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VENDOR = os.path.join(REPO, "deps", "bash")

# Deliberately shared. Each of these is something two live shells can see each
# other through, kept that way for a reason that is worth more than the
# isolation would be.
ALLOW = {
    # readline's keymaps cross-reference each other in static initialisers
    # (emacs_standard_keymap's Control-x entry IS emacs_ctlx_keymap), so they
    # cannot be thread-local without a fixup pass over all of them. The visible
    # consequence is narrow and interactive: `bind` in one shell is seen by
    # another. Noted in docs/bash_native_plan.md.
    "emacs_standard_keymap", "emacs_meta_keymap", "emacs_ctlx_keymap",
    "vi_movement_keymap", "vi_insertion_keymap", "builtin_keymap_names",
    # The set of builtins bash was compiled with -- identical in every shell.
    # `enable`/`disable` flip flags inside it, which is shared; see builtins.h.
    "static_shell_builtins", "shell_builtins", "num_shell_builtins",
    "current_builtin",
    # Written once, at startup, with the same value by every shell.
    "rl_library_version", "rl_readline_name",
    # Read-only lookup tables that only lack `const` upstream.
    "default_prefixes", "default_suffixes",
    "posix_collsyms", "posix_collwcsyms",

    # ---- smallclue (build/libsmallclue.a) ----
    # Its applets are native programs for the same reason bash is, so two live
    # ones share every global. The surface is far smaller: 36 mutable symbols
    # against bash's ~1500, and 24 already thread-local.
    #
    # Read-only tables that only lack `const`, same class as the bash ones above.
    "kAwkBuiltins", "kSmallclueAppletCount",
    "markdownLooksLikeNewsMetaLine.months", "smallclueBaudLabel.unknown",
    "smallclueDfFormatSize.suffixes", "smallclueDnsRcodeName.rcodeNames",
    "smallclueTouchParseDashD.formats",
    # nextvi's session registry is shared ON PURPOSE and carries its own lock
    # (s_nextvi_sessions_lock). Making it per-thread would give every task its
    # own empty registry and quietly defeat session sharing, which is the point
    # of it.
    "s_nextvi_sessions", "s_nextvi_session_count", "s_nextvi_session_cap",
    "s_nextvi_sessions_lock",

}

# Undecided: the fixers leave these alone, and the gate KEEPS REPORTING them.
#
# This is a third state on purpose. ALLOW means "shared, and that is right";
# putting an open question in there would record a decision nobody made, and the
# gate would go quiet on it. Leaving it out of ALLOW entirely is worse the other
# way -- the fixers would convert it, which is also a decision nobody made. So:
# skipped by the fixers, still reported by the gate, until someone answers the
# question written next to it.
DEFER = {
    # Written from a signal handler (deps/smallclue/src/core.c:6263). Whether
    # __thread is even CORRECT depends on which thread a native program's
    # signals are delivered on. If the handler does not run on the applet's own
    # thread, making this thread-local breaks it rather than fixes it.
    "g_pager_sigwinch_received",
    # A clipboard is arguably a shared system resource, so sharing may well be
    # right -- but it is unguarded, and concurrent applets turn that into a real
    # race. Needs a decision between "share it with a lock" and "per-applet".
    "g_clipboard_data", "g_clipboard_init", "g_clipboard_len",
}

# What the fixers skip. The gate skips only ALLOW.
SKIP = ALLOW | DEFER

SRC = (".c", ".h", ".def", ".y")


def shared_externals(archive):
    names = set()
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
                # `external` is not adjacent to the section: a common symbol
                # prints as `(common) (alignment 2^3) external _name`. Matching
                # them as one string quietly skipped every tentative definition
                # -- tempvar_list, pcomp_curcmd, rl_executing_keymap and the
                # rest -- and reported "nothing shared" while seven were.
                if not re.search(r"\((__DATA,__(data|bss|common)|common)\)", line):
                    continue
                if " external" not in line or "non-external" in line:
                    continue
                sym = line.split()[-1]
                if "$tlv$" in sym or not sym.startswith("_"):
                    continue
                name = sym[1:]
                if name not in SKIP:
                    names.add(name)
    return names


def vendor_for(archive):
    """Which vendored tree an archive's sources live in.

    libbash.a -> deps/bash, libsmallclue.a -> deps/smallclue. Walking the wrong
    one finds no declarations and reports every symbol as MISSED, which looks
    like a parsing bug rather than the wrong root.
    """
    stem = os.path.basename(archive)
    if stem.startswith("lib") and stem.endswith(".a"):
        cand = os.path.join(REPO, "deps", stem[3:-2])
        if os.path.isdir(cand):
            return cand
    return VENDOR


def sources(root=None):
    for root, _, files in os.walk(root or VENDOR):
        if os.sep + ".git" in root:
            continue
        for f in files:
            if f.endswith(SRC):
                yield os.path.join(root, f)


def declares(line, name):
    """True if this file-scope line declares `name`.

    Narrow on purpose. The name must be a declarator: it is followed by
    optional array brackets and then `;`, `=` or `,`, and nothing opens a
    paren before it -- `int f (int xpg_echo)` names a parameter and
    `static int g PARAMS((int))` is a prototype, and putting __thread on either
    does not compile.
    """
    if line.startswith(("#", "//", " ", "\t", "}", "*")) or "__thread" in line:
        return False
    m = re.search(r"\b" + re.escape(name) + r"\b\s*(\[[^\]]*\])*\s*[;=,]", line)
    if not m:
        return False
    head = line[:m.start()]
    if head.count("(") > head.count(")"):
        return False
    # A typedef or a struct member is not a variable.
    return not re.match(r"\s*(typedef|return|case)\b", line)


def main():
    archive = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build", "libbash.a")
    names = shared_externals(archive)
    if not names:
        print("bash-tls-fix-externs: nothing shared", file=sys.stderr)
        return 0
    print(f"bash-tls-fix-externs: {len(names)} shared externals: "
          f"{' '.join(sorted(names))}", file=sys.stderr)

    hits, changed = {n: 0 for n in names}, 0
    for path in sources(vendor_for(archive)):
        with open(path, encoding="utf-8", errors="surrogateescape") as fh:
            lines = fh.read().split("\n")
        touched = False
        for i, line in enumerate(lines):
            for n in names:
                if n in line and declares(line, n):
                    lines[i] = "__thread " + line
                    hits[n] += 1
                    touched = True
                    break
        if touched:
            changed += 1
            with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
                fh.write("\n".join(lines))

    for n in sorted(names):
        if hits[n] == 0:
            print(f"  MISSED {n}: no declaration found", file=sys.stderr)
    print(f"  patched {sum(hits.values())} declarations in {changed} files",
          file=sys.stderr)
    return 1 if any(v == 0 for v in hits.values()) else 0


if __name__ == "__main__":
    sys.exit(main())
