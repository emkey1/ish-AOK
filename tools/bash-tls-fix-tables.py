#!/usr/bin/env python3
"""Convert bash's option tables from static addresses to runtime fixups.

The last thing in the way of thread-local globals. A table like

    static struct { char *name; int *value; } shopt_vars[] = {
      { "autocd", &autocd, NULL },
    #if defined (ARRAY_VARS)
      { "assoc_expand_once", &expand_once_flag, set_assoc_expand },
    #endif

stops compiling the moment `autocd` becomes __thread, because the address of a
thread-local is not a compile-time constant. Every such table has the same
shape: a list of entries, some of them behind #if, each naming one variable.

So the address comes out of the initialiser and goes into a fixup function that
runs once per thread. The generated function copies the table's preprocessor
lines VERBATIM, which is what keeps the indices in step: an entry compiled out
by #if is also skipped by the fixup, so the two can never disagree about which
slot belongs to which variable.

Called once per shell from kernel/bash_glue.c, before bash's main -- these are
pointer fixups with no dependencies, so the earliest possible moment is also
the safest one.
"""

import re
import sys

# (file, table variable, struct field, braced)
#
# `struct field` is a member name, or a {slot: member} map when the table has
# more than one pointer member and different entries fill different ones.
# `slot` counts comma-separated initialisers inside the entry's braces.
#
# `braced` says whether each entry is written { like, this }. posix_vars in
# general.c is a one-member struct written with the braces elided, so its
# entries are counted differently -- getting that wrong would shift every index
# in its fixup by an unknown amount.
TABLES = [
    ("builtins/shopt.def", "shopt_vars", "value", True),
    ("builtins/set.def", "o_options", "variable", True),
    ("flags.c", "shell_flags", "value", True),
    ("general.c", "posix_vars", "posix_mode_var", False),
    # long_args is the one table with TWO pointer members: entries tagged Int
    # carry &an_int in slot 2, entries tagged Charp carry &a_char_p in slot 3.
    # A single field name here filled int_value for both, so --rcfile/--init-file
    # left char_value NULL -- and bash dispatches on the type tag, not on which
    # pointer is set, so it wrote through that NULL. Map the slot to the member.
    ("shell.c", "long_args", {2: "int_value", 3: "char_value"}, True),
    ("lib/readline/bind.c", "boolean_varlist", "value", True),
    ("lib/readline/terminal.c", "tc_strings", "tc_value", True),
]

def slot_of(line, pos):
    """Which comma-separated initialiser of a braced entry contains `pos`."""
    depth, slot, started = 0, 0, False
    for i, ch in enumerate(line):
        if i >= pos:
            break
        if ch == "{":
            if not started:
                started, depth = True, 0
                continue
            depth += 1
        elif ch == "}":
            depth -= 1
        elif ch == "(" or ch == "[":
            depth += 1
        elif ch == ")" or ch == "]":
            depth -= 1
        elif ch == "," and started and depth == 0:
            slot += 1
    return slot


CPP = re.compile(r"^\s*#\s*(if|ifdef|ifndef|else|elif|endif)\b")
ADDR = re.compile(r"&\s*([A-Za-z_]\w*)")


def convert(path, table, field, braced):
    with open(path, encoding="utf-8", errors="surrogateescape") as fh:
        src = fh.read()
    lines = src.split("\n")

    start = None
    for i, line in enumerate(lines):
        if re.search(r"\b" + re.escape(table) + r"\s*\[\s*\]\s*=", line):
            start = i
            break
    if start is None:
        return None, f"{path}: no table {table}[]"

    # A const table cannot be fixed up at runtime; the const was only
    # reasonable while the addresses were constants. Strip it ON THE TABLE'S
    # OWN LINE ONLY -- an earlier attempt searched backwards for a `const` and
    # found one on a struct MEMBER (`const char * const name;`), quietly
    # changing the type of the field instead of the table.
    # These tables are written `static const struct { ... } name[] = {`, so the
    # const that qualifies the ARRAY sits on the line that opens the anonymous
    # struct, several lines up. Walk back to the `{` that the `}` on the table's
    # line closes, and strip it there -- and only there. An earlier attempt
    # searched backwards for any `const` and hit a struct MEMBER
    # (`const char * const name;`), silently changing a field's type instead.
    if lines[start].lstrip().startswith("}"):
        # Start ABOVE the table's own line: it contains both the `}` that closes
        # the struct and the `{` that opens the initialiser, so counting it
        # balances to zero and the search stops where it began.
        bal = 1
        for j in range(start - 1, -1, -1):
            bal += lines[j].count("}") - lines[j].count("{")
            if bal <= 0:
                if re.search(r"\bconst\b", lines[j]):
                    lines[j] = re.sub(r"\bconst\s+", "", lines[j], count=1)
                break
    elif re.search(r"\bconst\b", lines[start]):
        lines[start] = re.sub(r"\bconst\s+", "", lines[start], count=1)

    # Count braces on the start line only AFTER the `=`. These tables are
    # usually written `} shopt_vars[] = {`, where the leading `}` closes the
    # anonymous struct -- counting it made the table look like it ended on its
    # first entry, and the tool reported nothing to do.
    depth, end = 0, None
    for i in range(start, len(lines)):
        text = lines[i]
        if i == start:
            text = text[text.index("=") + 1:]
        depth += text.count("{") - text.count("}")
        if depth == 0 and i > start and "}" in text:
            end = i
            break
    if end is None:
        return None, f"{path}: unterminated {table}[]"

    # The initialiser's opening brace is not always on the table's own line --
    # `static struct _tc_string tc_strings[] =` puts it on the next one, and
    # `} posix_vars[] = ` on the one after that. Counting that line as an entry
    # shifted every index by one and wrote a pointer past the end of the array,
    # which is a wild call, not a wrong option.
    first = start
    if "{" not in lines[start][lines[start].index("=") + 1:]:
        while first + 1 < end and lines[first + 1].strip() != "{":
            first += 1
        first += 1

    body, fixups, idx_expr = [], [], "aok_i"
    for i in range(first + 1, end):
        line = lines[i]
        if CPP.match(line):
            body.append(line)
            fixups.append(line)
            continue
        m = ADDR.search(line)
        stripped = line.strip()
        if braced:
            entries = line.count("{")
        else:
            entries = 1 if (stripped and not stripped.startswith("/*")
                            and not stripped.startswith("*")) else 0
        if m and entries:
            name = m.group(1)
            body.append(line[:m.start()] + "0" + line[m.end():])
            member = field[slot_of(line, m.start())] if isinstance(field, dict) else field
            fixups.append(f"  {table}[{idx_expr}].{member} = &{name};")
        else:
            body.append(line)
        if entries:
            fixups.append(f"  {idx_expr}++;" if not (m and entries) else f"  {idx_expr}++;")

    if not any("&" in f for f in fixups):
        return None, f"{path}: nothing to fix in {table}[]"

    fn = (f"\n/* iSH-AOK: {table}[] pointed at variables that are thread-local now,\n"
          f"   so its addresses are no longer compile-time constants. The entries\n"
          f"   below carry the same #if lines as the table, which is what keeps the\n"
          f"   indices in step with it. Generated by tools/bash-tls-fix-tables.py. */\n"
          f"void\naok_fix_{table} ()\n{{\n"
          f"  static __thread int aok_done;\n"
          f"  int {idx_expr} = 0;\n\n"
          f"  if (aok_done)\n    return;\n  aok_done = 1;\n\n"
          + "\n".join(fixups) + "\n}\n")

    out = lines[:first + 1] + body + lines[end:end + 1] + [fn] + lines[end + 1:]
    with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.write("\n".join(out))
    return f"aok_fix_{table}", None


def main():
    made, problems = [], []
    for path, table, field, braced in TABLES:
        name, err = convert(path, table, field, braced)
        if err:
            problems.append(err)
        else:
            made.append(name)
            print(f"  {path}: {name}()", file=sys.stderr)
    for p in problems:
        print(f"  PROBLEM {p}", file=sys.stderr)
    print("\n".join(f"extern void {m} PARAMS((void));" for m in made))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
