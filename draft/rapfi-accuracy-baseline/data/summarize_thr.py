#!/usr/bin/env python3
"""Summarise engine-ab throughput JSONs per position (median depth/nps/nodes)."""
import io, json, statistics, sys, collections

for path in sys.argv[1:]:
    d = json.load(io.open(path, encoding='utf-8'))
    print('== %s  turn=%dms reps=%d threads=%d hash=%dMB rule/caution as sent'
          % (path.split('/')[-1], d['turn'], d['reps'], d['threads'], d['hashKB'] // 1024))
    by = collections.defaultdict(list)
    bad = []
    for s in d['samples']:
        if s['d'] == 0 or not (s.get('best') or '').strip():
            bad.append((s['pos'], s['variant'], s['wall']))
            continue
        by[(s['pi'], s['variant'])].append(s)
    pos = sorted(set(k[0] for k in by))
    names = [v['name'] for v in d['variants']]
    print('  %-4s %-10s %2s %6s %6s %10s %8s %6s %s'
          % ('pos', 'variant', 'n', 'depth', 'seld', 'nps(k)', 'nodes(M)', 'wall', 'best'))
    agg = collections.defaultdict(lambda: {'d': [], 'sd': [], 'nps': [], 'nodes': [], 'wall': []})
    for p in pos:
        for n in names:
            rows = by.get((p, n), [])
            if not rows:
                continue
            md = statistics.median([r['d'] for r in rows])
            msd = statistics.median([r['sd'] for r in rows])
            mnps = statistics.median([r['speed'] for r in rows])
            mnd = statistics.median([r['nodes'] for r in rows])
            mw = statistics.median([r['wall'] for r in rows])
            bc = collections.Counter(r['best'] for r in rows)
            print('  %-4s %-10s %2d %6.1f %6.1f %10.0f %8.2f %6d %s'
                  % (p, n, len(rows), md, msd, mnps / 1000, mnd / 1e6, mw,
                     ','.join('%s:%d' % kv for kv in bc.most_common(2))))
            a = agg[n]
            a['d'] += [r['d'] for r in rows]
            a['sd'] += [r['sd'] for r in rows]
            a['nps'] += [r['speed'] for r in rows]
            a['nodes'] += [r['nodes'] for r in rows]
            a['wall'] += [r['wall'] for r in rows]
    print('  -- all positions --')
    for n in names:
        a = agg[n]
        if not a['nps']:
            continue
        print('  %-10s n=%2d  depth med %.1f  seld med %.1f  nps med %.0fk  nodes med %.2fM  wall med %dms'
              % (n, len(a['nps']), statistics.median(a['d']), statistics.median(a['sd']),
                 statistics.median(a['nps']) / 1000, statistics.median(a['nodes']) / 1e6,
                 statistics.median(a['wall'])))
    if bad:
        print('  dropped (engine exited before a move): %s' % bad)
