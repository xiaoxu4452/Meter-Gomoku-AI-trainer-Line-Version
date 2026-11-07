import collections, io, json, statistics, sys

files = sys.argv[1:]
samples = []
for f in files:
    samples += json.load(io.open(f, encoding='utf-8'))['samples']

variants = ['lazy', 'hyb', 'A', 'B']
by = collections.defaultdict(lambda: collections.defaultdict(list))
for s in samples:
    by[s['pi']][s['variant']].append(s)

print('pos var   n  best(count)                      eval(count)                 medNodes(M)')
for pi in sorted(by):
    stones = by[pi]['lazy'][0]['pos']
    for v in variants:
        rows = by[pi].get(v, [])
        if not rows:
            continue
        bc = collections.Counter(r['best'] for r in rows)
        ec = collections.Counter(r['ev'] for r in rows)
        print('s%-3d %-5s %2d %-30s %-26s %9.2f'
              % (stones, v, len(rows),
                 ','.join('%s:%d' % kv for kv in bc.most_common(3)),
                 ','.join('%s:%d' % kv for kv in ec.most_common(3)),
                 statistics.median([r['nodes'] / 1e6 for r in rows])))

print('\ndrift vs lazy (positions where best or eval differs in any pair):')
for pi in sorted(by):
    base = by[pi]['lazy']
    for v in ['hyb', 'A', 'B']:
        rows = by[pi].get(v, [])
        if not rows:
            continue
        bs = set(r['best'] for r in rows)
        lb = set(r['best'] for r in base)
        es = set(r['ev'] for r in rows)
        le = set(r['ev'] for r in base)
        same_b = bs == lb
        same_e = es == le
        print('  s%-3d %-5s bestSame=%s (%s vs %s) evalSame=%s (%s vs %s)'
              % (by[pi]['lazy'][0]['pos'], v, same_b, sorted(bs), sorted(lb), same_e,
                 sorted(es), sorted(le)))
