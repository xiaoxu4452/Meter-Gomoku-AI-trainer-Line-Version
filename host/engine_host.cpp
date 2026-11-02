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
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
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
static bool QHas(const std::string& s, const char* key) {
  std::string k = std::string("\"") + key + "\":";
  return s.find(k) != std::string::npos;
}
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
  // ★ 2026-10-23（用户：「再次检查调度是否科学合理，发挥最高智力」）：
  //   「开口」不等于「就绪」—— Rapfi 打印 `MESSAGE Load config from ...` 之后还要解压 4 份
  //   NNUE 权重（~38MB）才输出 `MESSAGE Evaluator set to ...`。这段 300~800ms 是**完全静默**的，
  //   所以「静默 N 毫秒」无法区分「正在加载」和「加载完了」（实测静默 120ms 判据在 234ms
  //   就误判就绪，第一条 ask 白等掉大半预算）。
  //   改成主动探针：boot 里发 `ABOUT`，收到 `name="Rapfi"` 才置 ready —— 管道是 FIFO，
  //   ABOUT 的回执必然落在模型加载完成之后，这是唯一不会误判的信号。
  std::atomic<bool> ready{false};
  std::atomic<DWORD> lastOutMs{0};
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
// ★ 2026-10-24（用户：「参考 gomoku-calculator 加选点范围 / 最大深度 / 最大节点」）：
//   三条都是 gomocup 协议的 INFO 选项，页面从设置区下发，值变了才在下一轮生效。
//   · candRange 0..5 → INFO CAUTION_FACTOR（3 = square3_line4 = 三圈半，官方推荐）
//   · maxDepth 1..99 → INFO MAX_DEPTH（72 = 官方推荐上限；2 秒内实际到不了，纯兜底）
//   · maxNodes ≥1 → INFO MAX_NODE（0/未设 = 不限制）
//   ⚠ CAUTION_FACTOR 值一变，引擎会**重建 board**（gomocup.cpp:280）——所以只在变化时发。
static int          g_candRange      = -1;
static int          g_maxDepth       = -1;
static long long    g_maxNodes       = -1;
// ★ 2026-10-24：用户指定的引擎变体（"" = 自动探测，按 kVariantOrder 从高到低）。
static std::string  g_variantOverride;
// ★ 2026-10-23（用户：「对局设置里加引擎权重下拉」）：NNUE vs 纯传统估值。
//   由页面在 engineConfig 里下发（页面侧已按机器兜底：没有 AVX2 的老 CPU 默认 classic）。
//   切换时**重启引擎进程**——比运行中 RELOADCONFIG 干净（模型重解压 + TT 重建都不会留脏状态），
//   启动耗时 300~800ms，只在用户真的改下拉时发生一次。
static std::string  g_weightsMode = "nnue";    // "nnue" | "classic"

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
// YXHASHCLEAR 回执（引擎打印 `MESSAGE Transposition table cleared.`）；受 g_stMu 保护
static bool                    g_hashCleared = false;
// tick 推送间隔。★ 2026-10-31（用户：「黑白胜率条数字刷新率太快，3 次/s 就行」）：
//   原来 120ms（≈8.3 次/s）每次都要序列化整份候选 JSON 再跨 WebView 消息泵投递，
//   评估/分析时 8 线程引擎正在吃满物理核，这些消息会和搜索抢同一批核。
//   333ms = 3 次/s，引擎侧不受影响，页面数字也不再跳得看不清。
static const DWORD             kTickMs = 333;

struct Pending { AskRequest req; int id = 0; bool fromLink = false; };
static std::deque<Pending>     g_queue;
static std::mutex              g_qMu;
static std::condition_variable g_qCv;
static std::thread             g_worker;
static bool                    g_workerRun = false;
static bool                    g_workerStop = false;
// ★ 2026-10-31（用户：「分析计算」落子后常常不重新评估，仪表盘一直卡在上一手的用时）：
//   过去 tick 只在 OnEngineLine 收到 INFO 行时才发 —— Rapfi 长考时某一层迭代可能十几秒
//   没有新 INFO，页面就完全收不到帧（live 任务预算 3600000ms，也不会自己收官）。
//   这条心跳线程只要还有在途搜索，就按 kTickMs 用**最后一次解析到的状态**继续发 tick：
//   深度 / 评估停在最后一层（正确），用时和节点帧持续刷新。
static std::thread             g_tickThread;
static std::atomic<bool>       g_tickRun{false};

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
  // ★ 2026-10-24（用户：「我的电脑可能不支持 AVX-VNNI」）：leaf 7 subleaf 1 必须先确认存在。
  //   规范上不支持的 subleaf 返回 0，但有些老 CPU/虚拟机会回显 subleaf 0 的数据 ⇒ 会误判
  //   AVX-VNNI 可用，选上去引擎直接 0xC000001D 非法指令。这里按 maxSubLeaf 严格门控。
  unsigned maxSub = (unsigned)r[0];
  bool avx2bit = (r[1] >> 5) & 1;
  bool f512 = (r[1] >> 16) & 1, dq = (r[1] >> 17) & 1, bw = (r[1] >> 30) & 1, vl = (r[1] >> 31) & 1;
  bool v512 = (r[2] >> 11) & 1;
  c.avx2       = avxSup && xmmYmm && avx2bit;
  c.avx512f    = c.avx2 && zmm && f512 && dq && bw && vl;
  c.avx512vnni = c.avx512f && v512;
  if (maxSub >= 1) {
    __cpuidex(r, 7, 1);
    c.avxvnni = c.avx2 && (((unsigned)r[0] >> 4) & 1) != 0;
  }
  else {
    c.avxvnni = false;
  }
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

// ★ 2026-10-25（用户：「发给其他精通五子棋的用户，反应下降了，应该最近构建变体有了差异」）：
//   真凶找到 —— 自动基准把 **avx2** 判成「最快」并写进 .variant-bench（见下方 bench 注释），
//   Probe 又优先采用它，于是实际跑的是慢 3 倍的变体（速度↓ 准度↓）。
//   两道闸：① 基准只在**同一档位**内比（avx2 永远不能顶掉 avx512 系列）；
//          ② 档位顺序把 avx512 排在 avxvnni 之前（512 位 VNNI/无 VNNI 都比 256 位强）。
// ★ 2026-10-30（用户：「rapfi 智力下降」）：复查发现 10-24 的 PGO 版 avx512vnni
//   在多个中盘局面只有 16 万~20 万 nps（非 PGO 同款约 200 万），深度从 22~24 掉到
//   15~17，准度随之崩。发布版 avx512vnni 已换回稳定非 PGO 构建。
//   复查基准（3s×6 局面×3 轮，8 线程，1GB hash）：非 PGO avx512vnni 2684k nps /
//   深度 24.7 / 4.9M 节点，avx512 2483k / 23.6 / 4.4M —— VNNI 快约 8%、深约 1 层，
//   所以恢复 VNNI 首选（它早已不是那个慢 10 倍的 PGO 版本）。
static const char* kVariantOrder[] = {"avx512vnni", "avx512", "avxvnni", "avx2", "sse"};

static bool VariantCpuOk(const std::string& v, const CpuCaps& c) {
  if (v == "avx512vnni") return c.avx512vnni;
  if (v == "avxvnni")    return c.avxvnni;
  if (v == "avx512")     return c.avx512f;
  if (v == "avx2")       return c.avx2;
  return true;
}

// 计算宽度档位（只用于「不许跨档覆盖」的判断，不是速度排序）：
//   512 位（avx512/avx512vnni）=3，256 位 VNNI=2，256 位 AVX2=1，SSE=0
static int VariantTier(const std::string& v) {
  if (v == "avx512vnni" || v == "avx512") return 3;
  if (v == "avxvnni") return 2;
  if (v == "avx2")    return 1;
  return 0;
}

// 本机**支持得了**的最高档位（avx512 系列=3 / avxvnni=2 / avx2=1）
static int TopTier(const CpuCaps& c) {
  if (c.avx512f) return 3;
  if (c.avxvnni) return 2;
  if (c.avx2)    return 1;
  return 0;
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

// ============================================================ 3a) 本机变体实测（auto-bench）
// ★ 2026-10-24（用户：「变体自动选择最快最准的」）：CPUID 档位排序分不出同级变体 ——
//   8845H/Zen4 实测 avx512 与 avx512vnni 就在彼此的热噪声内（但 avx2 明显慢），
//   真实速度要**跑一下才知道**。Init 后台线程做一次短基准：每候选 暖场局 + 2 计分局，
//   固定中盘局面比 TOTALNODES（取较好一轮，抗偶发后台抢占）；赢家写 .variant-bench
//  （带 CPU 指纹，换机自动作废），Probe() 在用户指定之后、.variant-ok 之前采用。
//   测完若引擎空闲且当前变体不同 → 热切换（重启引擎进程）；忙则下次启动生效。
static long long BenchOneRound(const std::wstring& dir, const std::string& name, int threads) {
  std::wstring exe = Join(dir, std::wstring(L"RapfiEngine-") + Utf8ToWide(name) + L".exe");
  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
  if (!CreatePipe(&inR, &inW, &sa, 0)) return -1;
  if (!CreatePipe(&outR, &outW, &sa, 0)) { CloseHandle(inR); CloseHandle(inW); return -1; }
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
    return -1;
  }
  CloseHandle(pi.hThread);
  HANDLE job = host::Job();
  if (job) AssignProcessToJobObject(job, pi.hProcess);
  host::ChildTracked(pi.hProcess);
  CloseHandle(inR);
  // 固定中盘局面（7 手、无杀；同 thread-quality.js POS[1]），1s 预算比 TOTALNODES。
  std::string script =
      "START 15\nINFO RULE 0\nINFO SHOW_DETAIL 2\nINFO THREAD_NUM " + std::to_string(threads) +
      "\nINFO HASH_SIZE 65536\nINFO PONDERING 0\nINFO TIMEOUT_MATCH 6000000\nINFO TIME_LEFT 6000000\n"
      "INFO TIMEOUT_TURN 500\n"
      "YXBOARD 7,7,1 7,8,2 8,8,1 6,7,2 8,7,1 6,8,2 9,9,1 DONE\nYXNBEST 1\n";
  DWORD written = 0;
  WriteFile(inW, script.data(), (DWORD)script.size(), &written, nullptr);
  CloseHandle(inW);

  long long nodes = 0;
  bool sawMove = false;
  std::string pending;
  char rb[4096];
  const DWORD t0 = NowMs();
  while (NowMs() - t0 < 4000) {
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
          if (line.compare(0, 16, "INFO TOTALNODES ") == 0) {
            long long v = _atoi64(line.c_str() + 16);
            if (v > nodes) nodes = v;
          } else if (IsBareMove(line)) {
            sawMove = true;
          }
        }
      }
    }
    if (sawMove) break;
    DWORD code2 = 0;
    if (GetExitCodeProcess(pi.hProcess, &code2) && code2 != STILL_ACTIVE) break;
    Sleep(30);
  }
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  if (code == STILL_ACTIVE) TerminateProcess(pi.hProcess, 0);
  CloseHandle(pi.hProcess);
  CloseHandle(outR);
  return sawMove ? nodes : -1;
}

static std::atomic<bool> g_benchRun{false};

static void BenchThreadMain() {
  // 等应用启动高峰过去（页面/识别都在抢 CPU 时测不准），再开始实测
  for (int i = 0; i < 8 && g_benchRun; ++i) Sleep(500);
  if (!g_benchRun) return;
  CpuCaps caps = CpuIdCaps();
  // [SCI] Bench with a single thread: relative variant speed ranking is stable
  // across thread counts, so a full-core bench would burn CPU for no ranking value.
  int threads = 1;
  std::string cur = g_variant;                 // Probe() 已定的当前变体

  // ★ 2026-10-25：旧实现按变体**连续**测 3 轮，第一个变体吃到启动/杀软扫描的尾巴 ——
  //   实测日志 avx512vnni=1099k、avx512=1231k、avx2=1783k，看起来 avx2 快 62%；
  //   但同一个 8 线程固定局面复测 avx2 与 avx512vnni 都是 ~1.1M nodes/s（差 2%）。
  //   改成腾场后**按轮次交错**测、取 3 轮中位数，顺序偏差和偶发抢占一起消掉。
  struct Cand { std::string name; long long s[3]; bool dead; long long score; };
  std::vector<Cand> cands;
  const int topTier = TopTier(caps);
  for (const char* v : kVariantOrder) {
    std::string name(v);
    if (name == "sse") break;                  // sse 只是兜底，不参与比测
    if (!VariantCpuOk(name, caps)) continue;
    // ★ 2026-10-25：只在**同一档位**内比（本机 avx512 系列 → 只让 avx512/avx512vnni 竞争）。
    //   1s 局面的节点数受「早出杀 / 到达层数不同」影响极大，跨档比出来的赢家会是慢 3 倍的
    //   avx2（历史事故）。跨档谁快是确定事实，不需要实测。
    if (VariantTier(name) != topTier) continue;
    if (!FileExists(Join(g_dir, std::wstring(L"RapfiEngine-") + Utf8ToWide(name) + L".exe"))) continue;
    cands.push_back({name, {-1, -1, -1}, false, -1});
  }
  if (cands.size() <= 1 || !g_benchRun) return;
  for (auto& c : cands) {                      // 每个变体先暖一场（文件缓存 / 分支预测）
    if (!g_benchRun) return;
    if (BenchOneRound(g_dir, c.name, threads) < 0) {
      ELOG("auto-bench: " + c.name + " 跑不动（非法指令？）跳过");
      c.dead = true;
    }
  }
  // [SCI] One round per variant is enough for ranking (median of 3 adds little).
  for (int r = 0; r < 1 && g_benchRun; ++r)
    for (auto& c : cands) {
      if (c.dead || !g_benchRun) continue;
      c.s[r] = BenchOneRound(g_dir, c.name, threads);
    }

  std::string log;
  std::string best;
  long long bestN = -1, curN = -1;
  for (auto& c : cands) {
    if (c.dead) continue;
    long long v[3];
    int n = 0;
    for (long long x : c.s) if (x > 0) v[n++] = x;
    long long sc = -1;
    if (n >= 3)      sc = std::max(std::min(v[0], v[1]), std::min(std::max(v[0], v[1]), v[2]));
    else if (n == 2) sc = std::min(v[0], v[1]);
    else if (n == 1) sc = v[0];
    if (sc < 0) continue;
    c.score = sc;
    log += c.name + "=" + std::to_string(sc / 1000) + "k ";
    if (sc > bestN) { bestN = sc; best = c.name; }
    if (c.name == cur) curN = sc;
  }
  if (!g_benchRun || best.empty()) return;
  // ★ 2026-10-25：跨档兜底闸（任何情况下都不许把低档位写进缓存）。
  if (VariantTier(best) < TopTier(caps)) { ELOG("auto-bench: 判定跨档，忽略（" + best + "）"); return; }
  // 当前变体与最优差距 < 12% 就不切换：同级变体本就在热噪声内（实测 avx512 与 avx512vnni
  // 复测差 2%），为 5% 的噪声白重启一次引擎、丢掉满置换表，纯亏。
  if (curN > 0 && curN * 100 >= bestN * 88) best = cur;
  ELOG("auto-bench（T" + std::to_string(threads) + " 节点/局，1 轮）: " + log + "→ " + best);
  {
    FILE* f = _wfopen(Join(g_dir, L".variant-bench").c_str(), L"wb");
    if (f) {
      std::string rec = best + " " + CpuFingerprint() + "\n";
      fwrite(rec.data(), 1, rec.size(), f);
      fclose(f);
    }
  }
  if (best == cur || !g_variantOverride.empty()) return;   // 用户已手选 / 本来就是它
  bool busy = false;
  { std::lock_guard<std::mutex> ls(g_stMu); busy = g_cur.active || g_cur.alive; }
  if (busy) { ELOG("auto-bench: 引擎忙，" + best + " 下次启动生效"); return; }
  g_probed = false; g_avail = false; g_variant.clear();
  if (g_e.alive) {
    std::lock_guard<std::mutex> lk(g_e.spawnMu);
    if (g_e.hProcess) { TerminateProcess(g_e.hProcess, 0); CloseHandle(g_e.hProcess); g_e.hProcess = nullptr; }
    { std::lock_guard<std::mutex> lw(g_writeMu); if (g_e.hStdin) { CloseHandle(g_e.hStdin); g_e.hStdin = nullptr; } }
    g_e.alive = false;
    { std::lock_guard<std::mutex> ls(g_stMu);
      g_cur.active = false; g_cur.alive = false; g_cur.done = true; g_drain = 0; }
    g_stCv.notify_all();
  }
  ELOG("auto-bench: 热切换 → " + best + "（下次 ask 生效）");
  host::OnVariantChanged();
}


static std::string ReadBenchCache() {
  FILE* f = _wfopen(Join(g_dir, L".variant-bench").c_str(), L"rb");
  if (!f) return std::string();
  char buf[256] = {0};
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  std::string cached(buf, n);
  while (!cached.empty() && (cached.back() == '\n' || cached.back() == '\r' || cached.back() == ' '))
    cached.pop_back();
  size_t sp = cached.find(' ');
  std::string v = (sp != std::string::npos) ? cached.substr(0, sp) : cached;
  std::string fp = (sp != std::string::npos) ? cached.substr(sp + 1) : std::string();
  if (v.empty() || fp != CpuFingerprint()) return std::string();
  // ★ 2026-10-25 硬闸：缓存里的变体必须是本机最高档位。历史上 bench 曾把 avx2（慢 3 倍）
  //   写进来并接管了实际运行 ▸ 老缓存一律作废，不在同一档位就当作没有缓存。
  CpuCaps caps = CpuIdCaps();
  if (VariantTier(v) < TopTier(caps)) {
    ELOG("auto-bench 缓存跨档（" + v + " 低于本机最高档位）→ 作废");
    return std::string();
  }
  return v;
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
  // ★ 2026-10-24（用户：「引擎变体也让用户选，默认配置最好的，不支持的别显示」）：
  //   页面指定的变体优先于自动探测；不合法/文件缺失就清空它回退自动（不报错、不卡住）。
  if (!g_variantOverride.empty()) {
    if (VariantCpuOk(g_variantOverride, caps) &&
        FileExists(Join(dir, std::wstring(L"RapfiEngine-") + Utf8ToWide(g_variantOverride) + L".exe"))) {
      g_variant = g_variantOverride;
      g_avail   = true;
      ELOG("可用（用户指定变体）：variant=" + g_variant);
      return true;
    }
    ELOG("用户指定的变体不可用 → 回退自动探测：" + g_variantOverride);
    g_variantOverride.clear();
  }
  ELOG("cpuid: avx2=" + std::string(caps.avx2 ? "1" : "0") +
       " avx512f=" + std::string(caps.avx512f ? "1" : "0") +
       " avx512vnni=" + std::string(caps.avx512vnni ? "1" : "0") +
       " avxvnni=" + std::string(caps.avxvnni ? "1" : "0"));

  {  // ★ 2026-10-24：本机实测最快变体缓存（后台基准线程写；带 CPU 指纹，换机自动作废）。
     //   排在用户指定之后、.variant-ok（沿用上次）之前 —— 「实测最快」优先于「上次碰巧用过」。
    std::string v = ReadBenchCache();
    if (!v.empty() && VariantCpuOk(v, caps) &&
        FileExists(Join(dir, std::wstring(L"RapfiEngine-") + Utf8ToWide(v) + L".exe"))) {
      g_variant = v;
      g_avail = true;
      ELOG("可用（本机实测最快）：variant=" + g_variant);
      return true;
    }
    if (!v.empty()) ELOG(".variant-bench 与本机不匹配 → 跳过（" + v + "）");
  }

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
  if (hostMax > mx) mx = hostMax;   // 保留实测最优档（物理核）
  return mx < 1 ? 1 : mx;
}
static int DefaultThreads() {
  // ★ 实测最优档＝逻辑核 - 视觉预留，与宿主 ThreadsDefault() 同口径。
  //   勿改回「物理核/2」——那会让引擎 nps 直接腰斩（2026-10-16 已实测并回退）。
  int t = host::ThreadsMaxHost();
  return t < 1 ? 1 : t;
}
static int DefaultHashKB() {
  // [SCI] RAM-adaptive default: >=16 GB -> 512 MB, >=8 GB -> 256 MB, else 128 MB.
  // A fixed 512 MB table on an 8 GB thin laptop wastes RAM the OS/UI could use.
  long long ramMB = host::TotalPhysKB() / 1024;
  if (ramMB >= 16384) return 512 * 1024;
  if (ramMB >= 8192)  return 256 * 1024;
  return 128 * 1024;
}
static int ClampHashKB(int kb) {
  long long ramKB = host::TotalPhysKB();
  long long hi = std::min<long long>(4096LL * 1024, std::max<long long>(262144, ramKB / 4));
  if (kb < 131072) kb = 131072;  // [SCI] floor 128 MB (was 256 MB)
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
  // ★ 2026-10-24（用户：「调用 CPU 不要太激进，容易过热降频」）：
  //   原来给 ABOVE_NORMAL，引擎会和界面/识别抢调度权，笔记本上更容易顶到温度墙。
  //   改回 NORMAL：线程数本来就压到物理核，优先级再拉高只是把 UI 也拖慢（JS 卡 → 手感迟钝）。
  SetPriorityClass(pi.hProcess, NORMAL_PRIORITY_CLASS);
  host::PinHostAway();
  host::ChildTracked(pi.hProcess);

  g_e.hProcess = pi.hProcess;
  g_e.hStdin   = inW;
  int myGen = ++g_e.gen;
  g_e.spoke = false;
  g_e.ready = false;
  g_e.lastOutMs = NowMs();
  g_e.alive = true;
  std::thread(ReaderThread, outR, myGen).detach();

  g_appliedRule    = -1;      // 新进程需要重新下发规则/思考模式
  g_appliedPonder  = -1;
  g_appliedSwapable = -1;
  g_appliedThreads = EffectiveThreads();
  g_appliedHashKB  = EffectiveHashKB();
  // ★★ 2026-10-25（用户：「我们的软件没有友好的使用这些参数」）：
  //   CAUTION_FACTOR（= 对局设置里的 style 谨慎度）在 boot 里**总是**下发，且排在 START 之前。
  //   两个作用：
  //     1) 首次建 Board 就用用户选的候选范围，少一次「保存历史→重建 Board→重放」；
  //     2) 更关键 —— 若 boot 不发，第一次 ask 才发时 candRange 还是 nullopt，
  //        引擎源码（rapfi/command/gomocup.cpp:294）会因 `!prevCandRange.has_value()` 判定为
  //        「变了」，于是重建 Board 并 `Search::Engine.clear(true)` **清空整个置换表**。
  //        前置之后 prevCandRange 已有值且相同，这次无谓的清空就没有了。
  //   （注：运行中改 style 无需重启进程 —— 同上那段源码会在值变化时自己热重建 Board。）
  int crBoot = (g_candRange >= 0 && g_candRange <= 5) ? g_candRange : 3;
  std::string boot = "INFO CAUTION_FACTOR " + std::to_string(crBoot) + "\n";
  // ★ 2026-10-30：SHOW_DETAIL 3 = INFO_DETAIL + INFO_REALTIME。宿主只解析 `INFO` 帧
  //   （EVAL/WINRATE/BESTLINE），从未读取 `MESSAGE REALTIME REFRESH`；0.44 引擎在
  //   realtime 档会每轮额外刷新整行 MESSAGE，纯属管道噪音。降到 2 保留全部结构化
  //   候选信息，同时少一路输出、少一次 writer 竞争。
  boot += "START 15\nINFO RULE 0\nINFO SHOW_DETAIL 2\n";
  // 纯传统估值：启动后立刻把配置切成 config-classic.toml（那份只是把 [model.evaluator]
  // 与它的 weights 子表整段注释掉，其它参数与本目录 config.toml 完全一致）。
  // ★ 必须在 START 之后（RELOADCONFIG 需要已建好 board），且**要在 THREAD_NUM/HASH_SIZE 之前**
  //   —— reloadConfig 会按 config 的 default_tt_size_kb 重建置换表，放在后面会把用户设的
  //   哈希大小冲掉。
  if (g_weightsMode == "classic") boot += "RELOADCONFIG config-classic.toml\n";
  boot += "INFO THREAD_NUM " + std::to_string(g_appliedThreads) + "\n";
  boot += "INFO HASH_SIZE "  + std::to_string(g_appliedHashKB)  + "\n";
  // 就绪探针：ABOUT 的回执一定排在「模型全部加载完之后」（管道 FIFO），见 Engine::ready 注释。
  boot += "ABOUT\n";
  WriteRaw(boot);
  ELOG("已启动 " + WideToUtf8(exe) + " (gen=" + std::to_string(myGen) +
       ", threads=" + std::to_string(g_appliedThreads) +
       ", hashKB=" + std::to_string(g_appliedHashKB) + ")");

  // ★ 2026-10-23（用户：「软件没有适配这个引擎，导致算力质量很低」）：
  //   等引擎**真正开口**（stdout 第一行）再放行 ask。0.44 启动要解压 4 份 NNUE（38MB）
  //   并分配置换表，实测 300~800ms；这段窗口里发下去的 ask 会在页面侧 375ms 超时、
  //   返回空候选，页面就弹「引擎还没就绪」并白跑一轮。最多等 2.5s，超时也继续（宁可慢不卡死）。
  DWORD tSpoke0 = NowMs();
  // 等 ABOUT 回执（= 模型加载完、主循环能处理命令）。最多 5s，超时也继续（慢机器/杀软扫描）。
  for (;;) {
    if (!g_e.alive.load()) break;
    if (g_e.ready.load()) break;
    if (NowMs() - tSpoke0 > 5000) break;
    Sleep(10);
  }
  if (g_e.ready.load())
    ELOG("引擎已就绪（ABOUT 探针用时 " + std::to_string(NowMs() - tSpoke0) + "ms）");
  else
    ELOG("引擎 5s 内没有回应 ABOUT，继续（首次加载模型可能更慢）");
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
        g_e.spoke = true;          // 引擎开口（第一行 stdout）
        g_e.lastOutMs = NowMs();
        // ABOUT 的回执 = 主循环已就绪（SpawnLocked 在等这个）
        if (line.compare(0, 12, "name=\"Rapfi\"") == 0) g_e.ready = true;
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
// ★ 2026-11-01：把「本轮」候选整轮提交成对外快照（见 engine_host.h ParseState::pend 注释）。
//   只在轮的边界调用：① 下一轮 `INFO PV 0` 到达；② 搜索结束（裸着法 / SWAP）之前。
//   空轮不提交，避免把还没开始的一轮当成结果把上一轮冲掉。
static void CommitRound(ParseState& st) {
  if (st.pend.empty()) return;
  st.cands.swap(st.pend);
  st.pend.clear();
  st.numPv = st.pendNumPv;
  // 深度也只按「已提交的整轮」推进：面板上的深度 = 完成深度，和候选表同一轮次。
  // （未提交的那一轮还在发名次，它的 DEPTH 已报出来但候选不全，显示它会让深度与
  //   候选错位一格 —— 官方 `MESSAGE Depth <完成>-<当前>` 的「完成」就是这个口径。）
  for (const Candidate& c : st.cands)
    if (c.x >= 0 && c.depth > st.maxDepth) st.maxDepth = c.depth;
}

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
          if ((size_t)st.pvIdx >= st.pend.size()) st.pend.resize((size_t)st.pvIdx + 1);
          st.pend[(size_t)st.pvIdx] = st.cur;
        }
        st.inPv = false;
      } else {
        int idx = atoi(val.c_str());
        // 名次回到 0 = 上一轮已经发完 ⇒ 整轮提交；一个名次的块内字段随后才陆续到达。
        if (idx == 0) { CommitRound(st); st.pendNumPv = 0; }
        st.pvIdx = idx;
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
      st.pendNumPv = n;
      if (n >= 0 && (size_t)n < st.pend.size()) st.pend.resize((size_t)n);
    }
    else if (tok == "DEPTH")      st.cur.depth = atoi(val.c_str());
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
  if (line == "SWAP") { CommitRound(st); st.swap = true; st.finished = true; return; }
  if (IsBareMove(line)) {
    auto c = ParseCoordList(line);
    // BALANCE_TWO 的终局回执是两个坐标（`x1,y1 x2,y2`），BALANCE_ONE / YXNBEST 是一个。
    // 全部收进 movePts，bestX/bestY 取首着（普通 ask 只需首着）。
    // ★ 搜索结束前把最后（可能只发了一半名次的）一轮也提交：cands[0] 必须与这条裸着法
    //   一致，页面用 r.best 落子、用 c0 取评分，两者不能来自不同的轮次。
    if (!c.empty()) { CommitRound(st); st.movePts = c; st.bestX = c[0].first; st.bestY = c[0].second; st.finished = true; }
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
  // ★★ 2026-10-25（用户：「分析的点数，有的是同一点，不能是多个点再同一个，如果重合，
  //   去掉评分低的那个；上限是这个点数，但不一定全显示」→「只是引擎里面这样做」）：
  //   候选**去重放在引擎回执这一层**，页面（连线版 calc.js / 练习器 calc.js）不再各写一份。
  //   根因在多线程 Lazy SMP：几个搜索线程以**不同深度**各自发一轮 `INFO PV <idx>`，
  //   而 PV 是按「名次槽位」回填的 —— 上一轮的第 3 名这一轮升到第 1 名时旧槽位不会清，
  //   同一个坐标就同时占住两个槽 ⇒ 棋盘叠两个圈、右侧列表出现两行同一点。
  //   这里按 (x,y) 保序去重：Rapfi 的 PV 名次 = 分数降序，先出现即名次高、评分高，
  //   后来的重复项（低分）直接丢弃。去重后条数 ≤ 页面设的「分析点数」上限，
  //   但**不保证等于**——引擎本来就可能给不满（局面窄、候选范围小、已出杀）。
  //   ⚠ 必须在输出层去重而非 st.cands 存储层：槽位下标 = PV 序号，中途删一项会让
  //     后续 PV 覆盖到错误的槽，跨轮彻底错乱。
  std::vector<std::pair<int, int>> seenCd;
  seenCd.reserve(16);
  r += ",\"candidates\":[";
  bool first = true;
  for (const Candidate& cd : ps.cands) {
    if (cd.x < 0) continue;
    bool dup = false;
    for (size_t si = 0; si < seenCd.size(); ++si) {
      if (seenCd[si].first == cd.x && seenCd[si].second == cd.y) { dup = true; break; }
    }
    if (dup) continue;                          // 同一点：保留先出现（名次/评分更高）的那条
    seenCd.push_back(std::make_pair(cd.x, cd.y));
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

// ★ 2026-11-01（用户：「不要用你的思路，要用 Rapfi-YixinBoard 和官方 Rapfi 的思路」）：
//   这一段以前是**宿主自创**的加速/省时策略（GB_STOP_RATIO 手工反算预算、START_DEPTH 跳层、
//   按上一手 eval/着法稳定性给系数、时间池透支）。官方 Rapfi 一件都没有：
//     · 每手时间全部由 search/timecontrol.cpp 内部算：
//         maximum = min(TIMEOUT_TURN, TIMEOUT_MATCH 分账) - turnTimeReserved(30ms)
//         optimum = maximum * advanced_stop_ratio（官方默认 0.9，见 searchconfig.h:81）
//       协议层只把界面给的每手时间**原样**下发（command/gomocup.cpp:185-196 setTimeControl），
//       不做任何加减乘除 —— 参考包 Rapfi-YixinBoard 也是这么用的。
//     · 从第几层起跑由 config.toml 的 startDepth（官方默认 2，searchcommon.h:137）决定；
//       协议里的 START_DEPTH 只是 Yixin 扩展，官方整合包 / YixinBoard 从不发送。
//   所以上述状态全部删除，宿主只做协议直通（时间语义映射见 BuildAskCmds 里的注释）。

// 本轮是否用了增量 TURN（DoAsk 的看门狗据此决定要不要降级整盘重发）
static bool g_askIncr = false;
// 本轮下发的 `INFO TIMEOUT_MATCH`（仅用于 ack 日志，便于事后核对时间语义）
static int  g_lastMatchMs = 0;

// （2026-11-01：上面这些自创的时间系数/时间池/跳层函数已按用户要求全部删除，
//   宿主不再猜测「这一手值多少毫秒」——那是引擎 search/timecontrol.cpp 的职责。）

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
    // ★★ 2026-11-01 21:2x（用户：「直接恢复 4.46 的引擎逻辑」）：回到 4.46 的 1 小时长任务。
    //   引擎侧 TIMEOUT_TURN 给足，由页面的 stop / 落子抢占来收尾（= 4.46 的持续分析语义）。
    budget = 3600000;
  } else {
    if (budget < 200)   budget = 200;
    if (budget > 60000) budget = 60000;
    // ★★ 同样回到 4.46：界面设的 N ms **原样**下发为 TIMEOUT_TURN。
    //   引擎内部按官方公式收手（search/timecontrol.cpp）：
    //     maximum = min(TIMEOUT_TURN, 分账) - turnTimeReserved(30ms)
    //     optimum = maximum * advanced_stop_ratio（config.toml [search.timectl] = 0.94）
    //   宿主不再做任何倍率换算 —— 4.46 就是原样下发，倍率只由引擎自己的配置决定。
  }
  int topN = req.topN < 1 ? 1 : (req.topN > 8 ? 8 : req.topN);

  // ---- 增量判定（见上方 g_seq* 注释）：新盘面 = 上一轮盘面 + 恰好一手 ----
  // ★ 2026-10-25：判定改成「集合包含 + 恰好多一个」，不再要求新子排在行主序末尾。
  //   旧实现的 std::equal 前缀比较只在「新子恰好是这一色最靠后的一颗」时成立，
  //   实战几乎永远不中 ⇒ TURN 通道等于没用（日志 0 条「增量下发 TURN」）。
  // 注意：bl / wh 是**行主序**（y 外层）建的，std::pair 的默认序是 x 优先，两者不一致；
  // 这里用「逐个查找」做集合判定，不依赖任何排序（n ≤ 225，一次 ask 一趟，开销可忽略）。
  auto hasPt = [](const std::vector<std::pair<int, int>>& v, const std::pair<int, int>& p) {
    return std::find(v.begin(), v.end(), p) != v.end();
  };
  auto hasAll = [&](const std::vector<std::pair<int, int>>& small,
                    const std::vector<std::pair<int, int>>& big) {
    for (const auto& p : small) if (!hasPt(big, p)) return false;
    return true;
  };
  bool incr = false;
  int  ix = -1, iy = -1;
  // ★ 2026-10-31：带 clearHash 的轮次一律整盘重发 —— 清 TT 时评估/常驻分析的 STOP
  //   可能已让引擎把它的最优手补进内部盘面（sendActionAndUpdateBoard），此时 TURN 会
  //   把新子叠到错误的盘面上。
  if (!forceFull && !req.clearHash && g_seqValid && !g_seqNoIncr && req.mode == 0 && topN == 1 && g_seqSide != 0) {
    int newColor = 0;
    if (bl.size() == g_seqBl.size() && wh.size() == g_seqWh.size() + 1 &&
        hasAll(g_seqBl, bl) && hasAll(g_seqWh, wh))
      newColor = 2;                                   // 只多了一个白子
    else if (wh.size() == g_seqWh.size() && bl.size() == g_seqBl.size() + 1 &&
             hasAll(g_seqWh, wh) && hasAll(g_seqBl, bl))
      newColor = 1;                                   // 只多了一个黑子
    // 新子必须是「上一轮行棋方的对手」，否则说明盘面被悔棋 / 覆盖，退回整盘重发
    if (newColor && newColor == 3 - g_seqSide) {
      const std::vector<std::pair<int, int>>& src = (newColor == 2) ? wh : bl;
      const std::vector<std::pair<int, int>>& old = (newColor == 2) ? g_seqWh : g_seqBl;
      for (const auto& p : src)
        if (!hasPt(old, p)) { ix = p.first; iy = p.second; incr = true; break; }
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
  // 选点范围 / 最大深度 / 最大节点：每轮重发是安全的（值不变时引擎不会重建 board）。
  // ★★ 2026-10-24 BUGFIX（用户：「分析计算算了几分钟又会开始重新算」）：MAX_DEPTH/MAX_NODE
  //   是**对局着法**的兜底（2 秒摸不到 72 层才兜）；不限时分析（live）里它们成了定时炸弹 ——
  //   引擎深算几分钟摸到上限就自己收官，页面 350ms 后重发 ask ⇒ 看起来「从头重算」。
  //   live 一律不下发这两个上限（引擎 config 的 max_search_depth=99 兜底，实际到不了）。
  if (g_candRange >= 0) c += "INFO CAUTION_FACTOR " + std::to_string(g_candRange) + "\n";
  if (!req.live) {
    if (g_maxDepth >= 2)  c += "INFO MAX_DEPTH " + std::to_string(g_maxDepth) + "\n";
    if (g_maxNodes >= 1)  c += "INFO MAX_NODE "  + std::to_string(g_maxNodes) + "\n";
  }
  c += "INFO TIMEOUT_TURN " + std::to_string(budget) + "\n";
  // ★★ 2026-11-01 21:2x（用户：「直接恢复 4.46 的引擎逻辑」）：
  //   时间控制回到 4.46 的**两条固定命令**，宿主不做任何分账 / 动态换算：
  //     INFO TIMEOUT_TURN  = 界面给的每手 N ms（原样）
  //     INFO TIMEOUT_MATCH = 6000000（100 分钟总时限，恒发）
  //     INFO TIME_LEFT     = 6000000（与总时限同步，引擎每手都按「时间充裕」走 ample 分支）
  //   引擎内部再按 search/timecontrol.cpp 的官方公式收手：
  //     optimum = advanced_stop_ratio(0.94) × (min(turn, 分账) - turnTimeReserved(30ms))
  //   注意：TIMEOUT_MATCH 给小了会掉进非 ample 省时分支（实测少 3 层），4.46 从不这么干。
  g_lastMatchMs = 6000000;
  c += "INFO TIMEOUT_MATCH 6000000\nINFO TIME_LEFT 6000000\n";
  // 迭代加深的起跑层由 config.toml 的 startDepth（官方默认 2）决定；
  // 不再下发 INFO START_DEPTH（Yixin 扩展，官方整合包从不使用，跳层会吞掉前 N 层的 INFO 输出）。
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
  { char dbg[8]={0}; if (GetEnvironmentVariableA("GB_CMD_LOG", dbg, sizeof(dbg))>0) ELOG("[cmd]\n" + c); }
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

static void PostTick(bool fromLink, int id, const std::string& json) {
  PostTo(fromLink, "{\"type\":\"engineAsk\",\"id\":" + std::to_string(id) +
                   ",\"phase\":\"tick\",\"res\":" + json + "}");
}

static void TickThreadMain() {
  while (g_tickRun.load()) {
    Sleep(100);
    bool post = false, fromLink = false;
    int id = 0;
    std::string json;
    {
      std::lock_guard<std::mutex> lk(g_stMu);
      if (!g_cur.active || g_cur.done || g_cur.superseded || !g_cur.alive) continue;
      DWORD now = NowMs();
      if (now - g_cur.lastTick < kTickMs) continue;
      g_cur.lastTick = now;
      post = true;
      fromLink = g_cur.fromLink;
      id = g_cur.id;
      json = ResJson(g_cur);
    }
    if (post) PostTick(fromLink, id, json);
  }
}

// reader 线程：逐行解析 → 节流 tick 推送；**终局回执只由 worker 统一投递**（见 DoAsk 尾部）
static void OnEngineLine(const std::string& line) {
  bool postTick = false, fromLink = false, justDone = false;
  int id = 0;
  std::string tickJson;
  // ★ 2026-10-31：YXHASHCLEAR 的回执是 `MESSAGE Transposition table cleared.`。
  //   必须在下面的「无在途 ask 就丢弃」之前截获 —— 清 TT 通常发生在两轮 ask 之间。
  if (line.compare(0, 8, "MESSAGE ") == 0) {
    if (line.find("Transposition table cleared") != std::string::npos) {
      std::lock_guard<std::mutex> lk(g_stMu);
      g_hashCleared = true;
      g_stCv.notify_all();
    }
    return;
  }
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
      if (now - g_cur.lastTick >= kTickMs) {
        g_cur.lastTick = now;
        postTick = true;
        fromLink = g_cur.fromLink;
        id = g_cur.id;
        tickJson = ResJson(g_cur);
      }
    }
  }
  if (justDone) g_stCv.notify_all();   // worker 醒来后统一发 engineAck
  if (postTick) PostTick(fromLink, id, tickJson);
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

  // ★★ 2026-10-31（用户：「添加的『评估』功能导致置换表错乱，而且置换表没有及时清空，
  //   评估的棋点不准、收敛有些错乱」）：评估 / 常驻分析和对弈共用同一个引擎进程和同一张
  //   置换表。评估在自由摆盘上连续深搜，TT 里沉淀的是「摆盘局面」的条目；切回对弈或换一个
  //   评估局面时，这些先验会参与根节点收敛（尤其 2GB TT 里大量同分/浅层条目），表现为
  //   首选点飘、收敛错乱。这里在页面明确标记的边界上清一次：
  //     STOP（thinking 期间协议层会丢弃 YXHASHCLEAR）→ 等欠账清零 → YXHASHCLEAR → 等回执。
  //   清完 TT 后引擎内部 board 历史/序列也可能被回归搜索改过 ⇒ 强制整盘重发（见 BuildAskCmds）。
  if (req.clearHash) {
    WriteRaw("STOP\n");
    {
      std::unique_lock<std::mutex> lk(g_stMu);
      g_stCv.wait_for(lk, std::chrono::milliseconds(200), [] {
        return g_drain == 0 || !g_cur.alive;
      });
      if (g_drain > 0) { ELOG("hashclear 前 drain 未清零，强制清零"); g_drain = 0; }
      g_hashCleared = false;
    }
    WriteRaw("YXHASHCLEAR\n");
    {
      std::unique_lock<std::mutex> lk(g_stMu);
      if (!g_stCv.wait_for(lk, std::chrono::milliseconds(400), [] {
            return g_hashCleared || !g_cur.alive;
          }))
        ELOG("YXHASHCLEAR 无回执（继续，TT 可能未清）");
    }
    g_seqValid = false;
    g_seqBl.clear();
    g_seqWh.clear();
    g_seqNoIncr = true;
    ELOG("置换表已清空（评估/对弈边界）");
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
      auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget + 1500);
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

  // ★ 2026-10-31（用户：「评估」导致棋力下降）：live / 平衡 / 扫描防守这类非对弈搜索结束时，
  //   引擎在 sendActionAndUpdateBoard() 里已经把它的当前最优手落进**自己**的内部盘面，而
  //   这一轮我们只给 opName=="ask" 记账 g_seq*。若 g_seqValid 仍是 true，下一轮页面只多一手
  //   就会走增量 TURN，把那颗子叠到「评估局面 + 引擎自己那手」的错误盘面上 —— 评估用完后
  //   切回对弈，AI 实际算的是一个多了颗子的假盘面，表现就是棋点飘、收敛错乱。
  //   非对弈搜索一结束就作废序列，下一轮一律整盘重发（同时把 TT 清掉，见 clearHash）。
  if (req.live || req.mode != 0) g_seqValid = false;

  // ★ 2026-10-25：把引擎这一手补进 g_seq*。引擎在 gomocup.cpp 的
  //   sendActionAndUpdateBoard() 里已经把**自己选中的这一手**落到内部 board 上，
  //   所以下一轮对手落子后的盘面相对 g_seq* 恰好只多一手 ⇒ TURN 通道才真正成立
  //   （保留 board 历史 / lastMove / 置换表，同预算深度 +1；之前 g_seq* 停在「下发时」，
  //   下一轮通常多两手，增量判定恒不成立）。
  if (g_cur.done && !g_cur.superseded && g_cur.alive && g_cur.opName == "ask" && req.mode == 0 &&
      req.rule != 5 && req.rule != 6) {
    int bx = g_cur.ps.bestX, by = g_cur.ps.bestY;
    bool legal = bx >= 0 && by >= 0 && by < (int)req.board.size() && bx < (int)req.board[by].size() &&
                 req.board[by][bx] == 0;
    if (legal) {
      std::vector<std::pair<int, int>>& dst = (g_cur.side == 1) ? g_seqBl : g_seqWh;
      std::vector<std::pair<int, int>>& oth = (g_cur.side == 1) ? g_seqWh : g_seqBl;
      auto mv = std::make_pair(bx, by);
      if (std::find(dst.begin(), dst.end(), mv) == dst.end()) {
        if (std::find(oth.begin(), oth.end(), mv) != oth.end())
          g_seqValid = false;   // 颜色对不上（不该发生），下次整盘重发
        else
          dst.push_back(mv);    // 只当集合用，顺序无关（见 BuildAskCmds 的集合判定）
      }
    }
  }

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
       " cands=" + std::to_string(g_cur.ps.cands.size()) + " numPv=" + std::to_string(g_cur.ps.numPv) +
       " timectl=" + (req.live ? std::string("analysis")
                               : (req.dynTime ? ("match/" + std::to_string(g_lastMatchMs))
                                              : std::string("turn-only"))) +
       (g_cur.opName == "balance" ? (" pts=" + std::to_string(g_cur.ps.movePts.size())) : std::string()) +
       (g_cur.superseded ? " [superseded]" : ""));
}

// ============================================================ 8) 对外接口
static std::thread g_benchThread;

bool Init() {
  StartWorker();
  // ★ 2026-10-31：启动 tick 心跳线程（Init 幂等；Shutdown 会 join 干净）
  if (!g_tickRun.load()) {
    g_tickRun = true;
    if (g_tickThread.joinable()) g_tickThread.join();
    g_tickThread = std::thread(TickThreadMain);
  }
  LoadPrefs();
  bool ok = Probe();
  ELOG(std::string("Init: ") + (ok ? "ok" : "failed") + " variant=" +
       (g_variant.empty() ? "(none)" : g_variant));
  // ★ 2026-10-24（用户：「变体自动选择最快最准的」）：无用户指定且还没有实测缓存时，
  //   后台补一次短基准（约 10s，不阻塞启动/首手）；赢家写 .variant-bench，空闲则热切换。
  //   E2E 环境（GB_TEST_OPEN_LINK）跳过：基准会多拉引擎进程，撞「单实例峰值」断言。
  //
  // ★★ 2026-10-25 默认**关闭**（用户：「发给其他精通五子棋的用户，反应下降了，应该最近构建
  //   变体有了差异」）：事故根因就是这个基准 —— 1s 单局面的 TOTALNODES 受「早出杀 / 到达层数
  //   不同 / 热噪声」支配，曾把 avx2 判成最快并写进缓存接管实际运行（真机 6 局面 × 3 轮
  //   交错实测：avx512vnni 27.1 层 / 2972k nps，avx2 25.9 层 / 2554k nps，avx2 才是慢的）。
  //   同档位之间本来只差几个百分点，为这点噪声在启动时多烧 10s CPU、还冒着选错的风险，
  //   不划算。改为**显式开启**：设 GB_VARIANT_BENCH=1（诊断/新机器调优时用）。
  //   跨档保护（VariantTier/TopTier）保留，任何情况下缓存都不会低于本机最高档位。
  char tbEnv[8] = {0};
  bool e2eMode = GetEnvironmentVariableA("GB_TEST_OPEN_LINK", tbEnv, sizeof(tbEnv)) > 0;
  char bmEnv[8] = {0};
  bool benchOptIn = GetEnvironmentVariableA("GB_VARIANT_BENCH", bmEnv, sizeof(bmEnv)) > 0;
  if (ok && !e2eMode && benchOptIn && g_variantOverride.empty() && ReadBenchCache().empty()) {
    g_benchRun = true;
    std::thread([] { BenchThreadMain(); }).detach();
  }
  return ok;
}

bool Available() { return Probe(); }
const std::string& Diag() { return g_diag; }

void Shutdown() {
  g_benchRun = false;      // 停掉后台变体实测（若在跑；它的引擎子进程在 Job 里，随宿主清）
  // 先停 tick 心跳，再拆引擎状态，避免心跳线程在 g_cur 被重置后还去读它
  g_tickRun = false;
  if (g_tickThread.joinable()) g_tickThread.join();
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
  c.benchVariant = ReadBenchCache();     // ★ 本机实测最快（空 = 未测）；sysInfo 卡片与下拉说明用
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
    if (QHas(q, "dyn")) req.dynTime = QInt(q, "dyn") != 0;
    // ★ 2026-11-01：整局总时限（官方 INFO TIMEOUT_MATCH）。页面给 600000（10 分钟）；
    //   缺省也按 600000（= 用满每手时间，见 BuildAskCmds 的注释）。
    if (QHas(q, "matchMs")) req.matchMs = QI64(q, "matchMs");
    req.topN      = QInt(q, "topN");
    req.pondering = QBool(q, "pondering");
    req.live      = (op == "live") || QBool(q, "live");
    // ★ 2026-10-31：评估 / 对弈边界清置换表（见 AskRequest::clearHash 与 DoAsk）。
    req.clearHash = QBool(q, "clearHash");
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
         (req.clearHash ? " clearHash=1" : "") +
         (req.mode ? (" balMode=" + std::to_string(req.mode) + " bias=" + std::to_string(req.bias)) : std::string()) +
         " key=" + std::to_string((long long)req.key));
    // ★★ 2026-11-01 21:2x（用户：「直接恢复 4.46 的引擎逻辑」）：
    //   回到 4.46 的原始语义 —— **新 ask 立即抢占在途搜索，并清空整个队列**。
    //   不引入任何优先级 / 排队 / 互斥策略：一次只跑一个请求，旧请求作废即作废。
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
      g_queue.clear();                     // 页面只关心最新盘面，旧的排队项一律作废
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
    std::string w = QStr(q, "weights");
    if (w != "nnue" && w != "classic") w = "nnue";
    bool wChanged = (w != g_weightsMode);
    g_weightsMode = w;
    // ★ 2026-10-24：选点范围 / 最大深度 / 最大节点（0 或缺失 = 用引擎 config 默认）
    int cr = QInt(q, "caution");
    int md = QInt(q, "maxDepth");
    long long mn = (long long)QI64(q, "maxNodes");
    // ★ 2026-10-25：style（CAUTION_FACTOR）**不需要重启进程** —— 我们的引擎源码
    //   （desktop-connect/rapfi/command/gomocup.cpp:293）收到 CAUTION_FACTOR 且值变化时，
    //   会自己保存历史 → 用新 range 重建 Board → 重放 → 清 TT，即时生效。
    //   （此前误读了 .workbuddy/tmp 里较旧的上游副本，以为它只在建盘时生效，已纠正。）
    //   cChanged 只用于打日志，不参与重启判断。
    g_candRange = (cr >= 0 && cr <= 5) ? cr : -1;
    g_maxDepth  = (md >= 2) ? md : -1;
    g_maxNodes  = (mn >= 1) ? mn : -1;
    // ★ 2026-10-24：引擎变体（"" = 自动）。变了就重启引擎，下次 EnsureAlive 会重新 Probe。
    std::string var = QStr(q, "variant");
    bool vChanged = (var != g_variantOverride);
    if (vChanged) {
      g_variantOverride = var;
      g_probed = false;
      g_avail  = false;
      g_variant.clear();
      DeleteFileW(Join(g_dir, L".variant-ok").c_str());
      ELOG("引擎变体切换 -> " + (var.empty() ? "自动" : var) + "（下次 ask 重新探测）");
    }
    SetUserPrefs(t, h);
    if ((wChanged || vChanged) && g_e.alive) {
      // 权重档 / 变体变了：停掉旧进程，下一次 ask 会按新配置重新拉起
      //（SpawnLocked 里发 RELOADCONFIG / CAUTION_FACTOR）。
      std::lock_guard<std::mutex> lk(g_e.spawnMu);
      if (g_e.hProcess) { TerminateProcess(g_e.hProcess, 0); CloseHandle(g_e.hProcess); g_e.hProcess = nullptr; }
      { std::lock_guard<std::mutex> lw(g_writeMu); if (g_e.hStdin) { CloseHandle(g_e.hStdin); g_e.hStdin = nullptr; } }
      g_e.alive = false;
      { std::lock_guard<std::mutex> ls(g_stMu);
        g_cur.active = false; g_cur.alive = false; g_cur.done = true; g_drain = 0; }
      g_stCv.notify_all();
      ELOG("引擎权重切换 -> " + g_weightsMode + "（进程重启，下次 ask 生效）");
    }
    Caps c = GetCaps();
    PostTo(fromLink, "{\"type\":\"engineAck\",\"id\":" + std::to_string(id) +
                     ",\"op\":\"config\",\"ok\":true,\"res\":{\"threads\":" + std::to_string(c.threads) +
                     ",\"hashMB\":" + std::to_string(c.hashMB) +
                     ",\"weights\":" + JsonQuote(g_weightsMode) + "}}");
    return;
  }
  ELOG("未知 op: " + op);
}

}  // namespace eng
