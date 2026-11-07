# Rapfi compile-level optimization sweep (4.514)

Goal: find the remaining ~10% that was previously attributed to engine version / codegen (not ISA),
using compiler and linker options only. AVX512-VNNI was already in the shipping build.

## Shipping baseline (unchanged, used as reference in every A/B)

- clang-cl 23.1.3 from the portable LLVM toolchain, Release, `/clang:-O3`
- ThinLTO (`ENABLE_LTO=ON`), `-flto=thin` on compile and link
- `-mavx512f -mavx512dq -mavx512bw -mavx512vl -mavx512vnni -mbmi2 -mlzcnt -mpopcnt`
- `-mprefer-vector-width=512`
- No command modules, `-ffp-contract=off`

Previously rejected on this code base and **not retried**: PGO, `-ffast-math`, the MSVC (`cl.exe`) build.

## Candidates

| id | change | notes |
|----|--------|-------|
| fulllto | full LTO instead of ThinLTO | `-flto=full` at compile; lld-link links bitcode directly. Configure needs two phases because CMake's try-compile runs while `CMAKE_EXE_LINKER_FLAGS` is already set and lld-link rejects `-clang:`-prefixed flags |
| ltoinline | full LTO + `-mllvm -inline-threshold=3000` | per-TU threshold; binary grew 1.66 MB -> 4.31 MB |
| align6 | `-mllvm -align-all-functions=6` | 64-byte function alignment |
| unroll | `-funroll-loops` | aggressive loop unrolling |

## Protocol

`tools/engine-ab.js`, five variants interleaved in one process per sample, HASH 512 MB,
SHOW_DETAIL 2, 8 threads, 10 s per sample. Positions = 5, 7 and 9 stones on the board
(driver positions 0, 1, 2); the 11-stone position is a forced mate and is excluded from
the throughput table (all variants finish in <1 s). 3 repetitions per variant per position.

Repeat noise band measured on the *baseline itself* in the same run:

| position | baseline spread across 3 reps (first run) | confirm run spread |
|----------|-------------------------------------------|--------------------|
| 5 stones | 8%  | 11% |
| 7 stones | 10% | 20% |
| 9 stones | 15% | 3%  |

## First run (8 threads, 10 s, 3 reps) - median NPS

| position | base | fulllto | align6 | unroll | ltoinline |
|----------|------|---------|--------|--------|-----------|
| 5 stones | 2030k | 1971k (-2.9%) | 1940k (-4.4%) | 2002k (-1.4%) | 1997k (-1.6%) |
| 7 stones | 2401k | 2514k (+4.7%) | 2371k (-1.2%) | 2468k (+2.8%) | 2196k (-8.5%) |
| 9 stones | 1929k | 2008k (+4.1%) | 1948k (+1.0%) | 1992k (+3.3%) | 1893k (-1.9%) |

Median depth moved by at most +-2 ply and not consistently in favour of any candidate.

## Confirmation run (fulllto only, 8 threads, 8 s, 5 reps)

| position | base | fulllto | delta |
|----------|------|---------|-------|
| 5 stones | 1949k | 1930k | -1.0% |
| 7 stones | 2389k | 2455k | +2.8% |
| 9 stones | 2124k | 2072k | -2.4% |
| all | 2124k | 2072k | -2.4% |

The +4-5% seen for full LTO on two positions in the first run did not reproduce: the
confirmation run is net negative. Every candidate stays inside the baseline's own
repeat-to-repeat spread.

## Fixed-depth sanity (depth 16, 1 thread)

Best move and eval are identical between baseline and fulllto on all three positions
(`7,8 / -217`, `6,6 / -149`, `10,6 / -45`). No evaluation drift, so the candidates are
behaviour-preserving - they simply do not buy throughput.

## Verdict

All four compile-level changes are **rejected**: none exceeds the interleaved repeat
noise band, and the only candidate with a positive signal (full LTO) flipped sign under
confirmation. The shipping binary remains the AVX512-VNNI + ThinLTO build already used
by 4.514; no build flag was changed in this release.

Raw samples: `data/compile-ab-8t.json`, `data/compile-ab-confirm.json`,
`data/compile-fix16.json`.
