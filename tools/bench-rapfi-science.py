import subprocess, time, threading, sys, shutil, os, re

SRC = r"C:\Users\harve\Desktop\Gobang auto\Meter Gomoku\rapfi-native"
ROOT = r"C:\Users\harve\Desktop\Gobang auto\_rapfi_bench"
DRAFT_CFG = r"C:\Users\harve\Desktop\Gobang auto\meter-gomoku-repo\draft\rapfi-science\config.toml"

for d in ("_bench_base", "_bench_mod"):
    t = os.path.join(ROOT, d)
    if not os.path.isdir(t):
        shutil.copytree(SRC, t, ignore=shutil.ignore_patterns("*.tmp"))
shutil.copyfile(DRAFT_CFG, os.path.join(ROOT, "_bench_mod", "config.toml"))

def run_pos(cwd, moves, wait=4.0):
    eng = os.path.join(cwd, "RapfiEngine-sse.exe")
    p = subprocess.Popen([eng], cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, bufsize=1, universal_newlines=True)
    time.sleep(1.5)
    buf = []
    def reader():
        try:
            for line in p.stdout:
                buf.append(line.rstrip("\n"))
        except Exception as e:
            buf.append("READER-ERR: %r" % e)
    threading.Thread(target=reader, daemon=True).start()
    def send(cmd):
        p.stdin.write(cmd + "\n"); p.stdin.flush(); time.sleep(0.3)
    send("START 15")
    send("INFO TIMEOUT_TURN 1000")
    send("INFO RULE 0")
    send("BOARD")
    for mv in moves:
        send(mv)
    send("DONE")
    time.sleep(wait)
    try: p.kill()
    except Exception: pass
    return list(buf)

def parse(out):
    sp = [l for l in out if "MESSAGE Speed" in l]
    sp = sp[-1] if sp else ""
    m = re.search(r"Depth (\d+)-(\d+) \| Eval ([+\-]?\w+) \| Node ([\d.]+[KM]?) \| Time (\d+)", sp)
    d, eval_, node, t = (int(m.group(1)), m.group(3), m.group(4), int(m.group(5))) if m else (-1, "-", "-", -1)
    best = None
    for l in out:
        if re.match(r"^\d+,\d+$", l.strip()):
            best = l.strip()
    err = [l for l in out if l.startswith("ERROR")]
    return {"depth": d, "eval": eval_, "nodes": node, "time": t, "move": best, "err": err[:2]}

POS = [
    ("open-even",  ["8,8,1", "8,9,2", "7,9,1", "7,7,2"],        "black to move, balanced-ish"),
    ("white-adv",  ["8,8,1", "8,9,2", "8,10,1", "9,9,2", "7,7,1", "9,10,2", "6,6,1"], "white 4 vs black 3, white line"),
    ("black-thr",  ["8,8,1", "8,9,2", "9,8,1", "9,9,2", "10,8,1", "7,7,2", "11,8,1"], "black 4 vs white 2, black threat"),
]
print("POS  CONFIG  DEPTH  EVAL  NODES  TIME(ms)  MOVE   ERR")
for name, mv, desc in POS:
    b = parse(run_pos(os.path.join(ROOT, "_bench_base"), mv))
    m = parse(run_pos(os.path.join(ROOT, "_bench_mod"), mv))
    for tag, r in (("BASE", b), ("MOD ", m)):
        print("%-9s %-6s %5d %5s %8s %8d %-6s %s" % (name, tag, r["depth"], r["eval"], r["nodes"], r["time"], str(r["move"]), ";".join(r["err"])))
print("BENCH DONE")
