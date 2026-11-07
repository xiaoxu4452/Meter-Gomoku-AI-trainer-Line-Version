#!/usr/bin/env python3
"""Generate an unbiased position corpus by letting the reference engine self-play.

Two games with fixed opening seeds; the engine chooses every move at 250 ms.
Snapshots are taken after a fixed number of stones (6, 10, 14) - no hand picking.
"""
import json, os, subprocess, sys, threading, time

EXE = r"C:\Users\harve\Desktop\document\Rapfi-YixinBoard\engine.exe"
CWD = r"C:\Users\harve\Desktop\document\Rapfi-YixinBoard"
SEEDS = [[(7, 7, 1)], [(7, 7, 1), (8, 8, 2)]]
STOP_AT = 16
SNAPS = [6, 10, 14]
MS = 250


def board_cmd(moves):
    out = "YXBOARD"
    for (x, y, c) in moves:
        out += " %d,%d,%d" % (x, y, c)
    return out + " DONE\n"


def best_move(moves):
    p = subprocess.Popen([EXE], cwd=CWD, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
                         bufsize=1)
    mv = {"v": None}
    done = threading.Event()

    def reader():
        for line in p.stdout:
            line = line.rstrip("\r\n")
            if line.startswith("INFO BESTLINE "):
                try:
                    xy = line[14:].strip().split()[0]
                    x, y = xy.split(",")
                    mv["v"] = (int(x), int(y))
                except Exception:
                    pass
            elif line.strip().replace(",", "").isdigit() and "," in line:
                try:
                    x, y = line.strip().split(",")
                    mv["v"] = (int(x), int(y))
                except Exception:
                    pass
        done.set()

    threading.Thread(target=reader, daemon=True).start()
    p.stdin.write("START 15\nINFO RULE 2\nINFO THREAD_NUM 2\nINFO HASH_SIZE 65536\n"
                  "INFO SHOW_DETAIL 2\nINFO CAUTION_FACTOR 3\nINFO PONDERING 0\n"
                  "INFO TIMEOUT_MATCH 0\nINFO TIMEOUT_TURN %d\n" % MS)
    p.stdin.write(board_cmd(moves))
    p.stdin.write("YXNBEST 1\n")
    p.stdin.flush()
    deadline = time.time() + MS / 1000.0 + 1.5
    while time.time() < deadline and mv["v"] is None:
        time.sleep(0.02)
    try:
        p.kill()
    except Exception:
        pass
    return mv["v"]


games = []
for seed in SEEDS:
    moves = list(seed)
    snaps = []
    while len(moves) < STOP_AT:
        side = 1 if len(moves) % 2 == 0 else 2
        mv = best_move(moves)
        if not mv or any((mv[0], mv[1]) == (x, y) for (x, y, _c) in moves):
            print("self-play stopped at %d stones (no legal move parsed)" % len(moves))
            break
        moves.append((mv[0], mv[1], side))
        if len(moves) in SNAPS:
            snaps.append({"stones": len(moves), "moves": list(moves)})
    games.append({"seed": [list(s) for s in seed], "moves": [list(m) for m in moves], "snaps": snaps})
    print("game seed=%s stones=%d snaps=%s" % (seed, len(moves), [s["stones"] for s in snaps]))

out = {"exe": EXE, "ms": MS, "rule": 2, "games": games}
with open(sys.argv[1], "w", encoding="utf-8") as f:
    json.dump(out, f)
print("wrote", sys.argv[1])
