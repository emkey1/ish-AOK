# summarise.py <results or ab log>... -- medians per architecture, build A vs B
# (the two labels in the order they first appear), from bench.sh's lines.
# 7-Zip is MIPS (higher is better); the Python columns are ms (lower is better).
import re, sys, statistics as st, collections
pat = re.compile(r'r(\d+)-b(\S+) .*?\] (\w+) (\S+) 7z=(\d*) fib=(\d+) method=(\d+) dict=(\d+) str=(\d+)')
for path in sys.argv[1:]:
    d = collections.defaultdict(list); builds = []
    for line in open(path, errors='replace'):
        m = pat.search(line)
        if not m:
            continue
        b, arch = m.group(2), m.group(3)
        if b not in builds:
            builds.append(b)
        d[(arch, b)].append([int(x) if x else None for x in m.groups()[4:]])
    if len(builds) < 2:
        print(path, ': fewer than two builds'); continue
    a, b = builds[:2]
    print('== %s  (%s -> %s; 7-Zip MIPS, Python ms)' % (path.split('/')[-1], a, b))
    for arch in ('i386', 'amd64', 'arm64', 'riscv64'):
        ra, rb = d.get((arch, a), []), d.get((arch, b), [])
        if not ra or not rb:
            continue
        med = lambda rows, i: st.median([v[i] for v in rows if v[i] is not None])
        cols = ' '.join('%s %d->%d' % (n, med(ra, i), med(rb, i))
                        for i, n in enumerate(('fib', 'method', 'dict', 'str'), 1))
        print('%-8s 7z %4d -> %-4d (%.2fx)  %s  [7z %s | %s; n=%d/%d]' % (
            arch, med(ra, 0), med(rb, 0), med(rb, 0) / med(ra, 0), cols,
            '/'.join(str(v[0]) for v in ra), '/'.join(str(v[0]) for v in rb), len(ra), len(rb)))
