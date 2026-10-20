//
//

#ifndef GB_ENGINE_HOST_H
#define GB_ENGINE_HOST_H

#include <string>
#include <vector>
#include <cstdint>

#ifndef _WINDEF_
typedef void* HANDLE;
#endif

namespace eng {

namespace host {
HANDLE       Job();
void         PostPage(const std::string& json);
void         PostLink(const std::string& json);
void         Log(const std::string& s);
std::wstring EngineDir();                         // <ExeDir>/rapfi-native
void         ChildTracked(HANDLE hProcess);
bool         ApplyAffinity(HANDLE hProcess);
void         PinHostAway();
void         UnpinHost();
int          PhysCores();
int          LogicCores();
int          ThreadsMaxHost();
long long    TotalPhysKB();
}  // namespace host

struct Candidate {
  int         x = -1, y = -1;
  std::string eval;
  int         winrateBp = -1;
  int         depth = 0, selDepth = 0;
  long long   nodes = 0;
  std::vector<std::pair<int, int>> pv;
};

struct AskRequest {
  std::vector<std::vector<int>> board;
  int      side = 0;
  int      rule = 0;
  int      budgetMs = 2000;
  int      topN = 1;
  bool     pondering = false;
  uint64_t key = 0;
  int      mode = 0;
  int      bias = 0;
  std::string opName = "ask";
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
  std::vector<std::pair<int, int>> pts;
  std::vector<std::pair<int, int>> forbid;
};

struct Caps {
  bool        available = false;
  std::string variant;                  // sse / avx2 / avxvnni / avx512 / avx512vnni
  std::string engineSource;
  bool        hasSSE = true, hasAVX2 = false, hasAVX512 = false;
  bool        hasAVX512VNNI = false, hasAVXVNNI = false;
  bool        hasVNNI = false;   // = hasAVX512VNNI || hasAVXVNNI
  int         physCores = 0, logicCores = 0;
  int         ramMB = 0;
  int         threads = 0, hashMB = 0;
  int         userThreads = 0, userHashMB = 0;
  std::vector<int> threadOptions;
  std::string diag;
};

bool Init();
void Shutdown();
bool Available();
const std::string& Diag();

void HandleOp(const std::string& op, int id, const std::string& qJson, bool fromLink);

Caps GetCaps();
void SetUserPrefs(int threads, int hashMB);
void LoadPrefs();

struct ParseState {
  bool        inPv = false;
  int         pvIdx = -1;
  Candidate   cur;
  std::vector<Candidate> cands;
  int         numPv = 0;
  int         maxDepth = 0, selDepth = 0, timeMs = 0;
  long long   totalNodes = 0, speed = 0;
  int         lastEvalSide = 0;
  bool        finished = false;
  bool        swap = false;
  int         bestX = -1, bestY = -1;
  std::vector<std::pair<int, int>> movePts;
  std::vector<std::pair<int, int>> forbid;
};
void ParseLine(const std::string& line, ParseState& st);

}  // namespace eng

#endif  // GB_ENGINE_HOST_H
