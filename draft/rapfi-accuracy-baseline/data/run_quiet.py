#!/usr/bin/env python3
"""Run the quiet random corpus with the same five scan budgets as run_accuracy.py."""
import json, os, subprocess, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
DRV = os.path.join(os.path.dirname(ROOT), 'scan-ab', 'drive_engine.py')
OUR = r'Meter Gomoku\rapfi-native\RapfiEngine-avx512vnni.exe'
OURCWD = r'Meter Gomoku\rapfi-native'
REF = r'C:\Users\harve\Desktop\document\Rapfi-YixinBoard\engine.exe'
REFCWD = r'C:\Users\harve\Desktop\document\Rapfi-YixinBoard'

corpus = json.load(open(os.path.join(ROOT, 'corpus-quiet.json'), encoding='utf-8'))
pos = []
for gi, g in enumerate(corpus['games']):
    for s in g['snaps']:
        pos.append((gi, s['stones'], s['moves']))

RUNS = [('ref60', 60), ('ref15', 15), ('ref5', 5), ('our15', 15), ('our5', 5)]

for gi, stones, moves in pos:
    mv = ';'.join('%d,%d,%d' % (m[0], m[1], m[2]) for m in moves)
    tag = 'q%d-s%d' % (gi, stones)
    for name, sec in RUNS:
        out = os.path.join(ROOT, 'acc-%s-%s.json' % (tag, name))
        if os.path.exists(out):
            continue
        exe, cwd = (OUR, OURCWD) if name.startswith('our') else (REF, REFCWD)
        cmd = [sys.executable, DRV, '--exe', exe, '--cwd', cwd, '--moves', mv,
               '--seconds', str(sec), '--threads', '8', '--hashkb', '1048576',
               '--detail', '2', '--caution', '3', '--rule', '2', '--json', out]
        r = subprocess.run(cmd, capture_output=True, text=True)
        tail = (r.stdout or '').strip().splitlines()
        print('%s %-6s %s' % (tag, name, tail[-1] if tail else 'no output'), flush=True)
print('done', flush=True)
