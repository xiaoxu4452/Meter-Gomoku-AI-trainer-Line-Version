# Rapfi Parallel Search - Experiment Log (DRAFT)

Date: 2026-11-06
Machine: AMD Ryzen 7 8845H (8 physical cores / 16 logical threads), Windows 11.
Engine tree: this repository (`rapfi/`), default build flags `NO_COMMAND_MODULES=ON`,
`ENABLE_LTO=ON`, `USE_BMI2=OFF`, clang-cl + Ninja.

Scope: test whether a root-splitting (YBWC-style) search, a larger/adaptive transposition
table, or a split hash can beat the shipped **Lazy SMP** default under equal time budgets.
Only the Lazy SMP path is shipped. Every experiment below was run on an experimental binary
built with `GB_RAPFI_YBWC=1` (`/DGB_YBWC_ROOT`); the normal release build never defines that
macro, so the shipped binaries contain none of this code (`RAPFI_YBWC` is absent from the
release executables; verified by byte search).

## 0. Measurement protocol

- Gomocup protocol over stdin/stdout, `START 15`, `YXBOARD <moves> DONE`, `YXNBEST 1`.
- 10 s per turn (`INFO TIMEOUT_TURN 10000`), 512 MB hash unless stated, `SHOW_DETAIL 3`.
- Four positions: pos0 (early opening), pos2 (midgame tangle), pos3 (one side clearly
  better), pos5 (mate threat).
- Modes are interleaved inside one process run (lazy, split2, split3 ... in rotation), so
  thermal drift and background load hit every mode alike.
- Per cell: median over reps; **noise band** = population standard deviation of depth over
  the repeats of the same cell. A gain counts only if it exceeds that band.
- Raw data: `data/*.json` (this folder). Fields: `d` (completed depth), `sd` (seldepth),
  `nodes`, `speed` (NPS), `ev`, `best`, `wall`, `pi` (position index), `rep`.

## 1. Five ISA variants (release validation)

5000 ms per turn, 4 threads, position 5 (`data/variants-5way-5s.json`).
`avxvnni` is expected to trap on this CPU: AMD Zen 4 has AVX-512 VNNI but not the
VEX-encoded AVX-VNNI, so the process exits with 0xC000001D (illegal instruction). The same
official prebuilt binary traps identically; this is a CPU capability limit, not a build
defect. `tools/smoke-rapfi.js` reports it as SKIP.

| variant | depth | NPS | best move | note |
| --- | --- | --- | --- | --- |
| sse | 23 | 576 K | 7,8 | baseline |
| avx2 | 22 | 731 K | 7,8 | |
| avxvnni | - | - | - | 0xC000001D on Zen 4 (not shipped usable) |
| avx512 | 24 | 1008 K | 7,8 | |
| avx512vnni | 25 | 1070 K | 7,8 | fastest usable tier |

Second pass with the rebuilt binaries, 2 reps (`data/variants-shipped-5s.json`):

| variant | depth (median) | NPS | best move |
| --- | --- | --- | --- |
| sse | 22 | 606.5 K | 7,8 |
| avx2 | 23.5 | 935.5 K | 7,8 |
| avx512 | 25.5 | 978 K | 7,8 |
| avx512vnni | 24.5 | 1025 K | 7,8 |

All tiers agree on the move; AVX-512/VNNI buys depth through NPS, not through a different
eval. The variant ranking is stable within the noise of 2 reps.

## 2. Pure YBWC vs Lazy SMP

`data/ybwc-8t.json`, `data/ybwc-16t.json`. Runtime switch `RAPFI_YBWC=1` turns the
root-split path on; `RAPFI_YBWC_SPLITDEPTH` unset means split every depth (pure YBWC).

| mode | 8t depth | 8t NPS | 16t depth | 16t NPS |
| --- | --- | --- | --- | --- |
| Lazy SMP | 28.5 | 2018 K | 30.0 | 3983 K |
| pure YBWC | 25.0 | 520 K | 25.0 | 569 K |

Pure YBWC loses 3.5 ply at 8 threads and 5 ply at 16 threads, at roughly 1/4 to 1/7 of the
NPS. It also **stops scaling**: 8 -> 16 threads gains only +9.5% NPS (Lazy gains +97% in
this same run). Reason: the root has too few young brothers, helpers spin on the ticket and
then on `siblingsOpen`; split overhead and shared-alpha restarts eat the extra cores.
**Verdict: rejected.**

## 3. Hybrid split (split at shallow depths, Lazy SMP below)

`RAPFI_YBWC_SPLITDEPTH=L` splits the root only while `depth <= L`; deeper iterations fall
back to Lazy SMP. Grid over L = 2,3,4,5 (plus 8, 12, 20 = effectively pure YBWC) and over
helper participation (`-h3`, `-h7`) and gate off (`ng`), 3 reps, 10 s.

8 threads (`data/grid-8t.json`). Median depth per position:

| mode | pos0 | pos2 | pos3 | pos5 | overall depth / NPS |
| --- | --- | --- | --- | --- | --- |
| lazy | 27 | 30 | 35 | 27 | 30.0 / 2237 K |
| split2 | 28 | 31 | 36 | 22 | 29.5 / 2762 K |
| split3 | 26 | 32 | 36 | 27 | 30.0 / 2631 K |
| split4 | 28 | 31 | 36 | 23 | 29.0 / 2501 K |
| split5 | 27 | 30 | 34 | 24 | 29.0 / 2038 K |
| split8 | 26 | 31 | 35 | 23 | 28.5 / 2013 K |
| split12 | 26 | 31 | 36 | 23 | 28.0 / 2397 K |
| split12-h3 | 26 | 30 | 37 | 23 | 30.0 / 2208 K |
| split12ng | 26 | 31 | 36 | 24 | 28.5 / 2684 K |
| split20 | 26 | 32 | 35 | 23 | 29.0 / 2145 K |

16 threads (`data/grid-16t.json`):

| mode | pos0 | pos2 | pos3 | pos5 | overall depth / NPS |
| --- | --- | --- | --- | --- | --- |
| lazy | 25 | 31 | 37 | 27 | 29.5 / 3619 K |
| split2 | 25 | 29 | 37 | 23 | 28.5 / 3412 K |
| split3 | 25 | 31 | 36 | 31 | 31.0 / 3613 K |
| split4 | 27 | 31 | 36 | 29 | 30.5 / 3735 K |
| split5 | 27 | 29 | 35 | 25 | 28.0 / 3512 K |
| split12 | 24 | 30 | 37 | 32 | 30.5 / 3052 K |
| split12-h7 | 25 | 30 | 35 | 32 | 30.5 / 3507 K |
| split12ng | 25 | 30 | 37 | 32 | 30.5 / 3714 K |

Noise band (sigma of depth over the 3 reps, same cell): 0.47 to 2.87 ply for pos0/pos2/pos3,
up to 8.7 ply on the mate-threat position pos5 (mate scores converge late, so depth is
unstable there). Typical cell sigma is ~1.25 ply.

Reading the grid:

- At 8 threads the only "wins" are +1 ply on single positions (split2 on pos0, split3 on
  pos2), all inside the noise band; pos5 is consistently worse (-3 to -5) for every split
  depth except split3.
- At 16 threads split3/split4 show +1.5 to +3.0 ply overall, but only because pos5's lazy
  run happened to end early (27 vs 31/29); the confirm run below removes that artefact.
- No split depth improves NPS beyond Lazy; split2 has the best NPS at 8t (2762 K vs
  2237 K) but loses depth on pos5.

16-thread confirmation, 5 reps, lazy / split3 / split4 only
(`data/confirm-16t.json`):

| mode | pos0 | pos2 | pos3 | pos5 | overall depth / NPS |
| --- | --- | --- | --- | --- | --- |
| lazy | 26 | 31 | 37 | 34 | 32.5 / 4041 K |
| split3 | 27 | 31 | 37 | 33 | 31.5 / 3902 K |
| split4 | 26 | 31 | 37 | 32 | 31.5 / 4115 K |

After 5 interleaved reps the earlier +2 ply on pos5 disappears (+1 on pos0 for split3, -1
on pos5, 0 elsewhere). The mean overall depth is 0.5-1.0 ply lower than Lazy. **Verdict:
no reproducible sweet spot; not adopted.** The parameter that looked best at 8t (split2)
is the worst at 16t, so there is no single constant to ship.

## 4. Fixed-depth drift (accurateness check)

`maxdepth 14` and `maxdepth 16`, 8 threads, same 4 positions, 3 reps
(`data/drift-d14-8t.json`, `data/drift-d16-8t.json`). A parallel search must return the
same best move and the same score as the single-threaded-equivalent Lazy SMP at a fixed
depth; otherwise the pruned window changed the answer.

d14 findings:

| position | lazy | split3 | split4 | split12 |
| --- | --- | --- | --- | --- |
| pos2 best | 10,8 (3/3) | 10,6 (1/3), 10,8 (2/3) | 10,6 (2/3) | 10,6 (1/3) |
| pos3 best | 8,6 (3/3) | 7,6 (1/3) | 8,6 (3/3) | 8,6 (3/3) |
| pos5 eval | +17 / +1 / +1 | -4 / +9 / -4 | -12 / -13 / -11 | +2 / +12 / -8 |

pos5 shows an **evaluation sign flip** at the same nominal depth (lazy positive, split4
negative), and pos2's best move flips between 10,8 and 10,6. d16 keeps the nominal depth but
still splits the best move on pos3 (lazy 8,6 3/3; split3/split4 7,6 or 8,6 mixed) and the
seldepth shrinks (e.g. pos3 d16: lazy 24, split3/4 21). The root-split path therefore
changes the root ordering / aspiration propagation; it is not merely a speed device.
**Verdict: rejected for the default path.**

## 5. Dynamic hash size

Question: does a larger table (or resizing mid-search) buy depth/accuracy on this machine?
Table size is a runtime parameter (`INFO HASH_SIZE`, host `EffectiveHashKB`), so this
experiment required no engine source change; no adaptive-resize code was added.

Sweep at 8 threads, 6 s, dynamic depth (`data/hash-sweep-8t.json`), 128/256/512/1024/2048 MB:

| table | pos0 depth | pos0 NPS | pos1 NPS | pos3 depth | pos4 depth |
| --- | --- | --- | --- | --- | --- |
| 128 MB | 24.5 | 1756 K | 1709 K | 30.5 | 29.0 |
| 256 MB | 25.0 | 1808 K | 1878 K | 31.0 | 28.0 |
| 512 MB | 24.0 | 1831 K | 1542 K | 30.0 | 30.5 |
| 1024 MB | 26.0 | 1725 K | 1361 K | 27.0 | 30.5 |
| 2048 MB | 25.5 | 1726 K | 1324 K | 30.5 | 28.5 |

At fixed depth 22 (`data/hash-d22-8t.json`) every size returns depth 22 and the same
seldepth, so the fall-off is not an accurateness gain; NPS actually drops with size on
pos1 (1147 K at 256 MB -> 945 K at 2048 MB) and pos4 (2042 K -> 1806 K).

16-thread confirm, 10 s (`data/hash-16t-confirm.json`):

| table | pos0 depth / NPS | pos3 depth / NPS |
| --- | --- | --- |
| 512 MB | 27.5 / 2872 K | 37.5 / 5436 K |
| 1024 MB | 26.5 / 2475 K | 37.0 / 5150 K |
| 2048 MB | 27.0 / 2934 K | 37.5 / 5348 K |

1024 MB is consistently a few percent slower than 512 MB; there is no depth compensation.
On this machine every run reports `largePage=0`, so a 1 GB table is backed by 262144 4 KB
pages and the extra misses are TLB pressure, not a hash-bucket or generation problem.
(The table is already reused across iterative-deepening iterations: one `incGeneration()`
per search, `clear()` only between games, so mid-search resizing would throw away exactly
the entries being relied on.) **Verdict: reject larger tables and reject resizing.**

## 6. Split hash (sub-hash tier-2) - refutation

`data/subhash-10s.json` (8 threads, 10 s) and `data/subhash-d14.json` (fixed depth 14).
Measured with an out-of-tree prototype; the patch is not in this tree because it was
rejected before integration.

| position | baseline 10 s | tier-2 10 s | baseline d14 seldepth | tier-2 d14 seldepth |
| --- | --- | --- | --- | --- |
| pos0 | 27 | 25 | 24 | 22 |
| pos1 | 27 | 28 | 24 | 19 |
| pos2 | 30 | 31 | 24 | 22 |

The dynamic-depth pass is a wash (+1/-2/0 ply over 3 reps), while the fixed-depth pass
loses seldepth on every position (24 -> 22/19/22) and changes pos2's best move from 10,8 to
10,6. The root split does not change the shared-table requirement; adding a second tier only
adds indirection and worsens the drift documented in section 4. The older measurements
(10 s depth 28 vs 27, NPS 1846 K vs 1805 K) do not separate from noise. **Verdict:
rejected, not re-measured at larger scale.**

## 7. Thread-scaling efficiency (8 -> 16)

Same binary, same 10 s budget, Lazy vs split modes (`data/grid-8t.json` vs
`data/grid-16t.json`):

| mode | 8t NPS | 16t NPS | 8 -> 16 |
| --- | --- | --- | --- |
| lazy | 2236 K | 3618 K | +61.8% |
| split2 | 2762 K | 3412 K | +23.5% |
| split3 | 2630 K | 3613 K | +37.4% |
| split4 | 2501 K | 3734 K | +49.3% |
| split5 | 2038 K | 3512 K | +72.3% |
| split12 | 2396 K | 3052 K | +27.3% |
| pure YBWC | 520 K | 569 K | +9.5% |

The physical-core boundary is 8; 16 logical threads are SMT siblings, so +60% NPS for Lazy
is already near the practical ceiling. Split modes scale worse because the extra helpers
contend on the root claim counter and the shared alpha, and pure YBWC barely scales at all.
There is no mode where 16 threads removes the SMT ceiling; the inflection point is the
8-core boundary, not a function of split depth.

## 8. Decisions

| item | decision | reason |
| --- | --- | --- |
| Default search | keep Lazy SMP | every alternative loses depth or accuracy; split gains are inside the noise band |
| Pure YBWC | rejected | 3.5-5 ply loss, 1/4-1/7 NPS, no 8->16 scaling |
| Hybrid root split | not adopted | no reproducible gain in 5-rep confirm; fixed-depth best-move/eval drift |
| Helper limit / gate off | not adopted | `-h3`/`-h7`/`ng` all inside noise; gate-off adds contention |
| TT > 512 MB | rejected | no depth gain, 1024 MB slower at 16t; TLB pressure hypothesis |
| Mid-search TT resize | rejected | would clear the table and destroy iterative-deepening reuse |
| Split hash (tier 2) | rejected | fixed-depth seldepth loss and best-move drift |
| Experimental binary | not shipped | release build excludes the code; reproducible with `GB_RAPFI_YBWC=1` |

The experimental code remains in `rapfi/search/ab/search.cpp` and `searcher.h` behind
`#ifdef GB_YBWC_ROOT`, inactive unless a build defines it; the default release binaries
contain no trace of it.

## 9. Reproduction

```bash
# experimental build (does not overwrite the release exe)
GB_RAPFI_YBWC=1 bash tools/build-rapfi-fast.sh avx512vnni
# -> RapfiEngine-avx512vnni.exe is replaced; use GB_RAPFI_SUFFIX=-ybwc to keep both

# runtime switches (experimental binary only)
RAPFI_YBWC=1 RAPFI_YBWC_SPLITDEPTH=3 RAPFI_YBWC_HELPERS=7 RAPFI_YBWC_GATE=1
```

Position suite (1-based board, 15x15): pos0 opening after a few stones, pos2 midgame
tangle, pos3 one-sided advantage, pos5 mate threat. Each experiment ran with modes
interleaved in one process so that the CPU state is shared.

## 10. Data index

| file | content |
| --- | --- |
| `data/variants-5way-5s.json` | 5 ISA variants, 5 s, 4 threads, 1 rep |
| `data/variants-shipped-5s.json` | rebuilt variants, 5 s, 4 threads, 2 reps |
| `data/vnni-classical.json` | post-rebuild avxvnni functional check, 2 s, 1 thread |
| `data/ybwc-8t.json` | pure YBWC vs Lazy, 8 threads, 3 reps |
| `data/ybwc-16t.json` | pure YBWC vs Lazy, 16 threads, 3 reps |
| `data/ybwc-hybrid-8t.json` | early hybrid L=12/20 vs Lazy vs pure YBWC, 8 threads |
| `data/ybwc-hybrid-16t.json` | same at 16 threads, 2 reps |
| `data/grid-8t.json` | split-depth/helper/gate grid, 8 threads, 3 reps, 10 s |
| `data/grid-16t.json` | same grid, 16 threads, 3 reps, 10 s |
| `data/confirm-16t.json` | Lazy / split3 / split4, 16 threads, 5 reps, 10 s |
| `data/drift-d14-8t.json` | fixed depth 14 drift, 8 threads, 3 reps |
| `data/drift-d16-8t.json` | fixed depth 16 drift, 8 threads, 3 reps |
| `data/hash-sweep-8t.json` | table size sweep, 8 threads, 6 s |
| `data/hash-d22-8t.json` | table size sweep at fixed depth 22 |
| `data/hash-16t-confirm.json` | 512/1024/2048 MB, 16 threads, 10 s |
| `data/subhash-10s.json` | split-hash tier 2 vs baseline, 8 threads, 10 s |
| `data/subhash-d14.json` | split-hash tier 2 at fixed depth 14 |

## 11. Distribution integrity

- Shipped binaries and `rapfi-native/engines.json` are built from the source in this
  repository with the flags above; SHA-256 of each variant is recorded in `engines.json`.
- `COPYING-GPLv3.txt` and `NOTICE.txt` are kept as-is; the GPL header in
  `rapfi/search/ab/search.cpp` and `searcher.h` is unchanged.
- The experimental `-ybwc` executable is intentionally not distributed; its source is in
  the repository behind `GB_YBWC_ROOT` so anyone can rebuild it under GPL-3.0.
