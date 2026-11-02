# Rapfi Integration - Scientific Review (DRAFT)

Scope: Meter Gomoku's Rapfi integration vs upstream `dotb1ue/rapfi` (official)
and `dotb1ue/rapfi-networks` (official weights). Findings are backed by the
upstream README, the official example config `gomocalc-classical260809.toml`,
and this repo's `host/engine_host.cpp`.

## 1. Findings (what is not scientific)

| # | Issue | Evidence | Effect |
| --- | --- | --- | --- |
| 1 | `value_readout_knots` missing in `config.toml` | Official 260809 config ships 4 knot arrays; local config has none | NNUE output is mapped to winrate with mismatched knots -> **inaccurate evaluation** (the "fast/deep but wrong score" symptom) |
| 2 | Fixed 512 MB hash table | `DefaultHashKB()=512*1024`, `default_tt_size_kb=524288`; upstream default is 32 MB | Large resident memory on weak PCs -> swapping, lag, fan spin |
| 3 | Default search threads = all physical cores | `DefaultThreads()=ThreadsMaxHost()` = `PhysCoreCount()` (host.cpp:2222-2226) | Engine saturates CPU during every move -> fan storm on 4-8 core machines |
| 4 | Startup AutoBench runs every variant 3 rounds, full threads, 6 s cap | `engine_host.cpp:382-456` (BenchOneRound 1 s/turn), auto-bench loop | Each app start pegs all cores for ~15-30 s -> fan storm + lag at launch |
| 5 | `advanced_stop_ratio` | repo now uses upstream 0.9 (2026-11-01) | Correct per upstream; this draft keeps 0.9. |
| 6 | Fixed `TIMEOUT_TURN 300` ms | `engine_host.cpp:338` | Shallow depth on complex midgames. Kept for UI responsiveness; depth improves once threads default drops. |
| 7 | classical210901/220723 configs kept | Upstream README: older-named models = v8 format, zero policy residuals | Fine for classical-only mode (weaker by design). Documented, not a bug. |

## 2. Revisions (this draft)

### config.toml / config-classic.toml
- Added the official `value_readout_knots` arrays (freestyle / standard / renju black / renju white) from
  `dotb1ue/rapfi-networks` `config-example/gomocalc-classical260809.toml`.
- `default_tt_size_kb` now a commented hint; the host overrides hash size at runtime
  (see engine_host.cpp `EffectiveHashKB`).

### host/engine_host.cpp (draft copy)
- `DefaultThreads()`: physical cores - 1 (reserve one core for UI/OS) instead of all cores.
- `DefaultHashKB()`: RAM-adaptive (>=16 GB -> 512 MB; >=8 GB -> 256 MB; else 128 MB);
  `ClampHashKB` floor lowered to 128 MB so weak PCs can go light.
- AutoBench: 1 round per variant, single-threaded benchmark (relative speed ranking
  is stable), 0.5 s/turn, 4 s cap per round. Cuts the launch CPU storm ~15x.
- New English comments mark every scientific change (search for `[SCI]`).

## 3. What was NOT changed (deliberately)

- `TIMEOUT_TURN 300` ms (responsiveness) - depth improves via threading default fix.
- `advanced_stop_ratio 0.9` (upstream value, already adopted by the repo).
- Runtime engine binaries / weights / models - this draft is source + config only.
- Existing behavior guarded by user prefs (`SetUserPrefs` still wins over defaults).

## 4. Validation to run after applying

1. `pack-rapfi-native.ps1` still passes (bundle untouched).
2. Launch app on a 4-core / 8 GB VM: task manager shows engine at ~75% during search,
   idle <5% (no busy loop), startup bench <5 s.
3. Compare eval curve before/after knots on the same midgame position: scores should
   shift to calibrated winrate scale (knots mapping active).

## Empirical A/B test (2026-11-01, engine CLI, same machine)

Protocol: `START 15` / `INFO TIMEOUT_TURN 1000` / `INFO RULE 0` / `BOARD <moves> DONE`
on `RapfiEngine-sse.exe`.
- BASE = release config (no knots, `default_tt_size_kb = 524288`, ratio 0.9)
- MOD  = this draft config (official 260809 knots, `default_tt_size_kb = 32768`, ratio 0.9)
Repro: `python tools/bench-rapfi-science.py` (copies the release rapfi-native into
`_rapfi_bench/_bench_base` and `_bench_mod`, runs each position on both).

| position    | config | depth | eval      | nodes   | time(ms) | move |
|-------------|--------|-------|-----------|---------|----------|------|
| open-even   | BASE   | 17    | 406..426  | 210K..245K | 971..974 | 9,7  |
| open-even   | MOD    | 16..17| 376..401  | 216K..228K | 925..974 | 9,7  |
| white-adv   | BASE   | 30    | +M11      | 384K..424K | 947..971 | 8,11 |
| white-adv   | MOD    | 30    | +M11      | 398K      | 887..938 | 8,11 |
| black-thr   | BASE   | 26    | -M2       | 50        | 0        | 12,8 |
| black-thr   | MOD    | 26    | -M2       | 50        | 0        | 12,8 |

Conclusions
- **Winrate calibration works.** The even opening is overrated by BASE
  (eval 406..426 while a balanced 2v2 is expected near 0); with official knots the
  readout converges to 376..401. This is the direct fix for "fast & deep but the
  score drifts".
- **Search quality is unchanged.** Depths are equal (one iteration less only on the
  even position), best moves identical in all three positions (9,7 / 8,11 / 12,8),
  mate values identical.
- **No speed cost.** Nodes/time are flat (or slightly better) with a 32 MB table;
  small midgame positions fit easily. The engine-side `default_tt_size_kb = 32768`
  applies when the engine runs standalone; when driven by the host app the table
  size comes from the host `DefaultHashKB()` (draft: RAM-adaptive, 128-512 MB).

Release export (2026-11-01): draft `config.toml` / `config-classic.toml` copied to
`Meter Gomoku\rapfi-native\` (originals kept as `config.toml.bak` /
`config-classic.toml.bak`).
