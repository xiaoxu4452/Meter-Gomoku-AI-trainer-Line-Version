// Meter Gomoku Link — v4.43
// engine_host.cpp —— Rapfi "极速版" 引擎调用层实现（宿主侧唯一权威）
// 契约见 engine_host.h；方案见 .trae/documents/rapfi-引擎调用层重建方案.md
//
//   1) 小工具 / 请求 JSON 取值
//   2) 全局状态
//   3) ISA 探测与变体降档（.variant-ok 缓存 + 冒烟）
//   4) 资源默认值与用户偏好（threads / hash）
//   5) 进程生命周期（Spawn / ReaderThread / Shutdown）
//   6) INFO-only 解析器（ParseLine）
//   7) ask 编排（结果 JSON / worker / 结果投递）
//   8) 对外接口（Init / HandleOp / GetCaps / ...）

#include "engine_host.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <intrin.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace eng {

// ============================================================ 1) 小工具
static std::wstring Join(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) return b;
  if (a.back() == L'\\' || a.back() == L'/') return a + b;
  return a + L"\\" + b;
}
static bool FileExists(const std::wstring& p) {
  DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w(n > 0 ? (size_t)n : 0, 0);
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}
static std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? (size_t)n : 0, 0);
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}
static std::string JsonQuote(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:
        if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
  }
  o += "\"";
  return o;
}
static DWORD NowMs() { return GetTickCount(); }

#define ELOG(x) host::Log(std::string("[eng] ") + (x))

// ---- 请求 JSON 取值（页面消息体极小，不做完整 JSON 解析）----
static int QInt(const std::string& s, const char* key) {
  std::string k = std::string("\"") + key + "\":";
  size_t p = s.find(k);
  if (p == std::string::npos) return 0;
  return atoi(s.c_str() + p + k.size());
}
static int64_t QI64(const std::string& s, const char* key) {
  std::string k = std::string("\"") + key + "\":";
  size_t p = s.find(k);
  if (p == std::string::npos) return 0;
  return _strtoi64(s.c_str() + p + k.size(), nullptr, 10);
}
static std::string QStr(const std::string& s, const char* key) {
  std::string k = std::string("\"") + key + "\":\"";
  size_t p = s.find(k);
  if (p == std::string::npos) return std::string();
  p += k.size();
  std::string o;
  for (size_t i = p; i < s.size(); ++i) {
    char c = s[i];
    if (c == '"') break;
    if (c == '\\' && i + 1 < s.size()) { ++i; o += s[i]; continue; }
    o += c;
  }
  return o;
}
static bool QBool(const std::string& s, const char* key) {
  std::string k = std::string("\"") + key + "\":";
  size_t p = s.find(k);
  return p != std::string::npos && s.compare(p + k.size(), 4, "true") == 0;
}
// 解析 "[[a,b,..],[..]]" 形式的嵌套整型数组（board / pv 通用）
static std::vector<std::vector<int>> QInt2D(const std::string& s, const char* key) {
  std::vector<std::vector<int>> out;
  std::string k = std::string("\"") + key + "\"";
  size_t p = s.find(k);
  if (p == std::string::npos) return out;
  p = s.find('[', p);
  if (p == std::string::npos) return out;
  int depth = 0;
  std::vector<int> row;
  for (size_t i = p; i < s.size(); ++i) {
    char c = s[i];
    if (c == '[') { depth++; if (depth == 2) row.clear(); }
    else if (c == ']') {
      depth--;
      if (depth == 1) out.push_back(row);
      else if (depth <= 0) break;
    } else if ((c >= '0' && c <= '9') || c == '-') {
      char* end = nullptr;
      long v = strtol(s.c_str() + i, &end, 10);
      if (end && end > s.c_str() + i) { row.push_back((int)v); i = (size_t)(end - s.c_str()) - 1; }
    }
  }
  return out;
}
// "x,y x,y ..." → 坐标表（页面坐标；坐标翻转已由引擎内部按 X_flipY 完成往返）
static std::vector<std::pair<int, int>> ParseCoordList(const std::string& s) {
  std::vector<std::pair<int, int>> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= s.size() || s[i] < '0' || s[i] > '9') { break; }
    char* e1 = nullptr;
    long x = strtol(s.c_str() + i, &e1, 10);
    if (!e1 || *e1 != ',') break;
    long y = strtol(e1 + 1, &e1, 10);
    out.push_back({(int)x, (int)y});
    i = (size_t)(e1 - s.c_str());
  }
  return out;
}
static bool IsBareMove(const std::string& s) {
  if (s.empty() || s.size() > 12) return false;
  bool comma = false;
  for (char c : s) {
    if (c == ',') { comma = true; continue; }
    if (c == ' ') continue;
    if (c < '0' || c > '9') return false;
  }
  return comma;
}

// ============================================================ 2) 全局状态
struct Engine {
  HANDLE     hProcess = nullptr;
  HANDLE     hStdin    = nullptr;
  std::mutex spawnMu;
  std::atomic<bool> alive{false};
  // ★ 2026-10-23：引擎是否已经「开口」（stdout 吐出第一行）。
  //   Rapfi 0.44 启动时要解压 4 份 NNUE 权重（~38MB）+ 分配置换表，实测 300~800ms 才进主循环；
  //   以前 spawn 一返回就允许 ask，第一条 ask 落在这个窗口里会超时返回空候选，
  //   页面于是弹「AI 引擎还没就绪（首次启动要加载模型）」。见 SpawnLocked 末尾的等待。
  std::atomic<bool> spoke{false};
  int        gen = 0;
};

static Engine       g_e;
static std::wstring g_dir;
static std::string  g_variant;
static std::string  g_badVariant;
static std::string  g_diag;
static bool         g_probed = false;
static bool         g_avail  = false;
static int          g_appliedThreads = 0;
static int          g_appliedHashKB  = 0;
static int          g_appliedRule    = -1;
static int          g_appliedPonder  = -1;
// ★ 2026-10-23（对齐 gomocalc 的调用序列）：Rapfi 的 `options.swapable` **默认 false**，
//   只有 `INFO SWAPABLE 1` 才会在根节点额外搜索「交换」这一变化（searchengine.cpp:429）。
//   gomocalc 每轮都会发 `INFO SWAPABLE (swaped ? 0 : 1)`；我们之前一条都没发 ⇒
//   在 SWAP1/SWAP2 规则（engineRule() 映射到 5）下，引擎根本没考虑过交换。
static int          g_appliedSwapable = -1;

static std::mutex g_writeMu;   // stdin 写入串行化

struct Cur {
  bool       active = false;
  bool       done = false;
  bool       superseded = false;
  bool       alive = true;
  int        id = 0;
  int64_t    key = 0;
  bool       fromLink = false;
  std::string opName = "ask";    // 回执 op 名（ask / balance）
  int        side = 0;          // 引擎走子方（由**实际送入引擎的盘面**推导，权威）
  bool       scan = false;      // 扫描防守（YXSEARCHDEFEND，全点 multipv）
  int        outstanding = 0;   // 已发出、尚未收到裸着法的 YXNBEST 次数
  bool       drainBooked = false;  // 本次作废是否已记过欠账（防止 op 路径与收官路径重复计数）
  ParseState ps;
  DWORD      t0 = 0;
  DWORD      lastTick = 0;
};
static Cur                     g_cur;
static std::mutex              g_stMu;
static std::condition_variable g_stCv;
// 已作废的搜索仍欠的裸着法条数：这些回执到达时不得写进当前 ask 的状态
static int                     g_drain = 0;

struct Pending { AskRequest req; int id = 0; bool fromLink = false; };
static std::deque<Pending>     g_queue;
static std::mutex              g_qMu;
static std::condition_variable g_qCv;
static std::thread             g_worker;
static bool                    g_workerRun = false;
static bool                    g_workerStop = false;

static int g_usrThreads = 0;   // 0 = 自动（floor(物理核/2)）
static int g_usrHashMB  = 0;   // 0 = 自动（512MB）

// ---- 前置声明 ----
static void WriteRaw(const std::string& text);
static bool EnsureAlive();
static void ReaderThread(HANDLE out, int myGen);
static void OnEngineLine(const std::string& line);
static void StartWorker();
static void DoAsk(const AskRequest& req, int id, bool fromLink);
static std::string BuildAskCmds(const AskRequest& req, bool forceFull = false);
static std::string ResJson(const Cur& c);
static void SavePrefs(int threads, int hashMB);
static std::wstring PrefFile();
static int MaxThreads();
static int EffectiveThreads();
static int EffectiveHashKB();

// ============================================================ 3) ISA 探测 / 变体
struct CpuCaps { bool sse = false, avx2 = false, avx512f = false, avx512vnni = false, avxvnni = false; };

static CpuCaps CpuIdCaps() {
  CpuCaps c;
  c.sse = true;
  int r[4] = {0, 0, 0, 0};
  __cpuidex(r, 0, 0);
  if (r[0] < 1) return c;
  __cpuidex(r, 1, 0);
  bool osxsave = (r[2] & (1 << 27)) != 0;
  bool avxSup  = (r[2] & (1 << 28)) != 0;
  unsigned long long xcr0 = osxsave ? _xgetbv(0) : 0;
  bool xmmYmm = osxsave && (xcr0 & 0x06) == 0x06;
  bool zmm    = osxsave && (xcr0 & 0xE6) == 0xE6;
  if (r[0] < 7) return c;
  __cpuidex(r, 7, 0);
  bool avx2bit = (r[1] >> 5) & 1;
  bool f512 = (r[1] >> 16) & 1, dq = (r[1] >> 17) & 1, bw = (r[1] >> 30) & 1, vl = (r[1] >> 31) & 1;
  bool v512 = (r[2] >> 11) & 1;
  c.avx2       = avxSup && xmmYmm && avx2bit;
  c.avx512f    = c.avx2 && zmm && f512 && dq && bw && vl;
  c.avx512vnni = c.avx512f && v512;
  __cpuidex(r, 7, 1);
  c.avxvnni    = c.avx2 && (((unsigned)r[0] >> 4) & 1) != 0;
  return c;
}

static std::string CpuFingerprint() {
  int r[4] = {0, 0, 0, 0};
  __cpuidex(r, 0, 0);
  unsigned vendor[3] = {(unsigned)r[1], (unsigned)r[3], (unsigned)r[2]};
  unsigned maxLeaf = (unsigned)r[0];
  unsigned sig = 0, feat7 = 0, feat7c = 0, feat7_1 = 0;
  if (maxLeaf >= 1) { __cpuidex(r, 1, 0); sig = (unsigned)r[0]; }
  if (maxLeaf >= 7) {
    __cpuidex(r, 7, 0);
    feat7 = (unsigned)r[1]; feat7c = (unsigned)r[2];
    __cpuidex(r, 7, 1); feat7_1 = (unsigned)r[0];
  }
  char b[192];
  snprintf(b, sizeof(b), "%08X%08X%08X-%08X-%08X%08X-%08X-%d",
           vendor[0], vendor[1], vendor[2], sig, feat7, feat7c, feat7_1, host::LogicCores());
  return std::string(b);
}

static const char* kVariantOrder[] = {"avx512vnni", "avxvnni", "avx512", "avx2", "sse"};

static bool VariantCpuOk(const std::string& v, const CpuCaps& c) {
  if (v == "avx512vnni") return c.avx512vnni;
  if (v == "avxvnni")    return c.avxvnni;
  if (v == "avx512")     return c.avx512f;
  if (v == "avx2")       return c.avx2;
  return true;
}

// 首次运行的降档验证：真跑一次搜索，既有裸着法、进程又正常退出才算可用。
static bool SmokeVariantOk(const std::wstring& dir, const std::string& name) {
  std::wstring exe = Join(dir, std::wstring(L"RapfiEngine-") + Utf8ToWide(name) + L".exe");
  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
  if (!CreatePipe(&inR, &inW, &sa, 0)) return false;
  if (!CreatePipe(&outR, &outW, &sa, 0)) { CloseHandle(inR); CloseHandle(inW); return false; }
  SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = inR; si.hStdOutput = outW; si.hStdError = outW;
  PROCESS_INFORMATION pi = {};
  std::wstring cmd = L"\"" + exe + L"\"";
  std::vector<wchar_t> cbuf(cmd.begin(), cmd.end());
  cbuf.push_back(0);
  if (!CreateProcessW(nullptr, cbuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                      nullptr, dir.c_str(), &si, &pi)) {
    CloseHandle(inR); CloseHandle(inW); CloseHandle(outR); CloseHandle(outW);
    return false;
  }
  CloseHandle(pi.hThread);
  HANDLE job = host::Job();
  if (job) AssignProcessToJobObject(job, pi.hProcess);
  host::ChildTracked(pi.hProcess);
  CloseHandle(inR);
  CloseHandle(outW);
  const char* script =
      "START 15\nINFO RULE 0\nINFO TIMEOUT_TURN 300\nINFO TIMEOUT_MATCH 5000\n"
      "INFO TIME_LEFT 5000\nYXBOARD 7,7,1 7,8,2 DONE\nYXNBEST 1\n";
  DWORD written = 0;
  WriteFile(inW, script, (DWORD)strlen(script), &written, nullptr);
  CloseHandle(inW);

  bool sawMove = false;
  std::string pending;
  char rb[4096];
  const DWORD t0 = NowMs();
  while (NowMs() - t0 < 5000) {
    DWORD avail = 0;
    if (PeekNamedPipe(outR, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
      DWORD n = 0;
      DWORD want = avail > (DWORD)sizeof(rb) ? (DWORD)sizeof(rb) : avail;
      if (ReadFile(outR, rb, want, &n, nullptr) && n > 0) {
        pending.append(rb, n);
        size_t pos = 0;
        while ((pos = pending.find('\n')) != std::string::npos) {
          std::string line = pending.substr(0, pos);
          pending.erase(0, pos + 1);
          if (!line.empty() && line.back() == '\r') line.pop_back();
          if (IsBareMove(line)) sawMove = true;
        }
      }
    }
    DWORD code = 0;
    if (GetExitCodeProcess(pi.hProcess, &code) && code != STILL_ACTIVE) {
      TerminateProcess(pi.hProcess, 0);
      CloseHandle(pi.hProcess);
      CloseHandle(outR);
      return sawMove && code == 0;
    }
    if (sawMove) break;
    Sleep(40);
  }
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  if (code == STILL_ACTIVE) TerminateProcess(pi.hProcess, 0);
  CloseHandle(pi.hProcess);
  CloseHandle(outR);
  return sawMove && (code == 0 || code == STILL_ACTIVE);
}

static std::string ReadEngineSourceTag(const std::string& variant) {
  FILE* f = _wfopen(Join(g_dir, L"engines.json").c_str(), L"rb");
  if (!f) return std::string();
  std::string all;
  char b[4096];
  size_t n = 0;
  while ((n = fread(b, 1, sizeof(b), f)) > 0) all.append(b, n);
  fclose(f);
  size_t p = all.find("\"" + variant + "\"");
  if (p == std::string::npos) return std::string();
  size_t s = all.find("\"source\"", p);
  if (s == std::string::npos) return std::string();
  return QStr(all.substr(s), "source");
}

static bool Probe() {
  if (g_probed) return g_avail;
  g_probed = true;
  std::wstring dir = Join(host::EngineDir(), L"rapfi-native");
  if (!FileExists(Join(dir, L"config.toml"))) {
    g_diag = "rapfi-native/config.toml 不存在";
    ELOG("探测失败：" + g_diag);
    return false;
  }
  g_dir = dir;
  CpuCaps caps = CpuIdCaps();
  ELOG("cpuid: avx2=" + std::string(caps.avx2 ? "1" : "0") +
       " avx512f=" + std::string(caps.avx512f ? "1" : "0") +
       " avx512vnni=" + std::string(caps.avx512vnni ? "1" : "0") +
       " avxvnni=" + std::string(caps.avxvnni ? "1" : "0"));

  {  // 上次成功的变体缓存（带 CPU 指纹；换机器/拷目录自动作废）
    FILE* f = _wfopen(Join(dir, L".variant-ok").c_str(), L"rb");
    if (f) {
      char buf[256] = {0};
      size_t n = fread(buf, 1, sizeof(buf) - 1, f);
      fclose(f);
      std::string cached(buf, n);
      while (!cached.empty() && (cached.back() == '\n' || cached.back() == '\r')) cached.pop_back();
      std::string v = cached, fp;
      size_t sp = cached.find(' ');
      if (sp != std::string::npos) { v = cached.substr(0, sp); fp = cached.substr(sp + 1); }
      if (!fp.empty() && fp == CpuFingerprint() && VariantCpuOk(v, caps) &&
          FileExists(Join(dir, std::wstring(L"RapfiEngine-") + Utf8ToWide(v) + L".exe"))) {
        g_variant = v;
        g_avail = true;
        ELOG("可用（沿用上次的变体）：variant=" + g_variant);
        return true;
      }
      if (!v.empty()) ELOG(".variant-ok 与本机 CPU 不匹配 → 作废，重新探测");
    }
  }

  std::string firstOk;
  for (const char* v : kVariantOrder) {
    if (!FileExists(Join(dir, std::wstring(L"RapfiEngine-") + Utf8ToWide(v) + L".exe"))) continue;
    if (v == g_badVariant) continue;
    if (!VariantCpuOk(v, caps)) continue;
    if (firstOk.empty()) firstOk = v;
    if (SmokeVariantOk(dir, v)) { g_variant = v; break; }
    ELOG(std::string("variant=") + v + " 冒烟失败（CPU 不支持该指令集？）→ 降一档");
  }
  if (g_variant.empty()) {
    g_variant = firstOk;
    if (g_variant.empty() && FileExists(Join(dir, L"RapfiEngine-sse.exe"))) g_variant = "sse";
  }
  if (g_variant.empty()) {
    g_diag = "rapfi-native/ 里没有可在本机运行的引擎 exe";
    ELOG(g_diag);
    return false;
  }
  g_avail = true;
  {
    FILE* f = _wfopen(Join(dir, L".variant-ok").c_str(), L"wb");
    if (f) {
      std::string rec = g_variant + " " + CpuFingerprint();
      fwrite(rec.data(), 1, rec.size(), f);
      fclose(f);
    }
  }
  ELOG("可用：variant=" + g_variant + " dir=" + WideToUtf8(dir));
  return true;
}

// ============================================================ 4) 资源 / 偏好
static int MaxThreads() {
  int phys = host::PhysCores();
  int mx = phys > 2 ? phys - 1 : (phys > 1 ? 2 : 1);
  int hostMax = host::ThreadsMaxHost();
  if (hostMax > mx) mx = hostMax;   // 保留实测最优档（逻辑核 - 视觉预留）
  return mx < 1 ? 1 : mx;
}
static int DefaultThreads() {
  // ★ 实测最优档＝逻辑核 - 视觉预留，与宿主 ThreadsDefault() 同口径。
  //   勿改回「物理核/2」——那会让引擎 nps 直接腰斩（2026-10-16 已实测并回退）。
  int t = host::ThreadsMaxHost();
  return t < 1 ? 1 : t;
}
static int DefaultHashKB() { return 512 * 1024; }
static int ClampHashKB(int kb) {
  long long ramKB = host::TotalPhysKB();
  long long hi = std::min<long long>(4096LL * 1024, std::max<long long>(262144, ramKB / 4));
  if (kb < 262144) kb = 262144;
  if (kb > (int)hi) kb = (int)hi;
  return kb;
}
static int EffectiveThreads() {
  int t = g_usrThreads > 0 ? g_usrThreads : DefaultThreads();
  return std::max(1, std::min(t, MaxThreads()));
}
static int EffectiveHashKB() {
  return ClampHashKB(g_usrHashMB > 0 ? g_usrHashMB * 1024 : DefaultHashKB());
}

static std::wstring PrefFile() {
  wchar_t tmp[MAX_PATH] = {0};
  GetTempPathW(MAX_PATH, tmp);
  return Join(std::wstring(tmp), L"GomokuTrainer.engine");
}
static void SavePrefs(int threads, int hashMB) {
  if (threads <= 0 && hashMB <= 0) return;
  std::string line = std::to_string(threads > 0 ? threads : g_usrThreads) + "|" +
                     std::to_string(hashMB > 0 ? hashMB : g_usrHashMB);
  FILE* f = _wfopen(PrefFile().c_str(), L"wb");
  if (!f) return;
  fwrite(line.data(), 1, line.size(), f);
  fflush(f);   // 立即落盘：硬杀也不丢
  fclose(f);
}

void SetUserPrefs(int threads, int hashMB) {
  g_usrThreads = threads > 0 ? threads : 0;
  g_usrHashMB  = hashMB > 0 ? hashMB : 0;
  SavePrefs(threads, hashMB);
  int t  = EffectiveThreads();
  int kb = EffectiveHashKB();
  if (t == g_appliedThreads && kb == g_appliedHashKB) return;
  std::string cmd;
  if (t != g_appliedThreads)  { g_appliedThreads = t;  cmd += "INFO THREAD_NUM " + std::to_string(t) + "\n"; }
  if (kb != g_appliedHashKB)  { g_appliedHashKB = kb;  cmd += "INFO HASH_SIZE " + std::to_string(kb) + "\n"; }
  if (g_e.alive.load()) WriteRaw(cmd);
  ELOG("配置生效：threads=" + std::to_string(t) + " hashKB=" + std::to_string(kb));
}

void LoadPrefs() {
  FILE* f = _wfopen(PrefFile().c_str(), L"rb");
  if (f) {
    char buf[64] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    int t = 0, h = 0;
    sscanf(buf, "%d|%d", &t, &h);
    int t2 = t, h2 = h;
    if (t > 0) {
      t = std::max(1, std::min(t, MaxThreads()));
      if (t != t2) ELOG("LINK_ENG_CLAMP threads " + std::to_string(t2) + " -> " + std::to_string(t));
    }
    if (h > 0) {
      int hc = (int)(ClampHashKB(h * 1024) / 1024);
      if (hc != h2) ELOG("LINK_ENG_CLAMP hashMB " + std::to_string(h2) + " -> " + std::to_string(hc));
      h = hc;
    }
    if (t > 0) g_usrThreads = t;
    if (h > 0) g_usrHashMB  = h;
    if (t != t2 || h != h2) SavePrefs(t, h);
    ELOG("读回用户偏好 threads=" + std::to_string(t) + " hashMB=" + std::to_string(h));
  }
  g_appliedThreads = EffectiveThreads();
  g_appliedHashKB  = EffectiveHashKB();
}

// ============================================================ 5) 进程生命周期
static void WriteRaw(const std::string& text) {
  std::lock_guard<std::mutex> lk(g_writeMu);
  if (!g_e.hStdin) return;
  std::string s = text;
  if (s.empty() || s.back() != '\n') s += "\n";
  DWORD wr = 0;
  if (!WriteFile(g_e.hStdin, s.data(), (DWORD)s.size(), &wr, nullptr))
    ELOG("write FAIL err=" + std::to_string(GetLastError()));
}

static void SpawnLocked() {
  if (g_e.hProcess) { TerminateProcess(g_e.hProcess, 0); CloseHandle(g_e.hProcess); g_e.hProcess = nullptr; }
  { std::lock_guard<std::mutex> lk(g_writeMu); if (g_e.hStdin) { CloseHandle(g_e.hStdin); g_e.hStdin = nullptr; } }
  g_e.alive = false;

  std::wstring exe = Join(g_dir, std::wstring(L"RapfiEngine-") + Utf8ToWide(g_variant) + L".exe");
  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
  if (!CreatePipe(&inR, &inW, &sa, 0)) return;
  if (!CreatePipe(&outR, &outW, &sa, 0)) { CloseHandle(inR); CloseHandle(inW); return; }
  SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = inR;
  si.hStdOutput = outW;
  si.hStdError = outW;
  PROCESS_INFORMATION pi = {};
  std::wstring cmd = L"\"" + exe + L"\"";
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(0);
  // CWD 必须是引擎目录：NO_COMMAND_MODULES 剥掉 CLI 后，配置只从 CWD/exe 同级的 config.toml 读。
  BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                           nullptr, g_dir.c_str(), &si, &pi);
  CloseHandle(inR);
  CloseHandle(outW);
  if (!ok) {
    CloseHandle(inW); CloseHandle(outR);
    g_diag = "启动引擎进程失败 err=" + std::to_string(GetLastError());
    ELOG(g_diag);
    return;
  }
  CloseHandle(pi.hThread);
  HANDLE job = host::Job();
  if (job) AssignProcessToJobObject(job, pi.hProcess);
  host::ApplyAffinity(pi.hProcess);
  SetPriorityClass(pi.hProcess, ABOVE_NORMAL_PRIORITY_CLASS);   // 引擎优先于界面
  host::PinHostAway();
  host::ChildTracked(pi.hProcess);

  g_e.hProcess = pi.hProcess;
  g_e.hStdin   = inW;
  int myGen = ++g_e.gen;
  g_e.spoke = false;
  g_e.alive = true;
  std::thread(ReaderThread, outR, myGen).detach();

  g_appliedRule    = -1;      // 新进程需要重新下发规则/思考模式
  g_appliedPonder  = -1;
  g_appliedSwapable = -1;
  g_appliedThreads = EffectiveThreads();
  g_appliedHashKB  = EffectiveHashKB();
  std::string boot = "START 15\nINFO RULE 0\nINFO SHOW_DETAIL 3\n";
  boot += "INFO THREAD_NUM " + std::to_string(g_appliedThreads) + "\n";
  boot += "INFO HASH_SIZE "  + std::to_string(g_appliedHashKB)  + "\n";
  WriteRaw(boot);
  ELOG("已启动 " + WideToUtf8(exe) + " (gen=" + std::to_string(myGen) +
       ", threads=" + std::to_string(g_appliedThreads) +
       ", hashKB=" + std::to_string(g_appliedHashKB) + ")");

  // ★ 2026-10-23（用户：「软件没有适配这个引擎，导致算力质量很低」）：
  //   等引擎**真正开口**（stdout 第一行）再放行 ask。0.44 启动要解压 4 份 NNUE（38MB）
  //   并分配置换表，实测 300~800ms；这段窗口里发下去的 ask 会在页面侧 375ms 超时、
  //   返回空候选，页面就弹「引擎还没就绪」并白跑一轮。最多等 2.5s，超时也继续（宁可慢不卡死）。
  DWORD tSpoke0 = NowMs();
  for (int i = 0; i < 250 && !g_e.spoke.load(); ++i) Sleep(10);
  if (g_e.spoke.load())
    ELOG("引擎已就绪（开口用时 " + std::to_string(NowMs() - tSpoke0) + "ms）");
  else
    ELOG("引擎启动 2.5s 内没有任何输出，继续（首次加载模型可能更慢）");
}

static bool EnsureAlive() {
  if (!Probe()) return false;
  std::lock_guard<std::mutex> lk(g_e.spawnMu);
  if (g_e.alive.load()) return true;
  SpawnLocked();
  return g_e.alive.load();
}

static void WitnessCrash(DWORD code) {
  std::string d;
  bool downgrade = false;
  if (code == 0xC000001D) { d = "引擎非法指令退出（本机 CPU 不支持该变体指令集）→ 自动降档"; downgrade = true; }
  else if (code == 0xC0000135 || code == 0xC0000139) { d = "引擎缺少运行库（请安装 VC++ Redistributable x64）"; downgrade = true; }
  else if (code == 0xC0000005) d = "引擎被安全软件拦截或文件损坏（请把 rapfi-native 加入白名单）";
  else { char b[64]; snprintf(b, sizeof(b), "引擎异常退出 code=0x%08lX", (unsigned long)code); d = b; }
  g_diag = d;
  ELOG(d);
  if (downgrade && !g_variant.empty()) {
    g_badVariant = g_variant;
    g_variant.clear();
    g_probed = false;
    g_avail  = false;
    DeleteFileW(Join(g_dir, L".variant-ok").c_str());
    host::UnpinHost();
    ELOG("变体已降档，下次启动引擎时将重挑可用 ISA");
  }
}

static void ReaderThread(HANDLE out, int myGen) {
  std::string pending;
  char buf[8192];
  for (;;) {
    DWORD n = 0;
    if (!ReadFile(out, buf, sizeof(buf), &n, nullptr) || n == 0) break;
    pending.append(buf, n);
    size_t pos;
    while ((pos = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, pos);
      pending.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) {
        g_e.spoke = true;          // 引擎第一行输出 = 主循环已就位（SpawnLocked 在等它）
        OnEngineLine(line);
      }
    }
  }
  CloseHandle(out);
  if (g_e.gen != myGen) return;

  DWORD code = 0;
  if (g_e.hProcess) GetExitCodeProcess(g_e.hProcess, &code);
  {
    std::lock_guard<std::mutex> lk(g_stMu);
    g_e.alive = false;
    g_cur.alive = false;
    g_cur.done = true;
    g_cur.ps.finished = false;
    g_drain = 0;
  }
  g_stCv.notify_all();
  char cb[32];
  snprintf(cb, sizeof(cb), "0x%lX", (unsigned long)code);
  ELOG(std::string("引擎进程退出 code=") + cb);
  if (code != 0) WitnessCrash(code);
}

// ============================================================ 6) 解析器（只信 INFO）
void ParseLine(const std::string& line, ParseState& st) {
  if (line.empty()) return;
  if (line.compare(0, 5, "INFO ") == 0) {
    const char* p = line.c_str() + 5;
    size_t i = 0;
    while (p[i] && p[i] != ' ') i++;
    std::string tok(p, i);
    std::string val = (p[i] == ' ') ? std::string(p + i + 1) : std::string();
    if (tok == "PV") {
      if (val == "DONE") {
        if (st.inPv && st.pvIdx >= 0) {
          if ((size_t)st.pvIdx >= st.cands.size()) st.cands.resize((size_t)st.pvIdx + 1);
          st.cands[(size_t)st.pvIdx] = st.cur;
        }
        st.inPv = false;
      } else {
        st.pvIdx = atoi(val.c_str());
        st.cur = Candidate();
        st.inPv = true;
      }
      return;
    }
    if (!st.inPv) return;
    if (tok == "NUMPV") {
      // ★ 2026-10-23（用户：「扫描防守……有的点都重叠了」）：
      //   live 搜索每轮都从 PV 0 重发全点 INFO；若本轮 PV 数比上一轮少，旧尾部会
      //   残留成「同一个坐标两条候选」（面板出现 2 j9 / 3 j9 就是它）。NUMPV 在每轮
      //   第一条 PV 到达时就给出本轮总数，先裁掉旧尾巴。
      int n = atoi(val.c_str());
      st.numPv = n;
      if (n >= 0 && (size_t)n < st.cands.size()) st.cands.resize((size_t)n);
    }
    else if (tok == "DEPTH")      { st.cur.depth = atoi(val.c_str()); if (st.cur.depth > st.maxDepth) st.maxDepth = st.cur.depth; }
    else if (tok == "SELDEPTH")   { st.cur.selDepth = atoi(val.c_str()); if (st.cur.selDepth > st.selDepth) st.selDepth = st.cur.selDepth; }
    else if (tok == "NODES")      st.cur.nodes = _strtoi64(val.c_str(), nullptr, 10);
    else if (tok == "TOTALNODES") st.totalNodes = _strtoi64(val.c_str(), nullptr, 10);
    else if (tok == "TOTALTIME")  st.timeMs = atoi(val.c_str());
    else if (tok == "SPEED")      st.speed = _strtoi64(val.c_str(), nullptr, 10);
    else if (tok == "EVAL")       { st.cur.eval = val; st.lastEvalSide = st.pvIdx; }
    else if (tok == "WINRATE") {
      double d = atof(val.c_str());
      int bp = (int)(d * 10000.0 + 0.5);
      st.cur.winrateBp = bp < 0 ? 0 : (bp > 10000 ? 10000 : bp);
    } else if (tok == "BESTLINE") {
      // 候选的坐标 = BESTLINE 首着（引擎不给单独的 x/y 字段）。
      // 实测（RapfiEngine-avx512vnni，SHOW_DETAIL 3）每轮只输出
      // `INFO PV <idx>` … `INFO BESTLINE x,y …` … `INFO PV DONE`，
      // 不从这里取坐标的话 candidate.x/y 恒为 -1 ⇒ 结构化结果永远是空候选。
      st.cur.pv = ParseCoordList(val);
      if (!st.cur.pv.empty()) { st.cur.x = st.cur.pv[0].first; st.cur.y = st.cur.pv[0].second; }
    }
    return;
  }
  // 人类文本（含 [Pondering] 前缀行）整行丢弃：其中的 Eval 是对手视角，绝不可采信。
  if (line.compare(0, 8, "MESSAGE ") == 0) return;
  if (line.compare(0, 6, "ERROR ") == 0)   { ELOG("engine ERROR: " + line.substr(6)); return; }
  if (line.compare(0, 6, "FORBID") == 0)   { st.forbid = ParseCoordList(line.substr(6)); return; }
  if (line == "SWAP") { st.swap = true; st.finished = true; return; }
  if (IsBareMove(line)) {
    auto c = ParseCoordList(line);
    // BALANCE_TWO 的终局回执是两个坐标（`x1,y1 x2,y2`），BALANCE_ONE / YXNBEST 是一个。
    // 全部收进 movePts，bestX/bestY 取首着（普通 ask 只需首着）。
    if (!c.empty()) { st.movePts = c; st.bestX = c[0].first; st.bestY = c[0].second; st.finished = true; }
  }
}

// ============================================================ 7) ask 编排
static std::string ResJson(const Cur& c) {
  const ParseState& ps = c.ps;
  int bx = -1, by = -1;
  std::string be;
  if (ps.bestX >= 0) { bx = ps.bestX; by = ps.bestY; }
  else if (!ps.cands.empty() && ps.cands[0].x >= 0) { bx = ps.cands[0].x; by = ps.cands[0].y; }
  if (!ps.cands.empty()) be = ps.cands[0].eval;

  char h[64];
  std::string r = "{\"side\":" + std::to_string(c.side);
  r += ",\"key\":" + std::to_string(c.key);
  r += ",\"op\":" + JsonQuote(c.opName);
  snprintf(h, sizeof(h), ",\"bestX\":%d,\"bestY\":%d", bx, by);
  r += h;
  r += ",\"bestEval\":" + JsonQuote(be);
  snprintf(h, sizeof(h), ",\"depth\":%d,\"selDepth\":%d,\"timeMs\":%d", ps.maxDepth, ps.selDepth, ps.timeMs);
  r += h;
  r += ",\"nodes\":" + std::to_string(ps.totalNodes);
  r += ",\"speed\":" + std::to_string(ps.speed);
  r += ",\"superseded\":" + std::string(c.superseded ? "true" : "false");
  r += ",\"finished\":" + std::string(ps.finished ? "true" : "false");
  r += ",\"swap\":" + std::string(ps.swap ? "true" : "false");
  r += ",\"candidates\":[";
  bool first = true;
  for (const Candidate& cd : ps.cands) {
    if (cd.x < 0) continue;
    if (!first) r += ",";
    first = false;
    snprintf(h, sizeof(h), "{\"x\":%d,\"y\":%d,\"depth\":%d,\"selDepth\":%d,\"nodes\":%lld,",
             cd.x, cd.y, cd.depth, cd.selDepth, cd.nodes);
    r += h;
    r += "\"winrateBp\":" + std::to_string(cd.winrateBp);
    r += ",\"eval\":" + JsonQuote(cd.eval);
    r += ",\"pv\":[";
    // ★ 扫描防守一次回 80+ 个候选：只留前 2 手，别让 tick JSON 被整条 PV 撑爆。
    size_t pvN = c.scan ? (cd.pv.size() > 2 ? 2 : cd.pv.size()) : cd.pv.size();
    for (size_t i = 0; i < pvN; ++i) {
      if (i) r += ",";
      r += "[" + std::to_string(cd.pv[i].first) + "," + std::to_string(cd.pv[i].second) + "]";
    }
    r += "]}";
  }
  r += "],\"pts\":[";
  for (size_t i = 0; i < ps.movePts.size(); ++i) {
    if (i) r += ",";
    r += "[" + std::to_string(ps.movePts[i].first) + "," + std::to_string(ps.movePts[i].second) + "]";
  }
  r += "],\"forbid\":[";
  for (size_t i = 0; i < ps.forbid.size(); ++i) {
    if (i) r += ",";
    r += "[" + std::to_string(ps.forbid[i].first) + "," + std::to_string(ps.forbid[i].second) + "]";
  }
  r += "]}";
  return r;
}

// 组盘：YXBOARD 期望**着法序列**（1=先手方黑、2=后手方白），引擎按序复盘。
// 交错 B,W,B,W… 天然满足「#黑 == #白（黑走）或 #黑 == #白+1（白走）」的合法局面，
// 复盘结束时引擎的走子方恰等于由盘面奇偶推导的 side。
// ★★ 2026-10-23（用户：「当前引擎调度上并不合理，远低于官方拉菲整合包的智力……
//   继续优化调用算法，提升并行计算能力，当前速度和深度都很浅很慢」）
//   —— 盘面**只比上一轮多一手**时，改用 Gomocup 的 `TURN x,y` 而不是整盘 `YXBOARD`。
//   差别（源码级）：YXBOARD → getPosition() → `board->newGame()` 把 board 历史清空，
//   引擎只剩置换表；TURN → board->move() + 直接 think()，board 历史 / lastMove /
//   置换表全部延续。实测同预算（14 线程 / 512MB TT / 均势开局）深度 +1（18→19）。
//   TURN 固定 multiPV=1 且会立刻 think()，所以只在 topN==1、非 scan、非 balance 时用；
//   扫描防守补过一手 PASS、平衡点改过 board 状态 ⇒ 一律打上 noIncr，下次退回整盘重发。
static std::vector<std::pair<int, int>> g_seqBl, g_seqWh;
static int  g_seqSide = 0;         // 上一轮**下发时**的行棋方（1 黑 / 2 白）
static bool g_seqValid = false;    // 上面三份记录是否可用
static bool g_seqNoIncr = false;   // 上一轮改过 board（scan 的 PASS / balance）⇒ 禁止增量

static bool SamePts(const std::vector<std::pair<int, int>>& a,
                    const std::vector<std::pair<int, int>>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i] != b[i]) return false;
  return true;
}

// 本轮是否用了增量 TURN（DoAsk 的看门狗据此决定要不要降级整盘重发）
static bool g_askIncr = false;

static std::string BuildAskCmds(const AskRequest& req, bool forceFull) {
  std::vector<std::pair<int, int>> bl, wh;
  for (size_t y = 0; y < req.board.size(); ++y)
    for (size_t x = 0; x < req.board[y].size(); ++x) {
      int v = req.board[y][x];
      if (v == 1) bl.push_back({(int)x, (int)y});
      else if (v == 2) wh.push_back({(int)x, (int)y});
    }
  std::string board = "YXBOARD";
  size_t n = std::max(bl.size(), wh.size());
  for (size_t i = 0; i < n; ++i) {
    if (i < bl.size()) board += " " + std::to_string(bl[i].first) + "," + std::to_string(bl[i].second) + ",1";
    if (i < wh.size()) board += " " + std::to_string(wh[i].first) + "," + std::to_string(wh[i].second) + ",2";
  }
  // ★ 2026-10-22 扫描防守（用户：完全按 Rapfi 整合包的算法）：
  //   先补一手 PASS 把行棋方让给**对手**，再走引擎原生 YXSEARCHDEFEND
  //   （command/gomocup.cpp: searchDefend ⇒ multiPV = 全盘合法点）。
  //   这样每个候选的 WINRATE 就是「对手下在这里」的胜率 = 该点的危险度；
  //   整合包/弈心界面里的扫描防守也是这条通道（不是本地启发式）。
  //
  //   ★ PASS 的坐标必须是 **-1,-1**（core/iohelper.cpp 的 PASS_COORD_X/Y），
  //     不是字面量 "PASS"：YXBOARD 走 parseLegalCoord(x,y)，写 "PASS,1" 会报
  //     `ERROR Coord is not valid or empty.`，PASS 被吞掉、扫描仍在**己方**视角
  //     （实测两种写法的 WINRATE 完全相同 = PASS 没生效）。
  //   颜色写「当前行棋方」：1=黑（SELF，首着为黑）、2=白（OPPO）。
  int toMove = (bl.size() > wh.size()) ? 2 : 1;
  if (req.mode == 3 && (bl.size() || wh.size()))
    board += " -1,-1," + std::to_string(toMove);
  board += " DONE\n";

  int budget = req.budgetMs;
  if (req.live) {
    // 常驻分析：给一个「实际上不会到点」的回合时限，引擎就**只加深、不收官**，
    // 中途不会被 YXNBEST 的终局着法打断；停止只由页面的 stop / 落子抢占触发。
    budget = 3600000;
  } else {
    if (budget < 200)   budget = 200;
    if (budget > 60000) budget = 60000;
  }
  int topN = req.topN < 1 ? 1 : (req.topN > 8 ? 8 : req.topN);

  // ---- 增量判定（见上方 g_seq* 注释）：新盘面 = 上一轮盘面 + 恰好一手 ----
  bool incr = false;
  int  ix = -1, iy = -1;
  if (!forceFull && g_seqValid && !g_seqNoIncr && req.mode == 0 && topN == 1 && g_seqSide != 0) {
    int newColor = 0;
    if (SamePts(bl, g_seqBl) && wh.size() == g_seqWh.size() + 1 &&
        std::equal(g_seqWh.begin(), g_seqWh.end(), wh.begin()))
      newColor = 2;                                   // 只多了一个白子
    else if (SamePts(wh, g_seqWh) && bl.size() == g_seqBl.size() + 1 &&
             std::equal(g_seqBl.begin(), g_seqBl.end(), bl.begin()))
      newColor = 1;                                   // 只多了一个黑子
    // 新子必须是「上一轮行棋方的对手」，否则说明盘面被悔棋 / 覆盖，退回整盘重发
    if (newColor && newColor == 3 - g_seqSide) {
      const std::vector<std::pair<int, int>>& src = (newColor == 2) ? wh : bl;
      ix = src.back().first; iy = src.back().second;
      incr = true;
    }
  }

  std::string c;
  if (req.rule != g_appliedRule) { g_appliedRule = req.rule; c += "INFO RULE " + std::to_string(req.rule) + "\n"; }
  int pon = req.live ? 0 : (req.pondering ? 1 : 0);
  if (pon != g_appliedPonder) { g_appliedPonder = pon; c += "INFO PONDERING " + std::to_string(pon) + "\n"; }
  // SWAP1/SWAP2（我们映射到 rule 5）才需要引擎搜「交换」分支；其余规则发 0 让引擎别白搜。
  int swapable = (req.rule == 5) ? 1 : 0;
  if (swapable != g_appliedSwapable) {
    g_appliedSwapable = swapable;
    c += "INFO SWAPABLE " + std::to_string(swapable) + "\n";
  }
  c += "INFO TIMEOUT_TURN " + std::to_string(budget) + "\n";
  c += "INFO TIMEOUT_MATCH 6000000\nINFO TIME_LEFT 6000000\n";
  if (incr) {
    // TURN 自己会 think()（等价 YXNBEST 1），**不能再跟 YXNBEST**（thinking 期间被丢弃）。
    char tb[40];
    snprintf(tb, sizeof(tb), "TURN %d,%d\n", ix, iy);
    c += tb;
  } else {
    c += board;
    if (req.mode == 3) {
      // 扫描防守：引擎原生全点 multipv（不看 topN）。
      c += "YXSEARCHDEFEND\n";
    } else if (req.mode == 0) {
      c += "YXNBEST " + std::to_string(topN) + "\n";
    } else {
      // 官方平衡点：YXBALANCEONE / YXBALANCETWO <bias>（见 rapfi command/gomocup.cpp balance()）。
      // 引擎按当前 YXBOARD 盘面做平衡搜索，回吐标准 INFO 块 + 裸着法行（ONE=1 点，TWO=2 点）。
      // 已经用 INFO TIMEOUT_TURN 限定预算 ⇒ 到点即出结果并自行停止（一次性，无常驻）。
      c += "YXBALANCE" + std::string(req.mode == 2 ? "TWO " : "ONE ") + std::to_string(req.bias) + "\n";
    }
  }
  // 记录「引擎当前 board 应该是哪个盘面」（下次增量判定的基准）
  if (incr) ELOG("增量下发 TURN " + std::to_string(ix) + "," + std::to_string(iy) +
                 "（保留 board 历史与置换表）");
  g_askIncr = incr;
  g_seqBl = bl; g_seqWh = wh;
  g_seqSide = (bl.size() > wh.size()) ? 2 : 1;
  g_seqValid = true;
  // 扫描防守补过 PASS、平衡点会改 board ⇒ 下一轮必须整盘重发
  g_seqNoIncr = (req.mode != 0);
  return c;
}

static void PostTo(bool fromLink, const std::string& json) {
  if (fromLink) host::PostLink(json);
  else          host::PostPage(json);
}

// reader 线程：逐行解析 → 节流 tick 推送；**终局回执只由 worker 统一投递**（见 DoAsk 尾部）
static void OnEngineLine(const std::string& line) {
  bool postTick = false, fromLink = false, justDone = false;
  int id = 0;
  std::string tickJson;
  {
    std::lock_guard<std::mutex> lk(g_stMu);
    if (!g_cur.active) {
      // 无在途 ask：游离回执（上一轮 STOP/超时的尾巴）只用来清欠账，绝不写进任何状态
      if (IsBareMove(line) || line == "SWAP") {
        if (g_drain > 0) { g_drain--; if (g_drain == 0) g_stCv.notify_all(); }
      }
      else if (line.compare(0, 6, "ERROR ") == 0) ELOG("engine ERROR(游离): " + line.substr(6));
      return;
    }
    // ★ 2026-10-23：欠账未清 = 引擎还在吐上一轮被 STOP 的搜索（通常是成百条 INFO PV）。
    //   这些帧属于旧盘面，一律不得写进当前 ask（否则新盘面一开局就顶着旧候选，
    //   扫描防守还会把旧点画到新局面上）；只核销终局着法 / 顺带记 ERROR。
    //   欠账清零 = 引擎已真正停搜，唤醒等在下一条 ask 前的 DoAsk，安全下发新盘面。
    if (g_drain > 0) {
      if (IsBareMove(line) || line == "SWAP") {
        g_drain--;
        if (g_drain == 0) g_stCv.notify_all();
      }
      else if (line.compare(0, 6, "ERROR ") == 0) ELOG("engine ERROR(欠账): " + line.substr(6));
      return;
    }
    ParseLine(line, g_cur.ps);
    // !g_cur.done 幂等门：引擎可能把终局着法吐不止一次（超时 + 收尾），
    // 不做一次性判定会重复算终局；ack 的单点投递在 DoAsk 尾部（此处只置位 + 唤醒）。
    if (g_cur.ps.finished && !g_cur.done) {
      if (g_cur.outstanding > 0) g_cur.outstanding--;
      if (g_cur.outstanding == 0) { g_cur.done = true; justDone = true; }
    }
    if (!g_cur.done) {
      DWORD now = NowMs();
      if (now - g_cur.lastTick >= 120) {
        g_cur.lastTick = now;
        postTick = true;
        fromLink = g_cur.fromLink;
        id = g_cur.id;
        tickJson = ResJson(g_cur);
      }
    }
  }
  if (justDone) g_stCv.notify_all();   // worker 醒来后统一发 engineAck
  if (postTick) {
    PostTo(fromLink, "{\"type\":\"engineAsk\",\"id\":" + std::to_string(id) +
                     ",\"phase\":\"tick\",\"res\":" + tickJson + "}");
  }
}

static void StartWorker() {
  if (g_workerRun) return;
  g_workerRun = true;
  g_worker = std::thread([] {
    for (;;) {
      Pending job;
      {
        std::unique_lock<std::mutex> lk(g_qMu);
        g_qCv.wait(lk, [] { return g_workerStop || !g_queue.empty(); });
        if (g_queue.empty()) break;
        job = g_queue.front();
        g_queue.pop_front();
      }
      try { DoAsk(job.req, job.id, job.fromLink); }
      catch (...) { ELOG("DoAsk 异常"); }
    }
  });
}

// 单实例串行服务：进入前先 STOP 掉在途搜索并等它把终局着法吐完，
// 否则引擎（thinking 期间只认 STOP）会把上一轮的着法混进新一轮的解析状态。
static void DoAsk(const AskRequest& req, int id, bool fromLink) {
  if (!EnsureAlive()) {
    std::string err = g_diag.empty() ? "engine unavailable" : g_diag;
    PostTo(fromLink, "{\"type\":\"engineAck\",\"id\":" + std::to_string(id) +
                     ",\"op\":\"ask\",\"ok\":false,\"err\":" + JsonQuote(err) + "}");
    return;
  }
  {
    std::unique_lock<std::mutex> lk(g_stMu);
    if (g_cur.active && !g_cur.done) {
      g_cur.superseded = true;
      int owe = g_cur.outstanding;
      lk.unlock();
      WriteRaw("STOP\n");
      lk.lock();
      // ★ 2026-10-23（用户：「智力降低的因素是速度变得很慢了」）：STOP 收尾实测 9ms
      //   （97 点扫描守防也一样），原来 2.5s 的等待纯属白等。
      // ★ 18:30（用户转述 Rapfi 作者：「还有 10% 是纯调用层面的优化」）：三处「等 STOP 收尾」
      //   全部从 500ms 收到 200ms —— 这是每轮抢占（live↔AI 落子）都要走的最坏路径，
      //   2 秒预算下 300ms 就是 15% 的思考时间。20 倍余量仍然安全（实测 9ms）。
      g_stCv.wait_for(lk, std::chrono::milliseconds(200), [] { return g_cur.done || !g_cur.alive; });
      if (!g_cur.drainBooked) { g_drain += g_cur.outstanding; g_cur.drainBooked = true; }
      (void)owe;
      g_cur.active = false;
    }
    g_cur = Cur();
    g_cur.active = true;
    g_cur.alive = true;
    g_cur.id = id;
    g_cur.key = req.key;
    g_cur.fromLink = fromLink;
    g_cur.opName = req.opName;
    g_cur.scan = (req.mode == 3);
    g_cur.t0 = NowMs();
    g_cur.lastTick = g_cur.t0;
    int nb = 0, nw = 0;
    for (const auto& row : req.board)
      for (int v : row) { if (v == 1) nb++; else if (v == 2) nw++; }
    g_cur.side = (nb > nw) ? 2 : 1;
    if (req.side != 0 && req.side != g_cur.side)
      ELOG("盘面奇偶修正 side " + std::to_string(req.side) + " -> " + std::to_string(g_cur.side) +
           " (#B=" + std::to_string(nb) + " #W=" + std::to_string(nw) + ")");
    g_cur.outstanding = 1;
    if (g_drain > 8) g_drain = 0;   // 兜底：异常累积时清零，避免永久错位
  }

  // ★ 2026-10-23（用户：「引擎的调用又出问题了，智力层大有下降」）：
  //   上一轮被 STOP 的搜索可能还没真正收尾（终局着法未到）；此时引擎仍 thinking，
  //   而 `runProtocol` 对 thinking 期间的命令（YXBOARD / YXNBEST …）一律丢弃 ——
  //   新盘面被整条吞掉，AI 自然拿不到候选。这里先等欠账清零（= STOP 的终局着法
  //   已到达、search 线程已退出），再下发新盘面；等不到就清欠账，宁可丢一条尾巴。
  bool drainPending = false;
  {
    std::lock_guard<std::mutex> lk(g_stMu);
    drainPending = (g_drain > 0 && g_cur.alive);
  }
  if (drainPending) {
    WriteRaw("STOP\n");   // 幂等：确保引擎在处理我们的盘面之前先处理 STOP
    std::unique_lock<std::mutex> lk(g_stMu);
    g_stCv.wait_for(lk, std::chrono::milliseconds(200), [] {
      return g_drain == 0 || !g_cur.alive;
    });
    if (g_drain > 0) {
      ELOG("drain 未清零，强制清零（丢一条迟到回执）");
      g_drain = 0;
    }
  }

  WriteRaw(BuildAskCmds(req));

  {
    std::unique_lock<std::mutex> lk(g_stMu);
    // ★ 2026-10-22 常驻分析：**不设截止时间**，一直等到 stop / 落子抢占 / 引擎退出。
    //   引擎侧 TIMEOUT_TURN 已经放到 1 小时，不会自行收官 ⇒ 搜索深度持续累积，
    //   不再出现「每轮到点重开、优势点被反复推倒重算」。
    if (req.live) {
      bool repaired = false;
      while (!g_cur.done && !g_cur.superseded && g_cur.alive) {
        // 增量看门狗（详下）：live 没有超时，TURN 若被引擎拒收就一行 INFO 都不会来。
        if (!repaired && g_askIncr && g_cur.ps.cands.empty() && g_cur.ps.totalNodes == 0 &&
            NowMs() - g_cur.t0 > 1200) {
          repaired = true;
          lk.unlock();
          ELOG("TURN 无响应（live）⇒ 降级整盘重发");
          WriteRaw(BuildAskCmds(req, true));
          lk.lock();
          continue;
        }
        g_stCv.wait_for(lk, std::chrono::milliseconds(200));
      }
    } else {
      int budget = req.budgetMs < 200 ? 200 : (req.budgetMs > 60000 ? 60000 : req.budgetMs);
      auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget + 3000);
      bool repaired = false;
      while (!g_cur.done && !g_cur.superseded && g_cur.alive) {
        // ★ 2026-10-23 增量通道看门狗：TURN 若被引擎拒收（非法点 / 上一轮状态未清），
        //   turn() 会直接 return 而不 think() ⇒ 一行 INFO 都不会有。1.2s 还听不到任何
        //   候选/节点，就当作没送到，整盘重发一次（等价旧行为，只多等 1.2s）。
        if (!repaired && g_askIncr && g_cur.ps.cands.empty() && g_cur.ps.totalNodes == 0 &&
            NowMs() - g_cur.t0 > 1200) {
          repaired = true;
          lk.unlock();
          ELOG("TURN 无响应 ⇒ 降级整盘重发");
          WriteRaw(BuildAskCmds(req, true));
          lk.lock();
          continue;
        }
        if (g_stCv.wait_for(lk, std::chrono::milliseconds(100)) == std::cv_status::timeout) {
          if (std::chrono::steady_clock::now() >= deadline) break;
        }
      }
    }
    // ★ 2026-10-23（用户：「引擎的调用又出问题了，智力层大有下降」）：
    //   到点后不要「先作废、再扔一条 STOP 就走」—— STOP 触发的终局着法就是引擎的
    //   **当前最优手**，丢掉它等于整轮预算白烧；更糟的是下一条 ask 会在引擎仍
    //   thinking 时下发盘面，被 runProtocol 整条吞掉（连锁超时 / AI 不动）。
    //   这里先发 STOP 并等终局着法回来：拿到就按正常结果（finished）返回。
    if (!g_cur.done && !g_cur.superseded && g_cur.alive) {
      lk.unlock();
      WriteRaw("STOP\n");
      lk.lock();
      g_stCv.wait_for(lk, std::chrono::milliseconds(200), [] {
        return g_cur.done || !g_cur.alive;
      });
    }
    g_cur.active = false;
    if (!g_cur.done) {
      if (!g_cur.drainBooked) { g_drain += g_cur.outstanding; g_cur.drainBooked = true; }
      std::string why = !g_cur.alive ? std::string("engine down")
                                     : (g_cur.superseded ? std::string() : std::string("search timeout"));
      if (!why.empty()) g_cur.ps.finished = false;
      if (!why.empty()) ELOG("ask 未收终局回执：" + why);
    }
  }

  // ★ 2026-10-23 增量下发的安全阀：这一轮没能正常收到终局回执（超时 / 被抢占 /
  //   引擎当时在 thinking 把 YXBOARD 吞掉了），就无法确认引擎内部的 board 已经切到
  //   我们这个盘面 ⇒ 下次一律整盘重发，避免 TURN 把子加到错的盘面上。
  if (!g_cur.done) g_seqValid = false;

  std::string res = ResJson(g_cur);
  std::string ack = "{\"type\":\"engineAck\",\"id\":" + std::to_string(id) + ",\"op\":" + JsonQuote(g_cur.opName) +
                    ",\"ok\":" + std::string(g_cur.done ? "true" : "false");
  if (!g_cur.done) ack += ",\"err\":" + JsonQuote(g_cur.superseded ? "superseded" :
                                                  (!g_cur.alive ? "engine down" : "search timeout"));
  ack += ",\"res\":" + res + "}";
  PostTo(fromLink, ack);
  ELOG("ack id=" + std::to_string(id) + " ok=" + std::string(g_cur.done ? "1" : "0") +
       " side=" + std::to_string(g_cur.side) +
       " best=" + std::to_string(g_cur.ps.bestX) + "," + std::to_string(g_cur.ps.bestY) +
       " eval=" + (g_cur.ps.cands.empty() || g_cur.ps.cands[0].eval.empty()
                     ? std::string("-") : g_cur.ps.cands[0].eval) +
       " depth=" + std::to_string(g_cur.ps.maxDepth) + "/" + std::to_string(g_cur.ps.selDepth) +
       " " + std::to_string(NowMs() - g_cur.t0) + "ms nodes=" + std::to_string(g_cur.ps.totalNodes) +
       " speed=" + std::to_string(g_cur.ps.speed) +
       " ttime=" + std::to_string(g_cur.ps.timeMs) +
       (g_cur.opName == "balance" ? (" pts=" + std::to_string(g_cur.ps.movePts.size())) : std::string()) +
       (g_cur.superseded ? " [superseded]" : ""));
}

// ============================================================ 8) 对外接口
bool Init() {
  StartWorker();
  LoadPrefs();
  bool ok = Probe();
  ELOG(std::string("Init: ") + (ok ? "ok" : "failed") + " variant=" +
       (g_variant.empty() ? "(none)" : g_variant));
  return ok;
}

bool Available() { return Probe(); }
const std::string& Diag() { return g_diag; }

void Shutdown() {
  if (g_e.hProcess) {
    WriteRaw("STOP\nEND\n");
    if (WaitForSingleObject(g_e.hProcess, 900) != WAIT_OBJECT_0)
      TerminateProcess(g_e.hProcess, 0);
    CloseHandle(g_e.hProcess);
    g_e.hProcess = nullptr;
  }
  { std::lock_guard<std::mutex> lk(g_writeMu); if (g_e.hStdin) { CloseHandle(g_e.hStdin); g_e.hStdin = nullptr; } }
  if (g_e.alive.exchange(false)) ELOG("Shutdown: 引擎已停止");
  {
    std::lock_guard<std::mutex> lk(g_stMu);
    g_cur = Cur();
    g_drain = 0;
  }
  g_stCv.notify_all();
  {
    std::lock_guard<std::mutex> lk(g_qMu);
    g_workerStop = true;   // 必须置位：否则 worker 会永远停在 g_qCv 上，join 死等
    g_queue.clear();
  }
  g_qCv.notify_all();
  // worker 是全局 joinable std::thread：静态析构时若仍未 join 会 std::terminate()（0xC0000409）。
  // 退出路径必须把它收干净；join 后再复位标志，允许后续重新 StartWorker。
  if (g_worker.joinable()) g_worker.join();
  g_workerRun  = false;
  g_workerStop = false;
}

Caps GetCaps() {
  Caps c;
  Probe();
  c.available   = g_avail;
  c.variant     = g_variant;
  c.engineSource = g_variant.empty() ? std::string() : ReadEngineSourceTag(g_variant);
  CpuCaps k = CpuIdCaps();
  c.hasSSE = k.sse; c.hasAVX2 = k.avx2; c.hasAVX512 = k.avx512f;
  c.hasAVX512VNNI = k.avx512vnni; c.hasAVXVNNI = k.avxvnni;
  c.hasVNNI = k.avx512vnni || k.avxvnni;
  c.physCores  = host::PhysCores();
  c.logicCores = host::LogicCores();
  c.ramMB      = (int)(host::TotalPhysKB() / 1024);
  c.threads    = EffectiveThreads();
  c.hashMB     = EffectiveHashKB() / 1024;
  c.userThreads = g_usrThreads;
  c.userHashMB  = g_usrHashMB;
  c.diag        = g_diag;

  // 档位：1,2,4,8,16,32(≤物理核) ∪ {floor(物理核/2)} ∪ {物理核-1} ∪ {宿主上限}
  std::vector<int> o;
  int phys = c.physCores > 0 ? c.physCores : 2;
  for (int v : {1, 2, 4, 8, 16, 32}) if (v <= phys) o.push_back(v);
  o.push_back(std::max(1, phys / 2));
  o.push_back(std::max(1, phys - 1));
  if (host::ThreadsMaxHost() >= 2) o.push_back(host::ThreadsMaxHost());
  std::sort(o.begin(), o.end());
  o.erase(std::unique(o.begin(), o.end()), o.end());
  c.threadOptions = o;
  return c;
}

void HandleOp(const std::string& op, int id, const std::string& q, bool fromLink) {
  if (op == "ask" || op == "balance" || op == "live") {
    AskRequest req;
    req.board     = QInt2D(q, "board");
    req.side      = QInt(q, "side");
    req.rule      = QInt(q, "rule");
    req.budgetMs  = QInt(q, "budgetMs");
    req.topN      = QInt(q, "topN");
    req.pondering = QBool(q, "pondering");
    req.live      = (op == "live") || QBool(q, "live");
    req.key       = (uint64_t)QI64(q, "key");
    // ★ 2026-10-22：扫描防守 —— 页面在 ask / live 的 q 里带 "mode":3，
    //   宿主据此走引擎原生 YXSEARCHDEFEND（全点 multipv，Rapfi 整合包同一算法）。
    if (op != "balance") req.mode = QInt(q, "mode");
    if (op == "live") { req.opName = "live"; req.pondering = false; }
    if (op == "balance") {
      // 官方平衡点（YXBALANCEONE / YXBALANCETWO）：一次性算完即停，不走 pondering。
      req.opName    = "balance";
      req.mode      = (QStr(q, "mode") == "two") ? 2 : 1;
      req.bias      = QInt(q, "bias");
      req.topN      = 1;
      req.pondering = false;
    }
    StartWorker();
    ELOG(op + " id=" + std::to_string(id) + (fromLink ? " [link]" : " [page]") +
         " side=" + std::to_string(req.side) + " rule=" + std::to_string(req.rule) +
         " budget=" + std::to_string(req.budgetMs) + "ms topN=" + std::to_string(req.topN) +
         " ponder=" + std::to_string(req.pondering ? 1 : 0) +
         (req.mode ? (" balMode=" + std::to_string(req.mode) + " bias=" + std::to_string(req.bias)) : std::string()) +
         " key=" + std::to_string((long long)req.key));
    // ★ 新 ask 立即抢占：作废在途搜索（DoAsk 会以 superseded 收官），并**丢弃更旧的排队项**
    //   （页面只关心最新盘面）。否则「排队等前一轮把整段预算跑完」会让落点永远滞后一个回合。
    bool preempted = false;
    {
      std::lock_guard<std::mutex> lk(g_stMu);
      if (g_cur.active && !g_cur.done) {
        g_cur.superseded = true;
        // 抢在「被 STOP 打断的旧搜索的终局着法」到达之前先把欠账挂上，
        // 否则那条尾巴会被误认成新 ask 的终局回执（drainBooked 保证只记一次）。
        if (!g_cur.drainBooked) { g_drain += g_cur.outstanding; g_cur.drainBooked = true; }
        preempted = true;
      }
    }
    if (preempted) { g_stCv.notify_all(); WriteRaw("STOP\n"); }
    {
      std::lock_guard<std::mutex> lk(g_qMu);
      g_queue.clear();
      Pending p;
      p.req = req; p.id = id; p.fromLink = fromLink;
      g_queue.push_back(p);
    }
    g_qCv.notify_all();
    return;
  }
  if (op == "stop") {
    std::lock_guard<std::mutex> lk(g_stMu);
    if (g_cur.active && !g_cur.done) {
      // 欠账记一次即可（见 DoAsk 收官路径）；drainBooked 防止两处重复计数，
      // 否则同一次作废欠两条，会把下几轮的终局着法吞掉。
      g_cur.superseded = true;
      if (!g_cur.drainBooked) { g_drain += g_cur.outstanding; g_cur.drainBooked = true; }
      ELOG("stop id=" + std::to_string(id) + " (当前 ask id=" + std::to_string(g_cur.id) + ")");
    }
    g_stCv.notify_all();
    // 真的打断搜索：引擎 thinking 期间只认 STOP
    WriteRaw("STOP\n");
    PostTo(fromLink, "{\"type\":\"engineAck\",\"id\":" + std::to_string(id) +
                     ",\"op\":\"stop\",\"ok\":true}");
    return;
  }
  if (op == "caps") {
    Caps c = GetCaps();
    char h[128];
    std::string r = "{\"available\":" + std::string(c.available ? "true" : "false");
    r += ",\"variant\":" + JsonQuote(c.variant);
    r += ",\"engineSource\":" + JsonQuote(c.engineSource);
    snprintf(h, sizeof(h), ",\"physCores\":%d,\"logicCores\":%d,\"ramMB\":%d", c.physCores, c.logicCores, c.ramMB);
    r += h;
    snprintf(h, sizeof(h), ",\"threads\":%d,\"hashMB\":%d,\"userThreads\":%d,\"userHashMB\":%d",
             c.threads, c.hashMB, c.userThreads, c.userHashMB);
    r += h;
    r += ",\"hasSSE\":" + std::string(c.hasSSE ? "true" : "false");
    r += ",\"hasAVX2\":" + std::string(c.hasAVX2 ? "true" : "false");
    r += ",\"hasAVX512\":" + std::string(c.hasAVX512 ? "true" : "false");
    r += ",\"hasAVX512VNNI\":" + std::string(c.hasAVX512VNNI ? "true" : "false");
    r += ",\"hasAVXVNNI\":" + std::string(c.hasAVXVNNI ? "true" : "false");
    r += ",\"hasVNNI\":" + std::string(c.hasVNNI ? "true" : "false");
    r += ",\"diag\":" + JsonQuote(c.diag);
    r += ",\"threadOptions\":[";
    for (size_t i = 0; i < c.threadOptions.size(); ++i) {
      if (i) r += ",";
      r += std::to_string(c.threadOptions[i]);
    }
    r += "]}";
    PostTo(fromLink, "{\"type\":\"engineAck\",\"id\":" + std::to_string(id) +
                     ",\"op\":\"caps\",\"ok\":true,\"res\":" + r + "}");
    return;
  }
  if (op == "config") {
    int t = QInt(q, "threads");
    int h = QInt(q, "hashMB");
    SetUserPrefs(t, h);
    Caps c = GetCaps();
    PostTo(fromLink, "{\"type\":\"engineAck\",\"id\":" + std::to_string(id) +
                     ",\"op\":\"config\",\"ok\":true,\"res\":{\"threads\":" + std::to_string(c.threads) +
                     ",\"hashMB\":" + std::to_string(c.hashMB) + "}}");
    return;
  }
  ELOG("未知 op: " + op);
}

}  // namespace eng
