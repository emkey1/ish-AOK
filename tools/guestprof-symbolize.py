#!/usr/bin/env python3
"""Turn a guestprof report's file offsets into symbol names.

The profiler cannot symbolize on its own: it runs inside the emulator, and the
guest's .so files live in a fakefs root whose host-side layout is an
implementation detail. So it reports "<path> +0x<file offset>" and this script
resolves that against the actual ELF.

The ELF parsing is done here rather than shelled out to readelf/nm because
neither is guaranteed: macOS has no binutils, and a minimal guest root often
has no binutils either. Supports 32- and 64-bit little-endian ELF, which covers
every guest AOK-iSH runs (i386, amd64, arm64, riscv64).

Usage:
    tools/guestprof-symbolize.py REPORT --libdir DIR [--top N]

DIR holds the guest binaries, by basename. Extract them with a realfs mount:
    ISH_REAL_MNT=DIR ./build/ish -f ROOT /bin/sh -c 'cp /usr/lib/.../libz.so.1.3.1 /realmnt/'
"""
import argparse, os, re, struct, sys

PT_LOAD = 1
SHT_SYMTAB, SHT_DYNSYM = 2, 11


class Elf:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.b = f.read()
        if self.b[:4] != b'\x7fELF':
            raise ValueError('%s: not an ELF file' % path)
        self.is64 = self.b[4] == 2
        if self.b[5] != 1:
            raise ValueError('%s: big-endian ELF is not supported' % path)
        self.loads = []     # (p_offset, p_filesz, p_vaddr)
        self.syms = []      # (value, size, name), FUNC only, sorted by value
        self._parse()

    def _u(self, fmt, off):
        return struct.unpack_from('<' + fmt, self.b, off)

    def _parse(self):
        if self.is64:
            e_phoff, e_shoff = self._u('Q', 0x20)[0], self._u('Q', 0x28)[0]
            e_phentsize, e_phnum = self._u('H', 0x36)[0], self._u('H', 0x38)[0]
            e_shentsize, e_shnum = self._u('H', 0x3a)[0], self._u('H', 0x3c)[0]
        else:
            e_phoff, e_shoff = self._u('I', 0x1c)[0], self._u('I', 0x20)[0]
            e_phentsize, e_phnum = self._u('H', 0x2a)[0], self._u('H', 0x2c)[0]
            e_shentsize, e_shnum = self._u('H', 0x2e)[0], self._u('H', 0x30)[0]

        for i in range(e_phnum):
            o = e_phoff + i * e_phentsize
            if self.is64:
                p_type = self._u('I', o)[0]
                p_offset, p_vaddr = self._u('Q', o + 8)[0], self._u('Q', o + 16)[0]
                p_filesz = self._u('Q', o + 32)[0]
            else:
                p_type = self._u('I', o)[0]
                p_offset, p_vaddr = self._u('I', o + 4)[0], self._u('I', o + 8)[0]
                p_filesz = self._u('I', o + 16)[0]
            if p_type == PT_LOAD:
                self.loads.append((p_offset, p_filesz, p_vaddr))

        # Both tables: a stripped .so keeps .dynsym only, and an unstripped one
        # has names in .symtab that .dynsym does not export.
        for i in range(e_shnum):
            o = e_shoff + i * e_shentsize
            sh_type = self._u('I', o + 4)[0]
            if sh_type not in (SHT_SYMTAB, SHT_DYNSYM):
                continue
            if self.is64:
                sh_off, sh_size = self._u('Q', o + 24)[0], self._u('Q', o + 32)[0]
                sh_link, sh_entsize = self._u('I', o + 40)[0], self._u('Q', o + 56)[0]
            else:
                sh_off, sh_size = self._u('I', o + 16)[0], self._u('I', o + 20)[0]
                sh_link, sh_entsize = self._u('I', o + 24)[0], self._u('I', o + 36)[0]
            so = e_shoff + sh_link * e_shentsize
            str_off = self._u('Q' if self.is64 else 'I', so + (24 if self.is64 else 16))[0]
            if sh_entsize == 0:
                continue
            for j in range(sh_size // sh_entsize):
                so2 = sh_off + j * sh_entsize
                if self.is64:
                    st_name, st_info = self._u('I', so2)[0], self.b[so2 + 4]
                    st_value, st_size = self._u('Q', so2 + 8)[0], self._u('Q', so2 + 16)[0]
                else:
                    st_name = self._u('I', so2)[0]
                    st_value, st_size = self._u('I', so2 + 4)[0], self._u('I', so2 + 8)[0]
                    st_info = self.b[so2 + 12]
                if (st_info & 0xf) != 2 or st_value == 0:   # STT_FUNC only
                    continue
                end = self.b.find(b'\0', str_off + st_name)
                name = self.b[str_off + st_name:end].decode('utf-8', 'replace')
                if name:
                    self.syms.append((st_value, st_size, name))
        self.syms.sort()

    def file_off_to_vaddr(self, off):
        for p_offset, p_filesz, p_vaddr in self.loads:
            if p_offset <= off < p_offset + p_filesz:
                return off - p_offset + p_vaddr
        return None

    def symbolize(self, off):
        """Returns (name, delta, exact). exact=False means the address is past
        the end of that symbol -- it is in an unexported local function that
        follows it, and the name is only the nearest landmark."""
        va = self.file_off_to_vaddr(off)
        if va is None:
            return None, None, False
        containing = None
        preceding = None
        for value, size, name in self.syms:
            if value > va:
                break
            preceding = (name, va - value)
            if not size or value + size > va:
                containing = (name, va - value)
        if containing:
            return containing[0], containing[1], True
        if preceding:
            return preceding[0], preceding[1], False
        return None, None, False


LINE = re.compile(r'^\s*([\d.]+)%\s+(\d+)\s+SYM\s+(\S+)\s+\+0x([0-9a-fA-F]+)\s*$')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('report')
    ap.add_argument('--libdir', required=True)
    ap.add_argument('--top', type=int, default=5)
    args = ap.parse_args()

    per_dso = {}
    for line in open(args.report):
        m = LINE.match(line)
        if not m:
            continue
        pct, count, path, off = float(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4), 16)
        per_dso.setdefault(path, []).append((pct, count, off))

    if not per_dso:
        print('no "SYM <path> +0x..." lines in %s' % args.report)
        return 1

    cache = {}
    for path, rows in sorted(per_dso.items(), key=lambda kv: -sum(r[1] for r in kv[1])):
        base = os.path.basename(path)
        local = os.path.join(args.libdir, base)
        elf = cache.get(local, 'miss')
        if elf == 'miss':
            try:
                elf = Elf(local)
            except Exception as e:
                elf = None
                print('%s: %s' % (base, e), file=sys.stderr)
            cache[local] = elf

        # Fold offsets that land in the same function together: the sampler
        # reports block addresses, so one function shows up as many offsets and
        # a raw top-5 of offsets would be five rows of the same symbol.
        by_sym, unresolved = {}, {}
        for pct, count, off in rows:
            name, delta, exact = None, None, False
            if elf is not None:
                name, delta, exact = elf.symbolize(off)
            if name:
                # An inexact hit names the landmark, not the function, so all
                # of them fold under one clearly-approximate label rather than
                # pretending to be that symbol.
                key = name if exact else '~after %s (unexported local)' % name
                p, c = by_sym.get(key, (0.0, 0))
                by_sym[key] = (p + pct, c + count)
            else:
                unresolved[off] = unresolved.get(off, 0) + count

        total = sum(c for _, c in by_sym.values()) + sum(unresolved.values())
        print('\n%s   (%d samples in report)' % (path, total))
        for name, (pct, count) in sorted(by_sym.items(), key=lambda kv: -kv[1][1])[:args.top]:
            print('   %6.2f%%  %8d  %s' % (pct, count, name))
        if unresolved:
            uc = sum(unresolved.values())
            print('   %6s   %8d  (%d offsets with no FUNC symbol)' % ('', uc, len(unresolved)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
