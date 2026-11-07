#!/usr/bin/env python3
"""Accuracy report: reference software as oracle, ours vs reference at equal budgets."""
import glob, io, json, os, re, statistics

ROOT = os.path.dirname(os.path.abspath(__file__))

def load(p):
    return json.load(io.open(p, encoding='utf-8'))

def full_round(d):
    best = None
    mx = max((r['count'] for r in d['rounds']), default=0)
    for r in d['rounds']:
        if r['count'] == mx and mx > 0:
            best = r
    return best

def spearman(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    def rank(v):
        order = sorted(range(n), key=lambda i: v[i])
        r = [0] * n
        k = 0
        while k < n:
            j = k
            while j + 1 < n and v[order[j + 1]] == v[order[k]]:
                j += 1
            avg = (k + j + 2) / 2.0
            for t in range(k, j + 1):
                r[order[t]] = avg
            k = j + 1
        return r
    rx, ry = rank(xs), rank(ys)
    d2 = sum((a - b) ** 2 for a, b in zip(rx, ry))
    return 1 - 6 * d2 / (n * (n * n - 1))

def mate_depth(d):
    for r in sorted(d['rounds'], key=lambda x: x['depth']):
        ps = [p for p in r['pvs'] if p.get('wr') is not None]
        if not ps:
            continue
        top = max(ps, key=lambda p: p['wr'])
        if re.match(r'^[+-]M\d+$', str(top.get('eval', ''))):
            return r['depth']
    return None

def final_round(d):
    best = None
    for r in d['rounds']:
        if r['pvs'] and (best is None or r['depth'] >= best['depth']):
            best = r
    return best

def table(path, oracle):
    d = load(path)
    r = full_round(d)
    if not r:
        return None
    fr = final_round(d)
    ora = {(p['x'], p['y']): p for p in full_round(oracle)['pvs'] if p.get('wr') is not None}
    cur = {(p['x'], p['y']): p for p in r['pvs'] if p.get('wr') is not None}
    pts = sorted(set(ora) & set(cur))
    if len(pts) < 20:
        return None
    xs = [cur[k]['wr'] for k in pts]
    ys = [ora[k]['wr'] for k in pts]
    dev = [abs(a - b) * 100 for a, b in zip(xs, ys)]
    top_o = [k for k in sorted(pts, key=lambda k: -ora[k]['wr'])[:3]]
    top_c = [k for k in sorted(pts, key=lambda k: -cur[k]['wr'])[:3]]
    decisive = [k for k in pts if abs(ora[k]['wr'] - 0.5) >= 0.30]
    ok = 0
    if decisive:
        ok = sum(1 for k in decisive
                 if (cur[k]['wr'] >= 0.5) == (ora[k]['wr'] >= 0.5)) / float(len(decisive))
    ft = None
    if fr:
        ps = [p for p in fr['pvs'] if p.get('wr') is not None]
        if ps:
            ft = max(ps, key=lambda p: p['wr'])
    ora_final = final_round(oracle)
    ora_top = None
    if ora_final:
        ps = [p for p in ora_final['pvs'] if p.get('wr') is not None]
        if ps:
            ora_top = max(ps, key=lambda p: p['wr'])
    return dict(depth=r['depth'], n=len(pts), mean=sum(dev) / len(dev), mx=max(dev),
                sp=spearman(xs, ys), top1=top_o[0] == top_c[0],
                top3=len(set(top_o) & set(top_c)) / 3.0, dec=ok,
                finalDepth=fr['depth'] if fr else None,
                finalTop=(ft['x'], ft['y'], ft['wr'], ft['eval']) if ft else None,
                finalTopOracle=(ora_top['x'], ora_top['y'], ora_top['wr'], ora_top['eval']) if ora_top else None,
                mateDepth=mate_depth(d))

files = sorted(glob.glob(os.path.join(ROOT, 'acc-*.json')))
tags = {}
for f in files:
    m = re.search(r'acc-((?:g|q)\d+-s\d+)-(ref60|ref15|ref5|our15|our5)\.json', f)
    if not m:
        continue
    tags.setdefault(m.group(1), {})[m.group(2)] = f

rows = {}
print('%-9s %-6s %5s %6s %6s %6s %6s %6s %6s  %s' %
      ('pos', 'run', 'depth', 'n', 'meanPp', 'maxPp', 'spear', 'top1', 'top3', 'final(best/wr/eval) mateD'))
for tag in sorted(tags):
    ora = load(tags[tag]['ref60'])
    for run in ['ref60', 'ref15', 'ref5', 'our15', 'our5']:
        if run not in tags[tag]:
            continue
        s = table(tags[tag][run], ora)
        if not s:
            print('%-9s %-6s no full round' % (tag, run))
            continue
        rows.setdefault(run, []).append(s)
        ft = s['finalTop']
        print('%-9s %-6s %5d %6d %6.2f %6.2f %6.3f %6s %6.2f  %s/%s mate@%s'
              % (tag, run, s['depth'], s['n'], s['mean'], s['mx'], s['sp'],
                 'Y' if s['top1'] else 'n', s['top3'],
                 ('%d,%d' % (ft[0], ft[1]) + '/%.1f/%s' % (ft[2] * 100, ft[3])) if ft else '-',
                 s['finalDepth'], s['mateDepth']))
print('\noracle final best:')
for tag in sorted(tags):
    s = table(tags[tag]['ref60'], load(tags[tag]['ref60']))
    if s and s['finalTopOracle']:
        ft = s['finalTopOracle']
        print('  %-9s %d,%d wr=%.1f%% eval=%s' % (tag, ft[0], ft[1], ft[2] * 100, ft[3]))

print('\naggregate (mean across positions):')
for run in ['ref60', 'ref15', 'ref5', 'our15', 'our5']:
    if run not in rows:
        continue
    rs = rows[run]
    print('  %-6s pos=%d  meanPp=%.2f  maxPp=%.2f  spear=%.3f  top1=%.0f%%  top3=%.2f  dec=%.0f%%'
          % (run, len(rs), statistics.mean([x['mean'] for x in rs]),
             max(x['mx'] for x in rs), statistics.mean([x['sp'] for x in rs]),
             statistics.mean([1.0 if x['top1'] else 0.0 for x in rs]) * 100,
             statistics.mean([x['top3'] for x in rs]),
             statistics.mean([x['dec'] for x in rs]) * 100))

print('\nhead-to-head at equal budget (negative delta = ours closer to oracle):')
for tag in sorted(tags):
    for bud in ['5', '15']:
        a = table(tags[tag]['our' + bud], load(tags[tag]['ref60'])) if ('our' + bud) in tags[tag] else None
        b = table(tags[tag]['ref' + bud], load(tags[tag]['ref60'])) if ('ref' + bud) in tags[tag] else None
        if a and b:
            print('  %-9s %ss  our=%.2f  ref=%.2f  delta=%+.2f  rank our=%.3f ref=%.3f'
                  % (tag, bud, a['mean'], b['mean'], a['mean'] - b['mean'], a['sp'], b['sp']))
