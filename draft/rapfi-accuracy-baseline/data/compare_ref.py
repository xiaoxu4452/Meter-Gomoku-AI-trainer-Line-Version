import io, json, sys

def load(p):
    return json.load(io.open(p, encoding='utf-8'))

def spearman(xs, ys):
    n = len(xs)
    if n < 3:
        return None
    rx = [sorted(xs).index(v) + 1 for v in xs]
    ry = [sorted(ys).index(v) + 1 for v in ys]
    d2 = sum((a - b) ** 2 for a, b in zip(rx, ry))
    return 1 - 6 * d2 / (n * (n * n - 1))

def compare(a, b):
    ra = load(a)
    rb = load(b)
    print('== %s  vs  %s' % (a, b))
    for r in ra['rounds']:
        ref = next((q for q in rb['rounds'] if q['depth'] == r['depth']), None)
        if not ref:
            continue
        va = {(p['x'], p['y']): p['wr'] for p in r['pvs'] if p.get('wr') is not None}
        vb = {(p['x'], p['y']): p['wr'] for p in ref['pvs'] if p.get('wr') is not None}
        common = sorted(set(va) & set(vb))
        if not common:
            print('  d%-2d no common points (%d vs %d)' % (r['depth'], len(va), len(vb)))
            continue
        ds = [abs(va[k] - vb[k]) for k in common]
        mean = sum(ds) / len(ds)
        mx = max(ds)
        worst = max(common, key=lambda k: abs(va[k] - vb[k]))
        sp = spearman([va[k] for k in common], [vb[k] for k in common])
        print('  d%-2d common=%d/%d  mean=%.2f  max=%.2f (%d,%d)  spearman=%s'
              % (r['depth'], len(common), max(len(va), len(vb)), mean, mx, worst[0], worst[1],
                 ('%.3f' % sp) if sp is not None else '-'))

if __name__ == '__main__':
    compare(sys.argv[1], sys.argv[2])
