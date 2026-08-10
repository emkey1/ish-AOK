#!/usr/bin/env python3
"""Generate the aokfs embedded-file table from a manifest.

Reads a manifest listing files to expose under /AOK/tests, reads each file's
bytes from the source directory, and emits a C include defining:

    struct aokfs_gen_file { const char *path; const char *data;
                            unsigned size; unsigned mode; };
    static const char aokfs_gen_data_N[] = "...";
    static const struct aokfs_gen_file aokfs_gen_files[] = { ... };
    #define AOKFS_GEN_FILE_COUNT (...)

The embedding is byte-exact: file content is reproduced verbatim (size is the
real byte count, not strlen). fs/aok.c consults this table so any edit to a
listed file -- or adding a new manifest line -- propagates on the next build.

Usage: gen-aokfs.py <manifest> <source-dir> <output.inc>
"""

import os
import re
import sys


def parse_manifest(manifest_path):
    """Return a list of (filename, mode) from the manifest."""
    entries = []
    with open(manifest_path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            name = parts[0]
            if len(parts) > 1:
                mode = int(parts[1], 8)
            else:
                mode = 0o555 if name.endswith(".sh") else 0o444
            entries.append((name, mode))
    return entries


def escape_line(seg):
    """Escape one line of bytes into a C string-literal body (no quotes)."""
    out = []
    for b in seg:
        if b == 0x5C:        # backslash
            out.append("\\\\")
        elif b == 0x22:      # double quote
            out.append("\\\"")
        elif b == 0x09:      # tab
            out.append("\\t")
        elif 0x20 <= b <= 0x7E:
            out.append(chr(b))
        else:
            # 3-digit octal: unambiguous (C reads at most 3 octal digits), so a
            # following literal digit cannot extend the escape.
            out.append("\\%03o" % b)
    return "".join(out)


def emit_data(data, var):
    """Emit a `static const char <var>[] = ...;` reproducing data byte-exact."""
    lines = data.split(b"\n")
    pieces = ["static const char %s[] =" % var]
    if data == b"":
        pieces.append('    "";')
        return "\n".join(pieces)
    for i, seg in enumerate(lines):
        is_last = i == len(lines) - 1
        if is_last and seg == b"":
            # File ended with a newline; the previous segment already carried
            # its trailing \n. Nothing to emit for the empty final segment.
            continue
        body = escape_line(seg)
        suffix = "" if is_last else "\\n"
        pieces.append('    "%s%s"' % (body, suffix))
    pieces[-1] += ";"
    return "\n".join(pieces)


def check_runner_requirements(entries, source_dir):
    """Fail the build if setup-regressions.sh needs a file the manifest omits.

    The runner calls need_file for each source it requires and exits on the
    first one missing, so a single unlisted test does not skip that test -- it
    stops the ENTIRE on-device suite before a line of it runs. That happened
    twice: inaddr_any_iface.c (added by 4102fc1d) and x86/port_io_gpf.c, each
    silently disabling the primary release gate until someone tried to run it.
    Nothing else notices, because the manifest and the runner are edited in
    different places and only meet on a device.

    Returns an error string, or None when the two agree.
    """
    runner = os.path.join(source_dir, "setup-regressions.sh")
    if not os.path.exists(runner):
        return None  # not the tests manifest (tools/docs use this too)

    with open(runner, "r") as f:
        text = f.read()
    # Match indented calls too: the arch-specific ones live inside conditionals.
    required = set(re.findall(r"\bneed_file\s+(\S+)", text))
    listed = {name for name, _mode in entries}
    missing = sorted(required - listed)
    if not missing:
        return None
    return (
        "%s requires %d file(s) that %s does not embed, which would leave the "
        "on-device suite unable to start:\n%s\nAdd them to the manifest.\n"
        % (runner, len(missing), "the manifest",
           "".join("    %s\n" % m for m in missing)))


def main():
    if len(sys.argv) not in (4, 5, 6):
        sys.stderr.write(
            "usage: gen-aokfs.py <manifest> <source-dir> <output.inc> "
            "[dest-prefix] [symbol-suffix]\n")
        return 2

    manifest_path, source_dir, output_path = sys.argv[1:4]
    # dest-prefix is the directory the files are served under (default /tests).
    # symbol-suffix keeps the emitted C symbols unique so multiple generated
    # tables (e.g. tests and tools) can coexist in one translation unit.
    dest_prefix = sys.argv[4] if len(sys.argv) > 4 else "/tests"
    sym_suffix = sys.argv[5] if len(sys.argv) > 5 else ""
    entries = parse_manifest(manifest_path)

    err = check_runner_requirements(entries, source_dir)
    if err:
        sys.stderr.write(err)
        return 1

    out = []
    out.append("/* Auto-generated by tools/gen-aokfs.py from %s. Do not edit. */"
               % os.path.basename(manifest_path))
    out.append("")
    out.append("#ifndef AOKFS_GEN_FILE_STRUCT_DEFINED")
    out.append("#define AOKFS_GEN_FILE_STRUCT_DEFINED")
    out.append("struct aokfs_gen_file {")
    out.append("    const char *path;")
    out.append("    const char *data;")
    out.append("    unsigned size;")
    out.append("    unsigned mode;")
    out.append("};")
    out.append("#endif")
    out.append("")

    table = []
    for idx, (name, mode) in enumerate(entries):
        src = os.path.join(source_dir, name)
        with open(src, "rb") as f:
            data = f.read()
        var = "aokfs_gen_data%s_%d" % (sym_suffix, idx)
        out.append(emit_data(data, var))
        out.append("")
        table.append('    { "%s/%s", %s, %uu, 0%ou },'
                     % (dest_prefix, name, var, len(data), mode))

    out.append("static const struct aokfs_gen_file aokfs_gen_files%s[] = {"
               % sym_suffix)
    out.extend(table)
    out.append("};")
    out.append("")
    out.append("#define AOKFS_GEN_FILE_COUNT%s "
               "(sizeof(aokfs_gen_files%s) / sizeof(aokfs_gen_files%s[0]))"
               % (sym_suffix, sym_suffix, sym_suffix))
    out.append("")

    text = "\n".join(out)
    # Only rewrite if changed, to avoid needless recompiles.
    if os.path.exists(output_path):
        with open(output_path, "r") as f:
            if f.read() == text:
                return 0
    with open(output_path, "w") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
