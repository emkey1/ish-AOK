#!/usr/bin/env python3
"""Extract every member of a Mach-O ar archive, duplicates included.

    ar-extract.py <archive> <dir>

Why not `ar x`. It writes each member under its own name, so two members that
share a name leave one file -- silently, and the archive is then short a
translation unit that the link will not miss until something calls into it.
Real archives do this: tree-sitter-md builds two parsers, a block one and an
inline one, and cc-rs derives an object name that collides for both.

The format is simple enough that parsing it is smaller than working around
`ar`'s ways of not doing this: an 8-byte magic, then 60-byte headers each
followed by their data padded to an even offset. BSD (which is what Apple's
tools write) puts a long name in the first `n` bytes of the DATA and writes
`#1/n` in the name field; the GNU form with a `//` string table is handled too,
so this does not depend on which toolchain produced the archive.
"""
import os
import sys


def members(blob):
    if not blob.startswith(b"!<arch>\n"):
        raise SystemExit("ar-extract: not an ar archive")
    pos = 8
    long_names = b""
    while pos + 60 <= len(blob):
        header = blob[pos:pos + 60]
        if header[58:60] != b"`\n":
            raise SystemExit("ar-extract: bad member header at offset %d" % pos)
        name = header[0:16].rstrip()
        size = int(header[48:58].decode().strip())
        pos += 60
        data = blob[pos:pos + size]
        pos += size + (size & 1)          # members are padded to an even offset

        if name == b"//":                 # GNU long-name table
            long_names = data
            continue
        if name.startswith(b"#1/"):       # BSD long name, inline in the data
            n = int(name[3:].decode())
            yield data[:n].rstrip(b"\0").decode(), data[n:]
            continue
        if name.startswith(b"/") and name[1:].isdigit():
            off = int(name[1:].decode())
            end = long_names.index(b"/\n", off)
            yield long_names[off:end].decode(), data
            continue
        if name in (b"/", b"/SYM64/", b"__.SYMDEF", b"__.SYMDEF SORTED"):
            continue                      # the symbol index, not a member
        yield name.rstrip(b"/").decode(), data


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    archive, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    with open(archive, "rb") as f:
        blob = f.read()

    seen = {}
    count = 0
    for name, data in members(blob):
        name = os.path.basename(name)
        # Duplicates keep both, distinguished by order of appearance. The
        # suffix goes before the extension so the result is still a .o.
        n = seen.get(name, 0)
        seen[name] = n + 1
        out = name if n == 0 else "%s.dup%d%s" % (
            os.path.splitext(name)[0], n, os.path.splitext(name)[1])
        with open(os.path.join(outdir, out), "wb") as f:
            f.write(data)
        count += 1
    print(count)


main()
