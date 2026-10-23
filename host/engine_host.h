// Meter Gomoku Link — v4.43
// engine_host.h —— Rapfi "极速版" 引擎调用层（宿主侧唯一权威）
//
// 设计要点（见 .trae/documents/rapfi-引擎调用层重建方案.md）：
//   · 单实例全核：**1 个** Rapfi 进程 + 1 份大哈希，串行服务所有请求，STOP 抢占。
//   · 协议解析/编排 100% 在本模块：页面只发一个 `ask` 拿结构化结果，宿主是唯一解析器。
//   · 只信 `INFO` 协议块（INFO PV <n> ... INFO PV DONE）；`MESSAGE` 人类文本一律不解析，
//     天然免疫 `[Pondering]` 行的对手视角评估。
//   · 坐标零转换：config.toml 的 coord_conversion_mode="X_flipY" 已由引擎内部完成
//     页面坐标(y 向下) ↔ 引擎内部坐标(y 向上) 的往返，宿主直接透传即可。
//   · 裸着法（如 `7,7`）出现即视为该次 ask 的终局回执。
//
// host.cpp 只需：① 实现下方 namespace host{} 的少量回调；② 把页面的 4 个 op 原样转给
// HandleOp()。其余（进程、解析、资源、推送）全在本模块内。

#ifndef GB_ENGINE_HOST_H
#define GB_ENGINE_HOST_H

#include <string>
#include <vector>
#include <cstdint>

// Windows 句柄类型（避免在本头文件里拖 windows.h）
#ifndef _WINDEF_
typedef void* HANDLE;
#endif

namespace eng {

// ---------------------------------------------------------------- 宿主回调面
// 由 host.cpp 实现：本模块与宿主之间的**唯一**反向依赖（保持面小、易审计）。
namespace host {
HANDLE       Job();            // 共享 Job 对象（KILL_ON_JOB_CLOSE，防孤儿进程）
void         PostPage(const std::string& json);   // 主页面出箱（PostToPageSoon）
void         PostLink(const std::string& json);   // 连线页推送（PostToLinkPageSoon）
void         Log(const std::string& s);           // GomokuTrainer.log（LogMsg）
std::wstring EngineDir();                         // <ExeDir>/rapfi-native
void         ChildTracked(HANDLE hProcess);       // 登记子进程 PID（随宿主退出而清理）
bool         ApplyAffinity(HANDLE hProcess);      // 引擎核组亲和（CpuSplit）
void         PinHostAway();                       // 宿主让出引擎核
void         UnpinHost();                         // 解除让核
int          PhysCores();                         // 物理核数
int          LogicCores();                        // 逻辑处理器数
int          ThreadsMaxHost();                    // 宿主口径上限（逻辑核 - 视觉预留）
long long    TotalPhysKB();                       // 物理内存 KB
}  // namespace host

// ---------------------------------------------------------------- 数据契约
struct Candidate {
  int         x = -1, y = -1;
  std::string eval;            // 引擎原文（相对走子方）："+M1" / "-M3" / "12"
  int         winrateBp = -1;  // 万分比（0..10000），-1 = 未知
  int         depth = 0, selDepth = 0;
  long long   nodes = 0;
  std::vector<std::pair<int, int>> pv;   // 页面坐标
};

struct AskRequest {
  std::vector<std::vector<int>> board;   // 15x15，0 空 / 1 黑 / 2 白（页面坐标）
  int      side = 0;                     // 引擎走子方（1=黑, 2=白）——评估视角锚点
  int      rule = 0;                     // 0 无禁手 / 1 标准 / 2 连珠 / 5 swap1
  int      budgetMs = 2000;
  int      topN = 1;
  bool     pondering = false;
  // ★ 2026-10-22：常驻分析（live）。budgetMs 不生效，改用超长 TIMEOUT_TURN 让引擎**一次连续
  //   加深**，不再按轮次 STOP→YXBOARD 重开；只有用户落子 / 停止分析 / 新 ask 抢占时才 STOP。
  bool     live = false;
  uint64_t key = 0;                      // 盘面指纹（回执原样带回，供页面判过期）
  // —— 平衡点（官方 YXBALANCEONE/TWO，一次性算完即停）——
  int      mode = 0;                     // 0 = 普通 ask(YXNBEST) / 1 = BALANCE_ONE / 2 = BALANCE_TWO
                                         // 3 = 扫描防守（YXSEARCHDEFEND，全点 multipv）
  int      bias = 0;                     // 传给 YXBALANCE* 的 balanceBias（TWO 模式由引擎内部取反）
  std::string opName = "ask";            // 回执 op 名（"ask" / "balance"）
};

struct AskResult {
  bool        ok = false;
  bool        superseded = false;
  bool        swap = false;
  bool        finished = false;
  std::string error;
  int         bestX = -1, bestY = -1;
  std::string bestEval;
  int         depth = 0, selDepth = 0, timeMs = 0;
  long long   nodes = 0, totalNodes = 0, speed = 0;
  uint64_t    key = 0;
  std::vector<Candidate> candidates;
  std::vector<std::pair<int, int>> pts;   // 平衡点：BALANCE_ONE=1 个 / BALANCE_TWO=2 个
  std::vector<std::pair<int, int>> forbid;
};

struct Caps {
  bool        available = false;
  std::string variant;                  // sse / avx2 / avxvnni / avx512 / avx512vnni
  std::string engineSource;             // built | prebuilt（读 engines.json）
  bool        hasSSE = true, hasAVX2 = false, hasAVX512 = false;
  bool        hasAVX512VNNI = false, hasAVXVNNI = false;
  bool        hasVNNI = false;   // = hasAVX512VNNI || hasAVXVNNI
  int         physCores = 0, logicCores = 0;
  int         ramMB = 0;
  int         threads = 0, hashMB = 0;  // 生效值
  int         userThreads = 0, userHashMB = 0;   // 0 = 自动
  std::vector<int> threadOptions;
  std::string diag;
};

// ---------------------------------------------------------------- 生命周期
bool Init();                  // 探测变体（含 .variant-ok 缓存与冒烟降档）；幂等
void Shutdown();              // STOP → 等待 → Terminate → Close + 释放 Job 借用
bool Available();             // 是否已有可用引擎（非阻塞，触发一次探测）
const std::string& Diag();    // 最近一次不可用的原因（供页面提示）

// ---------------------------------------------------------------- IPC（4 op）
// 页面 → 宿主：{"engine":1,"op":"ask|stop|caps|config","id":N,"q":{...}}
// 全部推送由本模块完成（ack 走 host::PostPage/PostLink，tick 走同一通道）。
void HandleOp(const std::string& op, int id, const std::string& qJson, bool fromLink);

// ---------------------------------------------------------------- 配置 / 偏好
Caps GetCaps();
void SetUserPrefs(int threads, int hashMB);   // 0 = 自动；立即落盘 + 生效
void LoadPrefs();                             // 启动时读回（含 clamp 回写）

// ---------------------------------------------------------------- 单测入口（无进程）
struct ParseState {
  bool        inPv = false;
  int         pvIdx = -1;
  Candidate   cur;
  std::vector<Candidate> cands;   // 按 pvIdx 存，索引即 PV 序号
  int         numPv = 0;
  int         maxDepth = 0, selDepth = 0, timeMs = 0;
  long long   totalNodes = 0, speed = 0;
  int         lastEvalSide = 0;   // 最近一条 INFO EVAL 所属的 PV
  bool        finished = false;   // 出现裸着法 / SWAP
  bool        swap = false;
  int         bestX = -1, bestY = -1;
  std::vector<std::pair<int, int>> movePts;   // 终局裸着法行的全部坐标（BALANCE_TWO 会有两个）
  std::vector<std::pair<int, int>> forbid;
};
void ParseLine(const std::string& line, ParseState& st);

}  // namespace eng

#endif  // GB_ENGINE_HOST_H
