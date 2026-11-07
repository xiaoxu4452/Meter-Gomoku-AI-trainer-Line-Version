import io, json, glob, os, sys

def load(p):
    return json.load(io.open(p, encoding='utf-8'))

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

def table(a, b, min_common=50):
    ra, rb = load(a), load(b)
    rows = []
    for r in ra['rounds']:
        ref = next((q for q in rb['rounds'] if q['depth'] == r['depth']), None)
        if not ref:
            continue
        va = {(p['x'], p['y']): p['wr'] for p in r['pvs'] if p.get('wr') is not None}
        vb = {(p['x'], p['y']): p['wr'] for p in ref['pvs'] if p.get('wr') is not None}
        common = sorted(set(va) & set(vb))
        if len(common) < min_common:
            continue
        ds = [abs(va[k] - vb[k]) * 100 for k in common]
        rows.append((r['depth'], len(common), sum(ds) / len(ds), max(ds),
                     spearman([va[k] for k in common], [vb[k] for k in common])))
    if not rows:
        return None
    mean_dev = sum(x[2] for x in rows) / len(rows)
    max_dev = max(x[3] for x in rows)
    mean_sp = sum(x[4] for x in rows if x[4] is not None) / max(1, len([x for x in rows if x[4] is not None]))
    last = rows[-1]
    return dict(depths=len(rows), mean=mean_dev, max=max_dev, sp=mean_sp, last=last)

base = sys.argv[1]
ref = sys.argv[2]
print('%-18s %6s %7s %8s %8s %s' % ('variant', 'depths', 'meanPp', 'maxPp', 'meanSp', 'last(d/mean/max/sp)'))
for p in [base] + sys.argv[3:]:
    s = table(p, ref)
    if not s:
        print('%-18s no overlap' % os.path.basename(p))
        continue
    name = os.path.basename(p).replace('.json', '').replace('refbase-', '').replace('-open', '')
    l = s['last']
    print('%-18s %6d %7.2f %8.2f %8.3f  d%d/%.2f/%.2f/%.3f'
          % (name, s['depths'], s['mean'], s['max'], s['sp'], l[0], l[2], l[3], l[4]))
