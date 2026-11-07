# Rapfi scan alignment: candidate area grows with the shape, evaluation refines with time

Date: 2026-11-07
Reference: `C:\Users\harve\Desktop\document\Rapfi-YixinBoard` (Rapfi 0.43.01, `model210901.bin`)
Target: desktop host `desktop-connect` + `Meter Gomoku/rapfi-native/config.toml`

The goal was to make the desktop "defend scan" (`YXSEARCHDEFEND`) behave exactly like the
Yixin-Board reference instead of our own variant. This note records what the reference really
does (line by line), what we changed, and the A/B measurements that prove the alignment.

---

## 1. What the reference actually does

### 1.1 Candidate area = fixed stencil around every stone

* `core/pos.h:307-318` `inCandidateRange(dx, dy, squareDist, lineDist)`:
  the centre is excluded; `|dx|,|dy| <= lineDist` is required; a cell qualifies when it is
  inside the square (`|dx| <= squareDist && |dy| <= squareDist`) **or** on one of the eight
  lines (`dx == 0 || dy == 0 || |dx| == |dy|`).
* `RANGE_SQUARE3_LINE4 = buildCandidateRange<3, 4>()` (`core/pos.h:350`) therefore yields
  **56 offsets**: the 7x7 square minus its centre (48) plus the 8 fourth cells of the
  horizontal / vertical / diagonal rays (8). `candidateRangeInfo()` maps it to
  `expandDist = 3` (`core/pos.h:375-376`).
* `game/board.cpp:245` — every `Board::move(pos)` does
  `st.candArea.expand(pos, boardSize, candAreaExpandDist)` (the bounding box grows by 3 in
  all directions) and `game/board.cpp:369+` stamps `candStencil` at `pos` into the board-level
  candidate bitboard, journaling the 0->1 transitions so `undo()` can rewind them exactly.
* Consequence: the candidate set is the **union of the 56-cell stencil of every stone on the
  board**, clipped to the board and minus occupied points. It grows as stones are added and
  never shrinks inside a game; it is recomputed from the move list (`YXBOARD ... DONE`), it is
  **not** expanded by search depth or by elapsed time.

### 1.2 Root set inside one search is a tactical filter, not "all candidates"

`search/movepick.cpp:131-158` (`RootMovePicker`):

| condition | root moves |
|---|---|
| self has A_FIVE | only winning moves (`generate<WINNING>`) |
| opponent has A_FIVE | only five-blocking moves (`generate<DEFEND_FIVE>`) |
| self has B_FLEX4 | only winning moves |
| opponent has B_FLEX4 | `DEFEND_FOUR \| ALL` plus VCF moves |
| opponent has C_BLOCK4_FLEX3 (and a legal renju situation) | only the defence points of that shape; falls back to `ALL` |
| otherwise | every candidate |

Then `search/searchengine.cpp:104-116` calls `Opening::filterSymmetryMoves`, which keeps one
representative per symmetry orbit. That is why a decisive position reports a handful of roots
instead of ~130, and why a symmetric opening reports about half of the candidate cells.

### 1.3 multiPV = min(requested, filter-surviving roots)

* `command/gomocup.cpp:694-695`: `YXSEARCHDEFEND` sets `options.multiPV = board->movesLeft()`
  and calls `think()`; no `CAUTION_FACTOR` change is involved.
* `search/ab/search.cpp:410,417`: `sd.multiPv = options.multiPV;`
  `sd.multiPv = min(sd.multiPv, th.rootMoves.size())`.
* `search/ab/search.cpp:470`: for every completed root depth, the loop
  `for (sd.pvIdx = 0; sd.pvIdx < sd.multiPv; ++sd.pvIdx)` searches each root and, after each
  PV, `sortRootMoves(begin, begin + pvIdx + 1)` re-orders the already-searched prefix. The
  printed index is the pre-sort slot, so consumers must re-sort in the same way.

### 1.4 Per-point output

`search/searchoutput.cpp` `printPvCompletes()` emits one `INFO PV <idx>` block containing
`NUMPV`, `DEPTH`, `SELDEPTH`, `EVAL`, `WINRATE`, `BESTLINE`:

* `EVAL` is from the **side to move** (positive = good for the player to move, `+M<ply>` /
  `-M<ply>` for proven mate);
* `WINRATE` is the same viewpoint (percentage, may exceed 100 only for mate);
* `BESTLINE` starts with the root move, in protocol coordinates.

### 1.5 What "refining" means in the GUI

`Yixin-Board/main.c` (cloned into `.workbuddy/tmp/Yixin-Board`):

* `searchdefend` (line ~4089) sends exactly `yxsearchdefend\n`. There is **no loop** that
  expands the candidate set, no re-issue of `YXBOARD`, no `CAUTION_FACTOR` stepping.
* Result handling (~line 6549): on each `INFO PV DONE` it immediately writes
  `boarddepth[y][x] = curdepth; boardtag[y][x] = tag` for that PV's first move.
* When `curpvidx + 1 == curnumpv` (the round is fully published) it clears every tag whose
  `boarddepth < curdepth` (~line 6555).

So the reference user experience is:

1. **shape growth** happens when a stone is played (stencil union), not while thinking;
2. **finer and finer** comes from iterative deepening: the *same* root set is re-reported each
   round with deeper `DEPTH/SELDEPTH/EVAL/WINRATE`, and the GUI repaints tag by tag as the round
   streams in, then discards stale-depth tags at round completion.

---

## 2. What the desktop did before and what changed

### 2.1 Real deviations found

| item | reference | desktop (before) |
|---|---|---|
| `coord_conversion_mode` | `flipY_X` | `X_flipY` → symmetric representatives differed; on a mirror-symmetric position our candidate set was the transpose of the reference (40-point symmetric difference) |
| scan publishing | GUI writes a tag at each `INFO PV DONE`; clears stale depths when the round finishes | host double-buffered the whole round and only committed after the next round's `INFO PV 0`; the final bare move committed the last half round, truncating the panel |
| candidate growth | per-move stencil union | identical (engine side, same `square3_line4`) |

### 2.2 Changes

* `rapfi-native/config.toml`: `coord_conversion_mode = "X_flipY"` -> `"flipY_X"` in both the
  shipped tree and the mirror. After the change the scan candidate set equals the reference
  set exactly on symmetric positions (see §3.3).
* `desktop-connect/src/engine_host.{h,cpp}`: `ParseState` gets `livePv` / `roundNumPv`. For
  scan requests (`mode == 3`) only:
  * every `INFO PV DONE` publishes the candidate immediately (no whole-round buffer);
  * when `pvIdx + 1 == NUMPV` the host drops entries whose `depth < curDepth` of that round —
    the same rule as the GUI;
  * a bare move no longer commits the trailing half round (the panel keeps the last complete
    round);
  * a new scan ask clears the previous scan state; auto-heal / restart keeps `livePv`.
  Normal play / balance requests keep the original whole-round double buffering (that buffering
  fixed an older evaluation-jitter bug and must not regress).

Note: `mode == 3` is also required for `ask`; the page already sends `scan: true` with
`topN: 1` in `ui/calc.js` `anaDefend()`.

---

## 3. A/B verification (same positions, same binary, same options)

Environment: Windows 10/11, `RapfiEngine-avx2.exe`, `RULE 2` (renju), 4 threads, 1 GB hash,
`INFO CAUTION_FACTOR 3`, `INFO SHOW_DETAIL 2`, `INFO PONDERING 0`, one long
`INFO TIMEOUT_TURN`. "Our" = desktop config/host; "ref" = `Rapfi-YixinBoard` with
`config.toml` untouched (after we aligned `flipY_X`, the only difference is packaging).

### 3.1 Point count per depth (does the set grow while searching?)

| position | stones | roots per completed round | bbox | fill of bbox |
|---|---|---|---|---|
| `open` | 5 | 68 at d2..d11 | `[3,4]-[11,11]` | 94.4% |
| `asym` | 10 | 129 at d2..d13 | `[1,2]-[13,13]` | 82.7% |
| `line` | 9 | 137 at d2..d10 | `[0,2]-[14,12]` | 83.0% |
| `spread` (two clusters) | 10 | 66 at d2..d12 | `[7,0]-[14,14]` | 55.0% |

The count is **constant across every completed depth**. The trailing partial rounds in the
recordings (6 / 13 / 10 points) are rounds still streaming out when time expired — exactly the
progressive publishing the GUI performs. So "grows with the shape" is a property of the move
list (more stones -> larger stencil union), and the scan itself only refines evaluations.

Shape check: the bbox is the stone bbox inflated by 3..4, never a padded rectangle; cluster
positions keep a low fill (55%) because the stencil follows each cluster separately.

### 3.2 Candidate set vs reference (set difference over every depth)

`open` and `asym`, per depth d2..d11: `onlyOur = 0`, `onlyRef = 0` (coordinates, not just
counts). Before the `flipY_X` fix, `spread2` (mirror symmetric) had 40 differing points; after
the fix `symdiff = 0`.

### 3.3 Per-point deviation vs an independent deep search

Reference C = `YXBOARD` with the candidate move appended, `YXNBEST 1`, 2.5 s, 3 in parallel;
our win rate for that point = `1 - WINRATE` (the engine's side there is the opponent).
Deviation is |scan win rate - reference win rate| over the tested points.

| position | group | roots | d/time | mean dev | max dev | Spearman |
|---|---|---|---|---|---|---|
| pos0 | A (scan) | 68 | d11 / 1.5 s | 6.49 pp | 21.62 pp | 0.952 |
| pos0 | A | 68 | d12 / 4 s | 3.67 pp | 8.20 pp | 0.770 |
| pos0 | A | 68 | d14 / 10 s | 2.66 pp | 7.43 pp | 0.842 |
| pos0 | B8 | 8 | d14 / 1.5 s | 2.28 pp | 6.43 pp | 0.714 |
| pos0 | B8 | 8 | d16 / 4 s | 1.02 pp | 2.74 pp | 0.881 |
| pos0 | B8 | 8 | d18 / 10 s | 0.87 pp | 3.12 pp | 0.690 |
| pos2 | A | 89 | d10 / 1.5 s | 7.56 pp | 19.32 pp | 0.867 |
| pos2 | A | 89 | d11 / 4 s | 8.08 pp | 21.56 pp | 0.867 |
| pos2 | A | 89 | d13 / 10 s | 8.32 pp | 24.63 pp | 0.891 |
| pos2 | B8 | 8 | d15 / 1.5 s | 6.37 pp | 12.59 pp | 0.929 |
| pos2 | B8 | 8 | d17 / 4 s | 6.35 pp | 14.92 pp | 0.881 |
| pos2 | B8 | 8 | d19 / 10 s | 4.66 pp | 11.88 pp | 0.905 |

pos1 is a decisive position: the reference gives `1.0` or `0.0` for all ten tested points, so
every group has zero deviation there. That validates win/loss tagging (scan and independent
search agree on which moves lose), but it says nothing about fine-grained refinement.

Depth trace: `drops = 0` in all runs (no endgame restarts / no depth rollback), so the
"round depth" really is monotonic.

### 3.4 Is it really "more time -> more accurate"?

For pos0 the scan's mean deviation falls 6.49 -> 3.67 -> 2.66 pp with 1.5 -> 4 -> 10 s, and it
picks the same best move and the same top few moves as the independent search. pos2 is the
counter-example to oversell: the scan is stuck around 7.6-8.3 pp even at d13, while a
narrower 8-root multiPV keeps improving (6.4 -> 4.7 pp) because spreading a fixed budget
over 89 roots buys much less depth per root. This is the honest trade-off of
`YXSEARCHDEFEND`, and it matches the reference engine's behaviour — it is not a defect of the
host.

---

## 4. Things that cannot be reproduced under the current Rapfi protocol

1. **"Expand the candidate area while the search runs."** There is no command for it.
   `INFO CAUTION_FACTOR` rebuilds the `Board` and clears the hash (the search restarts), and
   the reference GUI never steps it. The real growth mechanism is the per-move stencil union
   in `Board::move`; the closest reproducible substitute is to play the move (or re-send
   `YXBOARD`) and scan again, which is what the host does for a new board.
2. **GUI-side per-PV tag painting.** Not engine behaviour; reproduced host-side via `livePv`
   (per-`INFO PV DONE` publish + stale-depth cleanup at round end).
3. **`INFO SHOW_DETAIL 3` vs `2`.** Level 3 additionally emits `MESSAGE REALTIME POS/DONE`
   while a root move is being searched. Our UI does not render "scanning now" on that per-move
   basis, so `2` is kept; this is a visual difference only, not a evaluation difference.
4. **`CAUTION_FACTOR 3` as a gradual expansion knob.** It is a one-shot board reconstruction
   (`command/gomocup.cpp`), not a search-time expansion; it is set once per scan ask, same as
   the reference.

---

## 5. Conclusion

* The reference model is: **candidate area = union of stone stencils, fixed during a search;
  refinement = iterative deepening of the same root set, published PV by PV.**
* The desktop now reproduces that: identical candidate coordinates to the reference
  (set difference 0 after the `flipY_X` correction), identical root-set filtering, per-PV
  progressive publishing with stale-depth cleanup, no trailing half-round truncation.
* "More time is more accurate" holds where the root set is narrow enough to deepen (pos0:
  6.5 -> 2.7 pp mean deviation); on dense scans (89 roots) the extra time buys depth
  elsewhere and the deviation plateau is expected, exactly as with the reference binary.

## 6. Reproducing the measurements

Scripts (kept out of the repo, they need a local engine build):

* `.workbuddy/tmp/scan-ab/drive_engine.py` — drive the engine over stdin/stdout and dump
  `INFO PV` rounds to JSON (`--pos`, `--detail`, `--caution`, `--threads`, `--hashkb`).
* `.workbuddy/tmp/scan-ab/scan-ab.js` — stage `ab` (A = `YXSEARCHDEFEND`, B8/B16 = `YXNBEST`),
  stage `c` (per-point reference search, deviation + Spearman).
* `.workbuddy/tmp/scan-ab/summarize.py` — candidate count / bbox / fill / set-difference tables.

Raw captures are under `data/`: `our_asym.json`, `ref_asym.json`, `our_open.json`,
`ref_open.json`, `our_line2.json`, `our_spread2.json`, `scan-ab-pos{0,1,2}-result.json`.
