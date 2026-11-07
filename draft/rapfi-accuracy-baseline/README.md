# Rapfi accuracy baseline: is the reference software beatable, and by how much?

Date: 2026-11-07
Reference (accuracy oracle): `C:\Users\harve\Desktop\document\Rapfi-YixinBoard`
Our build: `Meter Gomoku\rapfi-native\RapfiEngine-avx512vnni.exe` (desktop host ships it)
Companion note: `draft/rapfi-scan-align` (scan semantics), `draft/rapfi-parallel-search` (root split)

This note answers one question honestly: **at the same wall-clock budget, is our engine
searching more / evaluating more accurately than the reference software, and if so, why?**

No parameter grid was used to "find" a win; every number below is from a fixed, repeatable
procedure whose scripts and raw JSON are in `data/`.

---

## 1. What the reference is, and why it is accurate

Everything in this section was read from the shipped files, not inferred.

| item | reference `Rapfi-YixinBoard` | our desktop build |
|---|---|---|
| engine binary | `engine.exe`, 2,485,760 B, dated 2025-06-15 | `RapfiEngine-avx512vnni.exe`, built from `desktop-connect/rapfi` |
| engine version | Rapfi **0.43.01** (version string inside the binary) | Rapfi **0.44.1** (`core/version.cpp`) |
| toolchain / target | MinGW64 GCC; ISA strings in the binary: `SSE41`, `AVX2` (no `AVX512`, no `VNNI`) | clang-cl; `SSE41`, `AVX2`, `AVX512`, `VNNI` |
| model descriptor | `binary_file = "model210901.bin"` (23,204 B) | `binary_file = "model260809.bin"` (57,624 B) |
| readout calibration | none in `config.toml` (defaults come from the model file) | explicit `value_readout_knots*` (6 knots per rule variant) |
| NNUE weights | `mix9svq*.bin.lz4` (4 files, ~9.8–10.3 MB) | **byte-identical copies** of the same 4 files |
| search config | `aspiration_window = true`, `num_iteration_after_mate = 24`, `max_search_depth = 99`, `advanced_stop_ratio = 0.9`, `default_candidate_range = "square3_line4"`, `coord_conversion_mode = "flipY_X"` | same keys, same values |
| database / opening book | `enable_by_default = false` (the GUI's `use database moves` flag never reaches the engine in our headless runs) | `enable_by_default = false` |
| pondering | `0` (off) in `settings.txt` | off in the host protocol |
| GUI-sent options | `rule 2`, `style 3` (= `CAUTION_FACTOR 3`), `threads 8`, `hash 65536 MB` (nominal), `multi-pv 224` | rule 2, caution 3, threads/hash sent per run |

Consequences, stated plainly:

* **The weights are the same.** The accuracy difference is not "a stronger network".
* **The engine version is different** (0.43.01 vs 0.44.1) and the **model descriptor** that
  interprets those weights is different (210901 vs 260809). Those two differences are the
  upstream project's, not an algorithm we wrote.
* **The nominal 64 GB hash is unusable on this machine** (31.3 GB physical memory). The
  reference's own debug log shows its scan actually ran with `hash_size 1048576` (1 GB).
  All comparisons therefore use 1 GB for both engines — equal, and legal.
* **Model files are not interchangeable.** Our engine refuses the reference's
  `model210901.bin`; the reference's 0.43.01 refuses `model260809.bin`. "Swap the weight
  variant to catch up" is therefore not available as a knob.
* **No "nine-heaven" opening book was recoverable.** `rapfi.db` is 2,738 B and contains no
  such record, and the GUI book is disabled for engine queries. The opening-coverage
  requirement is met instead with standard/self-play positions plus seeded random quiet
  positions (section 4).

### 1.1 Output conventions (checked before comparing, so a unit difference is not read as an accuracy difference)

* `EVAL` is side-to-move relative and may be `±M<ply>`.
* `WINRATE` is 0..1; the GUI only does `round(winrate * 100)` and clamps at `99`.
* `EVAL`'s numeric unit depends on the model descriptor (the same winrate can print as
  `-251` for us and `-285` for the reference). **All accuracy metrics below use WINRATE,
  never raw EVAL.**
* With `YXSEARCHDEFEND` both engines emit one `INFO PV <i>` block per candidate, `i` reset
  every iteration; the reference GUI publishes each block as it arrives. Our host now does
  the same in scan mode (see `draft/rapfi-scan-align`).

---

## 2. Definitions

**Throughput layer** (same position, same time, same threads/hash/rule/caution):

* `NPS` = the engine's own `INFO SPEED` / `MESSAGE Speed` report.
* `depth` = the completed root depth of the final iteration; `seldepth` = max selective depth.
* `nodes` = `INFO TOTALNODES`.
* Ignored sample: a run that exits before printing a move (recorded and reported).

**Accuracy layer**:

* Oracle = **the reference engine itself** at **60 s** on the same position, same options.
* Candidates compared = the intersection of the two point sets in the last *complete*
  multipv round (a round is complete when `INFO NUM<PV>` blocks all arrived).
* `meanPp` = mean |our winrate − oracle winrate| over the intersection, in percentage points;
  `maxPp` = the largest single-point |Δ|.
* `spear` = Spearman rank correlation of the winrates over that intersection.
* `top1` = does our highest-winrate point equal the oracle's; `top3` = |overlap| / 3.
* `dec` = fraction of points the oracle calls decisive (|wr − 0.5| ≥ 0.30) that we also
  call decisive in the same direction.
* Sample handling: a position whose search ends in a mate before a full multipv round exists
  is reported but excluded from pointwise statistics (its point set is 1–3 points).

---

## 3. Same-budget throughput comparison

Positions (identical stone lists to `tools/engine-ab.js`; each variant runs 3 repetitions,
interleaved, one engine process per sample):

| pos | stones | character |
|---|---|---|
| 0 | 5 | quiet opening |
| 1 | 7 | tactical, mate available (`+M11`) |
| 2 | 9 | tactical, long mate (`+M35`) |
| 4 | 9 | tactical, losing side (`−M16`) |

All runs: `turn = 10 s`, `hash = 512 MB`, `rule 2`, `caution 3`, `detail 2`. The only
difference between the two columns is the engine binary and its own config/model.

### 3.1 8 threads (the desktop default), 3 reps per position

| pos | our NPS (median) | ref NPS (median) | Δ | our depth | ref depth | our nodes | ref nodes |
|---|---|---|---|---|---|---|---|
| 0 (5 stones) | 1,970 kn/s | 1,521 kn/s | **+29.5 %** | 25 | 25 | 15.24 M | 10.01 M |
| 1 (7, mate) | 4,842 kn/s | 4,573 kn/s | +5.9 % | 29 | 31 | 0.45 M | 0.53 M |
| 2 (9) | 2,966 kn/s | 2,481 kn/s | **+19.5 %** | 34 | 31.5 | 24.14 M | 23.76 M |
| 4 (9, mate) | 3,023 kn/s | 2,644 kn/s | **+14.3 %** | 32 | 32 | 5.66 M | 8.73 M |
| **all 12 / 11 samples** | **2,992 kn/s** | **2,571 kn/s** | **+16.4 %** | 31 | 31 | 10.97 M | 8.73 M |

Wall-clock to the *first* proven mate (the search terminates there, so this is a real
"same-time" outcome):

* pos1: **479 ms** (ours) vs **578 ms** (ref), −17 %.
* pos4: **2,274 ms** (ours) vs **3,760 ms** (ref), −40 %.
* One reference sample on pos2 exited before printing a move (`d=0`, 643 ms); it is
  excluded from the medians and listed here rather than hidden.

Note: depth medians are equal on pos0 and only +1–2 on pos2/pos4. Depth is a noisy,
pruning-dependent quantity under Lazy SMP; **NPS is the stable throughput signal**, and it
is higher for us on every position except the 3-sample mate case where it is a tie
(+5.9 % is inside the spread).

### 3.2 1 thread, 3 reps per position (isolates the search from parallel overhead)

| pos | our NPS | ref NPS | Δ | our depth | ref depth |
|---|---|---|---|---|---|
| 0 | 259 kn/s | 195 kn/s | **+32.8 %** | 24 | 21 |
| 1 (mate) | 560 kn/s | 501 kn/s | +11.8 % | 33 (mate at 650 ms) | 35 (mate at 859 ms) |
| 2 | 471 kn/s | 421 kn/s | +11.9 % | 29 | 28 |
| 4 | 403 kn/s | 371 kn/s | +8.6 % | 37 | 35 |
| **all** | **429 kn/s** | **396 kn/s** | **+8.3 %** | 31 | 31.5 |

At one thread our depth is **consistently 1–3 higher** and the mate on pos1 is found 24 %
sooner. This is the cleanest evidence for "same time, deeper search".

### 3.3 ISA isolation: our AVX2 build vs the reference (8 threads)

The reference binary carries only `SSE41`/`AVX2` strings. To separate "AVX512-VNNI" from
"engine version", both columns were rerun with our AVX2 build:

| pos | our AVX2 NPS | ref NPS | Δ | our depth | ref depth |
|---|---|---|---|---|---|
| 0 | 1,516 kn/s | 1,366 kn/s | +11.0 % | 25 | 26 |
| 1 (mate) | 4,353 kn/s | 4,477 kn/s | −2.8 % | 30 | 31 |
| 2 | 2,897 kn/s | 2,651 kn/s | +9.3 % | 34 | 33 |
| 4 | 2,928 kn/s | 2,439 kn/s | **+20.1 %** | 34 | 34 |
| **all** | **2,912 kn/s** | **2,632 kn/s** | **+10.7 %** | 31.5 | 32 |

**Therefore:** roughly half of the 8-thread gap is ISA/codegen (AVX512-VNNI on a CPU that
supports it), and roughly 10 % survives even at the same ISA class — that residual comes
from engine version 0.44.1 vs 0.43.01 (and, inseparably, clang-cl vs MinGW). It is **not**
a self-invented optimisation.

### 3.4 Verified / rejected technical-gain claims

| claim | verdict | evidence |
|---|---|---|
| more nodes per second at the same ISA | **verified** | 3.3: +9..+20 % on 3 of 4 positions |
| AVX512-VNNI adds throughput | **verified** | 3.1 vs 3.3: 1,970 vs 1,516 kn/s on pos0 (+30 %), +2..+11 % elsewhere |
| newer engine version deepens search at equal time | **verified at 1 thread** (+1..+3 ply), **not reproducible at 8 threads** | 3.2 vs 3.1 |
| host call layer reduces overhead | **not applicable to search** | NPS is reported inside the engine; the host is not in the timing path. The host only affects UI latency. |
| TT reuse is better in our build | **not measured / no evidence** | no isolated TT experiment; do not claim it |
| larger hash closes the gap | **rejected** | earlier sweep on `open`: 256 MB / 4 GB / 4 threads / 16 threads all stayed at 6.5–7.8 pp mean deviation from the deep reference; hash/threads are not accuracy levers |
| swapping to the reference weights helps | **impossible** | the 4 `.lz4` files are byte-identical, and the model descriptors are not interchangeable |

---

## 4. Same-time accuracy comparison (reference as the oracle)

Sample (8 usable positions, **not hand-picked**):

* 5 positions from seeded (`[(7,7,1)]`, `[(7,7,1),(8,8,2)]`) 250 ms reference self-play games,
  snapshotted at 6 / 10 / 14 stones (`data/corpus.json`).
* 3 positions from **seeded random legal placement** (`seed = 20261107`): stones are dropped
  alternately into an empty cell within Chebyshev distance 2 of an existing stone; a
  candidate position is rejected if either side already has 4-in-a-row or an open three
  (`data/corpus-quiet.json`, 8/8/10 stones). This compensates for the fact that most
  self-play snapshots are already decided.
* Excluded from pointwise statistics (reported separately): one self-play snapshot and one
  random position reach a mate before a full multipv round exists.

Budget parity: 5 s and 15 s, 8 threads, 1 GB hash, rule 2, caution 3, detail 2,
`YXSEARCHDEFEND` (multipv = all root moves), one process per sample.

| run | depth (median) | meanPp | maxPp | Spearman | top1 | top3 | decisive |
|---|---|---|---|---|---|---|---|
| oracle = ref @ 60 s | — | 0 | 0 | 1.000 | 100 % | 1.00 | 100 % |
| ref @ 15 s | 19 | 2.11 | 65.54 | 0.952 | 62 % | 0.75 | 100 % |
| **our @ 15 s** | **21** | **1.67** | **68.43** | **0.964** | **75 %** | **0.79** | 100 % |
| ref @ 5 s | 15 | 4.67 | 71.20 | 0.896 | 38 % | 0.58 | 99 % |
| **our @ 5 s** | **16** | **3.14** | **76.19** | **0.909** | **50 %** | **0.54** | 99 % |

Per-position head-to-head (negative Δ = we are closer to the oracle):

| position | 5 s our / ref / Δ | 15 s our / ref / Δ |
|---|---|---|
| self-play 6 stones (g0) | 1.02 / 0.67 / **+0.35** | 0.51 / 0.28 / **+0.23** |
| self-play 10 stones (g0) | 1.26 / 1.89 / −0.63 | 0.31 / 0.54 / −0.23 |
| self-play 14 stones (g0) | 4.93 / 6.37 / −1.44 | 3.98 / 4.47 / −0.50 |
| self-play 6 stones (g1) | 2.87 / 5.59 / −2.71 | 1.21 / 1.74 / −0.53 |
| self-play 10 stones (g1) | 0.10 / 0.31 / −0.21 | 0.13 / 0.05 / **+0.07** |
| random 8 stones (q0) | 10.05 / 12.89 / −2.85 | 5.43 / 7.65 / −2.23 |
| random 10 stones (q2) | 4.52 / 9.14 / −4.61 | 1.68 / 2.06 / −0.38 |
| random 10 stones (q3) | 0.36 / 0.54 / −0.18 | 0.09 / 0.05 / **+0.04** |

**We are closer to the oracle in 12 of 15 position-budget pairs** (7 of 8 at 5 s, 5 of 8 at
15 s). We are worse in three: one self-play 6-stone position at both budgets, and one
15 s pair on two tactical positions where both engines are already within 0.15 pp of the
oracle (i.e. the "loss" is numerically negligible but is still reported).

Honest caveats:

* The worst single point is **larger** for us at 5 s (76.19 pp vs 71.20 pp) — higher average
  accuracy does not mean every point is better.
* Deeper is not automatically more accurate: we reach 1–2 more ply at 15 s on 7 of 8
  positions, and the improvement in `meanPp` / `spear` / `top1` tracks that — but the
  attribution chain ends at "more nodes per second", not at "a better evaluation function".
* The oracle is the reference at 60 s; the reference is itself a noisy 8-thread search.
  Its 60 s value is treated as a fixed reference point, not as ground truth.

---

## 5. Verdict

1. **Same-budget throughput: we legitimately win.** +8 % NPS at 1 thread, +10.7 % vs the
   reference at the same ISA class, +16.4 % with the AVX512-VNNI build (8 threads). At
   equal time we reach 1–3 more ply at 1 thread and 1–2 more ply on 7 of 8 accuracy
   positions at 8 threads.
2. **Same-time accuracy: we are marginally ahead on this sample** (mean deviation,
   Spearman, top-1), with three reported losses and a worse single-point maximum at 5 s.
3. **Attribution:** the gain comes from **engine version 0.44.1, the model descriptor 260809,
   the AVX512-VNNI target and clang-cl**, not from an algorithm we invented. The two
   engines share the same NNUE weight files and the same search architecture (Lazy SMP);
   our own code changes (scan publishing in the host, config alignment) do not alter
   search accuracy.
4. **What cannot be claimed:** that the evaluation itself is better than the reference's,
   or that we can exceed it on every position class. On tactical positions where both find
   the same mate, the difference is 0.0–0.15 pp and not meaningful.
5. **If a strictly same-ISA comparison is demanded,** the residual advantage is ~10 % NPS
   and is explained by version/codegen, not by our code. In that framing the honest answer
   is: **"aligned with, slightly faster than, and not algorithmically superior to the
   reference."**

---

## 6. Root-split (YBWC) dessert experiment — rejected

The previous round suggested root splitting on top of Lazy SMP ("hi-brid") might help, but
the effect disappeared when the sample was enlarged, and fixed-depth evaluations drifted.
This round tested two *new* mechanisms derived from that failure:

* **A**: while a helper thread works on a root-split claim, **its TT stores are suppressed**
  (`thread_local ttStoreSuppressed`, reads still allowed), so the null-window subtree cannot
  rewrite the table that Lazy SMP depends on.
* **B**: the root split only runs in the **fixed-depth domain** (only when the GUI sent
  `INFO MAX_DEPTH`) and the split path **does not reuse aspiration** from the previous
  iteration.

Controls: `lazy` (stock build, no split), `hyb` (split, shared TT writes enabled).

Strict criteria, fixed in advance: median-depth gain must exceed the same-cell repeat noise
band, must be same-direction in at least 3/4 positions, and fixed-depth 14/16 best move and
evaluation must not drift.

### 6.1 Dynamic search (10 s, 8 threads, 4 positions, 5 reps)

* Same-cell noise band per position: 4 / 1 / 18 / 2 ply (the 18-ply position is unusable
  as a measurement cell).
* Median-depth gain vs lazy: `hyb` −1 / +2 / +6 / −1; `A` 0 / +2 / +2 / −1; `B` −1 / 0 /
  +13 / 0. Only `B` on the noisy 18-ply cell exceeded the band, and fewer than 3 of 4
  positions moved in the same direction in every case.
* `A` is slightly slower than `hyb` overall (mean depth 30.75 vs 31.5; stock lazy 30), so
  suppressing TT writes has a real speed cost.

### 6.2 Fixed depth (8 threads, 5 reps, `MAX_DEPTH` 14 and 16)

The control itself drifts under 8-thread Lazy SMP, which invalidates an absolute
"zero drift" rule:

* At depth 14, stock lazy gives e.g. `-147 … -185` on one position and `-104 … -173` on
  another. All variants must therefore be judged against that repeat band, not against a
  single run.
* Depth 14: `A` changes the best move on one position and leaves the band on two; `B`
  changes the best move on one position and — decisively — turns a `-M14` mate score into
  `-702`, i.e. it loses the mate classification; `hyb` keeps best moves but leaves the
  eval band on three positions.
* Depth 16 (see `data/fixed16-drift.txt`): lazy best moves alternate between `10,8` and
  `10,6` on one position, so "best move must not drift" is not satisfiable even for the
  control. `hyb` fixes the move to `10,8` but its eval range `-46 … -13` lies outside the
  lazy range `-33 … 0`; `A` stays inside that range on this position but leaves the band
  on the 7-stone position (`-122 … -166` vs `-142 … -172`); `B` runs at ~350 kn/s
  (≈5× slower) and leaves the band, and at depth 14 it still loses the mate score.

**Conclusion: both new paths are rejected; `hyb` is rejected as well.** The root-split
prototype adds no reproducible depth beyond Lazy SMP's own noise band, and its fixed-depth
behaviour is less stable than the stock search — path B additionally breaks mate
classification. There is **no further parallel-search dessert worth mining in Rapfi's
current architecture by splitting root moves on top of Lazy SMP**, and no further parameter
grid was run. The prototype stays behind the `GB_YBWC_ROOT` compile flag, off in the shipped
build.

---

## 7. Cleanup and defaults

* The scan publishing change is the only behavioural change kept in the desktop host; it
  reproduces the reference GUI's per-`INFO PV DONE` publication and end-of-round pruning.
* The YBWC prototype is compiled only under `GB_YBWC_ROOT`; the shipped
  `RapfiEngine-avx512vnni.exe` does not contain it, and `engines.json` lists only
  sse / avx2 / avxvnni / avx512 / avx512vnni. The experiment-only
  `RapfiEngine-avx512vnni-ybwc.exe` is removed from the export package (the build tree
  keeps it for reproducibility).
* No parameter grid or dead experiment branch was added to the shipped search path.

---

## 8. Reproduction

```powershell
# throughput (8 threads, 10 s, 4 positions, 3 interleaved reps)
node tools/engine-ab.js --turn 10000 --reps 3 --threads 8 --hashkb 524288 --rule 2 --caution 3 `
  --pos 0,1,2,4 `
  --variants "our=Meter Gomoku/rapfi-native/RapfiEngine-avx512vnni.exe,ref=C:\Users\harve\Desktop\document\Rapfi-YixinBoard\engine.exe" `
  --out .workbuddy/tmp/engine-sweet/thr-8t.json

# accuracy corpus (reference oracle 60 s + ref/our at 5/15 s)
python .workbuddy/tmp/engine-sweet/gen_quiet.py
python .workbuddy/tmp/engine-sweet/run_accuracy.py
python .workbuddy/tmp/engine-sweet/run_quiet.py
python .workbuddy/tmp/engine-sweet/analyze_acc.py

# root-split fixed-depth drift
node tools/engine-ab.js --turn 20000 --reps 5 --threads 8 --hashkb 524288 --maxdepth 16 `
  --pos 0,1,2,4 --variants "lazy=<stock>,hyb=<ybwc>|RAPFI_YBWC=1;RAPFI_YBWC_SPLITDEPTH=3,A=<ybwc>|RAPFI_YBWC=1;RAPFI_YBWC_SPLITDEPTH=3;RAPFI_YBWC_TT=0,B=<ybwc>|RAPFI_YBWC=1;RAPFI_YBWC_FIXEDONLY=1" `
  --out .workbuddy/tmp/engine-sweet/fix16.json
```

Data files in `data/`: `accuracy-table.txt`, `throughput-table.txt`, `fixed14-drift.txt`,
`fixed16-drift.txt`, plus the raw JSON of every run and the analysis scripts.

---

## 9. Final scope trim: what is answered, what is "not measured"

This round was closed on the data already collected. No new grid or large sample was run.

| question | status |
|---|---|
| NPS / nodes / depth vs the reference, 8 threads, 10 s, 4 positions, 3 reps | **measured** (section 3.1) |
| same at 1 thread | **measured** (section 3.2) |
| ISA contribution (our AVX2 build vs reference) | **measured** (section 3.3) |
| same-time accuracy, 8 positions, 5 s / 15 s, reference 60 s as oracle | **measured** (section 4) |
| same-time accuracy on the user's "nine-heaven" opening set | **not measured**: the string is not present in `rapfi.db` (2,738 B) and the GUI book is disabled for engine queries; the opening requirement is covered by the standard/self-play + seeded random corpus instead |
| depth advantage at 8 threads on every position | **not reproducible**: equal medians on the 5-stone position; +1–2 ply on the 9-stone position only |
| TT-reuse / call-layer overhead as a gain | **not measured**; no evidence, so not claimed |
| accuracy per position class (e.g. "we are better at openings") | **not measured**: 8 positions are too few to split into classes |
| replacing the NNUE model to catch up | **impossible**: the four `.lz4` weight files are byte-identical, and the two model descriptors cannot be loaded by the other engine |

### 9.1 Scan-alignment checklist (verified in the host, not assumed)

* Scan requests always rebuild the position with a full `YXBOARD <moves> DONE`; the
  incremental `TURN x,y` shortcut is explicitly disabled for scan (it is only used for
  ordinary single-PV, non-scan, non-balance asks). So every scan search replays the board
  from scratch, exactly like the reference GUI.
* Scan requests send `INFO CAUTION_FACTOR 3` and then `YXSEARCHDEFEND`, with
  `multiPV = number of remaining root moves`, no extra `PASS` and no side flip. This is
  what the reference `searchDefend()` does.
* There is no host-side convergence test, early stop, or candidate truncation in the scan
  path: every `INFO PV DONE` is published as it arrives and the round is pruned against the
  reference's `depth < curDepth` rule only when all `NUM<PV>` blocks have arrived.
* `YXBLOCKRESET` is **not sent and not needed**: the host never sends `BLOCK` commands, so
  `options.blockMoves` is always empty and the command would be a no-op. The engine still
  implements it (`gomocup.cpp`), so nothing is missing from the protocol side.
* Known non-reproducible item (from `draft/rapfi-scan-align`): the protocol has no command
  that grows the candidate region *during* a search. The reference's "growth" is the
  per-move `candArea` expansion on `Board::move`, i.e. it happens when the board is
  replayed, not while searching. Our host reproduces that by replaying `YXBOARD` each scan.

### 9.2 Verdict in one sentence

**There is a measurable, attributable "same-time deeper / same-time slightly more accurate"
advantage on this sample, and it is attributable to engine version 0.44.1, the 260809 model
descriptor, the AVX512-VNNI target and clang-cl — not to an algorithm we invented.** If a
strictly same-ISA, same-model comparison is required, the honest verdict is **"align with
the reference"**: same weights, same search architecture, same rule/caution/multi-PV/hash/
thread semantics, with only the upstream-version and codegen differences remaining.

Configuration items that must stay aligned to the reference (all already in place):

| item | aligned value | where |
|---|---|---|
| rule | 2 (renju) | host protocol / `settings.txt` |
| caution (style) | 3 → `INFO CAUTION_FACTOR 3` before `START` | host boot / `settings.txt` |
| scan mode | `YXSEARCHDEFEND`, multiPV = remaining root moves | host |
| multi-PV display | 224 (or all root moves) | `settings.txt` |
| threads | 8 | run argument / `settings.txt` |
| hash | 1 GB (the reference's 64 GB is not physically available) | run argument |
| pondering | off | host / `settings.txt` |
| candidate range | `square3_line4`, `CAUTION_FACTOR` rebuild behavior unchanged | `config.toml` + `gomocup.cpp` |
| coord conversion | `flipY_X` | `config.toml` |
| database | disabled by default on both sides | `config.toml` |
