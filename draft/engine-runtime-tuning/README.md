# Call layer, hash and thread tuning (4.514)

Three measured questions: does our host/engine plumbing beat the reference build,
where are the hash-size lower/upper bounds, and where is the thread-count knee.

Harness: `tools/engine-ab.js` (interleaved fixed-time) and `tools/engine-call-layer.js`
(same protocol, latency/memory probes). Machine: Ryzen 7 8845H, 8 physical / 16 logical
cores, 32 GB RAM, Windows 11. Reference binary:
`C:\Users\harve\Desktop\document\Rapfi-YixinBoard\engine.exe` (Rapfi 0.43.01, MinGW, AVX2).

## 1. Protocol-level latency and memory (8 threads, 512 MB, 5 trials)

| metric | ours AVX512-VNNI | ours AVX2 | reference 0.43.01 |
|--------|------------------|-----------|-------------------|
| spawn -> first stdout line | 68.9 ms | 65.6 ms | 4.2 ms |
| search command -> first `INFO DEPTH` | 197.7 ms | 193.6 ms | 191.8 ms |
| `STOP` -> best move | 0.72 ms | 0.93 ms | 0.87 ms |
| RSS, 1 s after search start | 558 MB | 557 MB | 563 MB |
| RSS growth over 30 s search | +5 MB | +4 MB | +6 MB |

The 4.2 ms "init" of the reference is its banner, printed before weights are loaded; the
first real search line is the comparable metric and it is a tie (192 vs 198 ms). Stop
latency and memory growth are also ties. Conclusion for call layer: parity on latency,
no leak, no host-side stall.

### Same-ISA throughput (AVX2 vs AVX2, 8 threads, 10 s, 3 reps, 2 positions)

| position | ours AVX2 | reference AVX2 | delta |
|----------|-----------|----------------|-------|
| 5 stones | 1516k nps / d25 | 1366k nps / d26 | +11% nps |
| 9 stones | 2897k nps / d34 | 2632k nps / d33 | +10% nps |

### Our AVX512-VNNI vs the reference AVX2 (same 8 threads, 10 s)

| position | ours | reference | delta |
|----------|------|-----------|-------|
| 5 stones | 1970k / d25 | 1521k / d25 | +29% nps, same depth |
| 9 stones | 2966k / d34 | 2481k / d31.5 | +20% nps, +2.5 ply |

So the legitimate advantage decomposes into ~10% from the host/build path (same ISA)
plus the AVX512-VNNI ISA itself. Both are throughput, not accuracy claims.

## 2. Hash size - measured sweep and bounds

### Short searches (8 threads, 8 s, 3 positions, 2 reps, median)

| hash | nps | depth |
|------|-----|-------|
| 64 MB | 1862k | 27-49.5 |
| 128 MB | 1939k | 26.5-45 |
| 256 MB | 1967k | 26-48.5 |
| 384 MB | 1977k | 26-46 |
| 512 MB | 1854k | 26.5-45 |
| 1024 MB | 2018k | 26-49 |
| 2048 MB | 1850k | 26-50.5 |

### Long search (8 threads, 30 s, 9-stone position, 2 reps, median)

| hash | nps | depth |
|------|-----|-------|
| 64 MB | 1661k | 33.5-52 |
| 128 MB | 1805k | 33.5-48 |
| 256 MB | 1676k | 32-49 |
| 512 MB | 1659k | 33.5-56 |
| 1024 MB | 1636k | 34.5-54.5 |

No size in 64 MB..2048 MB changes depth beyond noise, even at 30 s. There is no
cache/TLB penalty visible above 512 MB either (the engine allocates large pages, so
TLB pressure is not the limiter here).

### Quantitative bounds

TT layout: one bucket = 64 bytes = 5 entries (12 bytes + padding), so
`entries = hashMB * 16,384 buckets * 5`; capacity per MB is ~81,920 positions.

- Lower bound (capacity): a turn writes at most ~`nodes/3` distinct positions (the rest
  are re-visits). `hashMB_min ~= nodes_per_turn * 12 / (3 * 2^20) = nodes * 4 bytes / MB`.
  At 2.5 M nps and a 30 s turn this is ~286 MB; short 8-10 s turns need <100 MB.
  Measured: 64 MB already loses nothing, so 128 MB is a safe floor with headroom.
- Upper bound: the longest supported turn on this hardware is bounded by RAM and by
  diminishing returns. Measured 128-1024 MB identical; the host cap is
  `min(4096 MB, 0.8 * RAM, RAM - 2048 MB)` = 4 GB on a 32 GB machine. Beyond that is
  wasted address space, not speed.

Recommended default / formula (already what the host does, verified to match):
`hashMB = clamp(512, 128, min(4096, 0.8*RAM, RAM-2048))` for >=16 GB machines,
`clamp(256, 128, ...)` for 8-16 GB, `128` below 8 GB; keep 512 as the default on this
class of machine because measurements show no gain above it and 512 keeps 30 s turns
safely inside capacity.

## 3. Thread count - sweep and knee

8 threads vs 12 vs 16 in the same run (8 threads is the physical-core count; the host
pins the engine to physical cores):

| threads | nps | selective depth (median) |
|---------|-----|--------------------------|
| 1 | 286k | 24-45 |
| 2 | 550k | 26-46.5 |
| 4 | 1157k | 27-45.5 |
| 6 | 1621k | 28-45 |
| 8 | 1957k | 25.5-47 |
| 12 | 2398k | 25.5-44.5 |
| 16 | 2816k | 26-41 |

Confirmation (10 s, 3 positions, 3 reps):

| threads | nps | selective depth |
|---------|-----|-----------------|
| 8 | 2079k | 27-51 |
| 12 | 2517k | 26-48 |
| 16 | 2930k | 27-50 |

NPS scales with SMT up to 16 (8 -> 16 is +40%), but depth does not improve at all and
the 16-thread run is slightly worse on selective depth because 16 Lazy-SMP workers
oversubscribe 8 pinned physical cores. The knee for search quality is therefore the
physical-core count; the host's default `threadsMax = physical cores` is correct.
Policy: `threads = min(physical cores, 16)`, never logical cores; keep one core free
for the host/vision threads when the machine has <=4 physical cores.

## 4. Settings audit (desktop panel)

Every engine-side setting is re-sent on each ask, so a change applies to the next
search without restarting the app:

| setting | path / evidence | verdict |
|---------|-----------------|---------|
| threads | `engineConfig` -> `SetUserPrefs` -> `INFO THREAD_NUM` | effective, immediate |
| hash | same path -> `INFO HASH_SIZE` | effective, immediate |
| rule | `BuildAskCmds` compares with `g_appliedRule`, re-sends `INFO RULE` | effective, next ask |
| candidate range | re-sent `INFO CAUTION_FACTOR` (default 3) + `YXSEARCHDEFEND` | effective, next ask |
| max depth | re-sent `INFO MAX_DEPTH` when >= 2 | effective, next ask |
| max nodes | re-sent `INFO MAX_NODES` when > 0 | effective, next ask |
| think time | per-ask `INFO TIMEOUT_TURN` / `TIME_LEFT` | effective, immediate |
| analysis points (multi-PV) | per-ask `topN` -> `YXNBEST` | effective, next ask |
| weights variant | `engineConfig` hot-swaps the engine before the next ask | effective, one ask delay |
| background thinking | per-ask `INFO PONDERING` | effective, next ask |
| board rules / AI side / link options / theme / font / language | page state only | effective, persisted |

Two real "fake display" cases were found and fixed in `ui/calc.js`:

1. **Hash**: when the stored value was not one of the dropdown values, or exceeded the
   RAM cap, the picker showed the fallback while the host kept using the stored value.
   Now the stored value is clamped to the shown value and re-applied through
   `engineConfig`, so picker and engine always agree.
2. **Threads**: an old profile could hold 10/12 on an 8-core machine; the picker showed
   the clamped 8 but the stored value stayed 10/12. Now the stored value is clamped
   visibly and re-applied.

Both fixes are in the 4.514 installer (host exe and ui.pak rebuilt from the fixed
source). Engine-side settings were additionally verified by the protocol traces above
(thread/hash sweeps show the engine really receives and honours the values).

Raw samples: `data/call-layer.json`, `data/hash-sweep.json`, `data/hash-long.json`,
`data/thread-sweep.json`, `data/thread-confirm.json`.
