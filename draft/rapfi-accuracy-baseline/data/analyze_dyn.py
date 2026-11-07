import io, json, statistics, collections, sys

files = ['.workbuddy/tmp/engine-sweet/dyn-8t-a.json',
         '.workbuddy/tmp/engine-sweet/dyn-8t-b.json']
samples = []
for f in files:
    d = json.load(io.open(f, encoding='utf-8'))
    samples += d['samples']

variants = ['lazy', 'hyb', 'A', 'B']
by = collections.defaultdict(dict)
for s in samples:
    by[s['pos']].setdefault(s['variant'], []).append(s)

print('pos var   n  medD  min  max  spread  medNodes(M)  best(move:count)')
for pos in sorted(by):
    base = statistics.median([s['d'] for s in by[pos]['lazy']])
    for v in variants:
        rows = by[pos][v]
        ds = [s['d'] for s in rows]
        ns = [s['nodes'] / 1e6 for s in rows]
        best = collections.Counter(s['best'] for s in rows)
        print('%-3d %-5s %2d %5.1f %4d %4d %6d %10.2f  %s%s'
              % (pos, v, len(rows), statistics.median(ds), min(ds), max(ds),
                 max(ds) - min(ds), statistics.median(ns),
                 ','.join('%s:%d' % kv for kv in best.most_common()),
                 '  (+%.1f)' % (statistics.median(ds) - base) if v != 'lazy' else ''))

print('\nsummary median across positions:')
for v in variants:
    meds = [statistics.median([s['d'] for s in by[pos][v]]) for pos in sorted(by)]
    print('  %-5s %s  mean=%.2f' % (v, ['%.0f' % m for m in meds], sum(meds) / len(meds)))
