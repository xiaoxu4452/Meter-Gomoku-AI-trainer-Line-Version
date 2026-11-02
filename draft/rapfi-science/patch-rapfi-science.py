# Patch draft engine_host.cpp with scientific-review fixes.
# Each replace asserts the old text exists exactly once; fails loudly otherwise.
import io, sys

P = r"C:\Users\harve\Desktop\Gobang auto\meter-gomoku-repo\draft\rapfi-science\engine_host.cpp"
s = io.open(P, "r", encoding="utf-8").read()
orig = s

def rep(old, new):
    global s
    n = s.count(old)
    assert n == 1, "pattern count=%d for: %r" % (n, old[:70])
    s = s.replace(old, new)

# 1) Bench uses 1 thread: relative ranking stable, launch CPU storm cut.
rep(
"  CpuCaps caps = CpuIdCaps();\n  int threads = EffectiveThreads();",
"  CpuCaps caps = CpuIdCaps();\n  // [SCI] Benchmark with a single thread: relative variant speed ranking is\n  // stable across thread counts, so a full-core bench is wasted CPU at launch.\n  int threads = 1;")

# 2) One bench round per variant instead of three.
rep(
"  for (int r = 0; r < 3 && g_benchRun; ++r)",
"  // [SCI] One round per variant is enough for ranking (median of 3 adds little).\n  for (int r = 0; r < 1 && g_benchRun; ++r)")

# 3) Log line reflects one round.
rep(
'ELOG("auto-bench（T" + std::to_string(threads) + " 节点/局，3 轮中位数）: " + log + "→ " + best);',
'ELOG("auto-bench（T" + std::to_string(threads) + " 节点/局，1 轮）: " + log + "→ " + best);')

# 4) Bench turn cap 1000ms -> 500ms.
rep('"INFO TIMEOUT_TURN 1000\\n"', '"INFO TIMEOUT_TURN 500\\n"')

# 5) Bench round wall cap 6s -> 4s.
rep("const DWORD t0 = NowMs();\n  while (NowMs() - t0 < 6000) {",
    "const DWORD t0 = NowMs();\n  while (NowMs() - t0 < 4000) {")

# 6) Default search threads = physical cores - 1 (keep one core for UI/OS).
rep(
"""static int DefaultThreads() {
  int t = host::ThreadsMaxHost();
  return t < 1 ? 1 : t;
}""",
"""static int DefaultThreads() {
  // [SCI] Default to physical cores - 1: keep one core for UI/OS instead of
  // saturating the CPU. Users can still raise it via SetUserPrefs.
  int t = host::ThreadsMaxHost();
  int phys = host::PhysCores();
  if (t >= phys) t = phys - 1;
  return t < 1 ? 1 : t;
}""")

# 7) RAM-adaptive default hash size; weak PCs get a light table.
rep(
"static int DefaultHashKB() { return 512 * 1024; }",
"""static int DefaultHashKB() {
  // [SCI] RAM-adaptive default: >=16 GB -> 512 MB, >=8 GB -> 256 MB, else 128 MB.
  long long ramMB = host::TotalPhysKB() / 1024;
  if (ramMB >= 16384) return 512 * 1024;
  if (ramMB >= 8192)  return 256 * 1024;
  return 128 * 1024;
}""")

# 8) Clamp floor lowered to 128 MB so small-RAM machines can go light.
rep(
"if (kb < 262144) kb = 262144;",
"if (kb < 131072) kb = 131072;  // [SCI] floor 128 MB (was 256 MB)")

assert s != orig, "no change applied"
io.open(P, "w", encoding="utf-8", newline="\n").write(s)
print("PATCH OK, %d edits applied" % (sum(1 for _ in [1,2,3,4,5,6,7,8])))
