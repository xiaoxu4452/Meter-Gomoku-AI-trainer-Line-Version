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
| 5 | `advanced_stop_ratio 0.94` vs official `0.9` | config compare | 0.94 searches more per turn (better accuracy, more CPU). Kept: accuracy is the goal; documented. |
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
- `advanced_stop_ratio 0.94` (accuracy-first policy, upstream is 0.9).
- Runtime engine binaries / weights / models - this draft is source + config only.
- Existing behavior guarded by user prefs (`SetUserPrefs` still wins over defaults).

## 4. Validation to run after applying

1. `pack-rapfi-native.ps1` still passes (bundle untouched).
2. Launch app on a 4-core / 8 GB VM: task manager shows engine at ~75% during search,
   idle <5% (no busy loop), startup bench <5 s.
3. Compare eval curve before/after knots on the same midgame position: scores should
   shift to calibrated winrate scale (knots mapping active).
