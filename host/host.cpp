/* Meter Gomoku Trainer - desktop host (Win32 / WebView2). Owns board scanning, recognition, click injection, the native engine process and overlay windows. */


#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX              
#endif
#include <winsock2.h>     
#include <ws2tcpip.h>
#include <windows.h>
#include <objidl.h>       
#include <gdiplus.h>      
#include <gdiplusflat.h>  
#include <commdlg.h>      
#include <shellapi.h>
#include <shlwapi.h>
#include <objbase.h>
#include <dwmapi.h>       
#include <mmsystem.h>     
                          
#include <tlhelp32.h>     
#include "WebView2.h"
#include "ui_crypto.h"          
#include "appver.h"             
#include "engine_host.h"

#include <algorithm>   
#include <atomic>
#include <cctype>      
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <intrin.h>   
#include <immintrin.h> 
#include <mutex>      
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")  

static HINSTANCE g_hInst = nullptr;
static HWND      g_hwnd = nullptr;
static HWND      g_view = nullptr;      
static ICoreWebView2* g_webview = nullptr;
static ICoreWebView2Controller* g_controller = nullptr;
static ICoreWebView2Environment* g_env = nullptr;
static std::atomic<bool> g_running{true};

static HANDLE g_job = nullptr;

static std::mutex g_childMtx;
static std::vector<DWORD> g_children;      

static void ChildTrack(HANDLE hProcess) {
  if (!hProcess) return;
  DWORD pid = GetProcessId(hProcess);
  if (!pid) return;
  std::lock_guard<std::mutex> lk(g_childMtx);
  g_children.push_back(pid);
}

static bool IsOurChildName(const wchar_t* n) {
  if (!n) return false;
  if (_wcsnicmp(n, L"RapfiEngine-", 12) == 0) return true;      
  if (_wcsicmp(n, L"GomokuVision.exe") == 0) return true;       
  if (_wcsicmp(n, L"msedgewebview2.exe") == 0) return true;     
  return false;
}

static std::string g_pending;           

static HWND      g_rvHwnd = nullptr;
static ICoreWebView2* g_rvWebview = nullptr;
static ICoreWebView2Controller* g_rvController = nullptr;
static std::string g_rvPending;         
static bool        g_rvCreating = false;

static void OpenReviewWindow(const std::string& recordJson);
static void ScheduleRvPrewarm(UINT ms);      
static void PrewarmReviewWindow();           
static void PostToRvPage(const std::string& json);
static void PostToPage(const std::string& json);

static void OpenVisWindow();
static void OpenLinkWindow();        
static void LinkAutoDropAt(int cx, int cy, bool force, bool real);   
static void PostToVisPage(const std::string& json);
static void PostToVisPageSoon(const std::string& json);   
static void StartVisShotFlow();                           
static void RunVisionForVis(std::string req);             

static bool ApplyEngineAffinity(HANDLE h);                
static bool ApplyVisionAffinity(HANDLE h);
static void BoostVisionPriority(HANDLE h);
static void BoostExistingVisionPriority();                
static void CpuSplitPinHost();                            
static void CpuSplitUnpinHost();                          
static std::string CpuSplitStatusJson();                   

static void FireTestHooks();

static void OpenUpdateWindow();
static void OpenSetWindow();

// settings window globals (defined once here; used by MsgHandler mainSync relay too)
static HWND      g_setHwnd = nullptr;
static ICoreWebView2* g_setWebview = nullptr;
static ICoreWebView2Controller* g_setController = nullptr;
static bool g_setCreating = false, g_setPageReady = false;

static const UINT_PTR kRvTimerTestOpen = 3;  

static HWND      g_visHwnd = nullptr;
static ICoreWebView2* g_visWebview = nullptr;
static ICoreWebView2Controller* g_visController = nullptr;
static std::string g_visPending;         
static bool        g_visCreating = false;
static bool        g_visShown = false;
static bool        g_visPageReady = false;
static std::vector<std::string> g_outboxVis;   
static std::vector<std::string> g_outboxLink;  

static std::mutex g_outboxMtx;
static const size_t GB_OUTBOX_CAP = 512;       

static HWND      g_linkHwnd = nullptr;

static std::wstring ExeDir() {
  wchar_t buf[MAX_PATH] = {0};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring s(buf);
  size_t p = s.find_last_of(L"\\/");
  return (p == std::wstring::npos) ? L"." : s.substr(0, p);
}
static std::wstring Join(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) return b;
  if (a.back() == L'\\' || a.back() == L'/') return a + b;
  return a + L"\\" + b;
}
static bool FileExists(const std::wstring& p) {
  return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

#ifdef GB_SUITE_ENGINE
static void LogMsg(const std::string& s);        

static void SpawnDetached(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd) {
  std::wstring cmd = L"\"" + exe + L"\"";
  if (!args.empty()) cmd += L" " + args;
  STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
  PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
  std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
  
  if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                     cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
    
    ChildTrack(pi.hProcess);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    LogMsg("[deps] spawned engine backend (as --as-backend)");
  } else {
    char b[96]; snprintf(b, sizeof(b), "[deps] spawn failed err=%lu", (unsigned long)GetLastError());
    LogMsg(b);
  }
}

static std::atomic<unsigned long long> g_engineSpawnedAt(0);
static void EnsureEngine() {
  
  std::wstring root = ExeDir();                 
  std::wstring exe = Join(root, L"Web GomokuEngine.exe");
  if (!FileExists(exe)) {
    exe = Join(root, L"GomokuEngine.exe");
    if (!FileExists(exe)) return;               
  }
  
  unsigned long long now = GetTickCount64(), cur = g_engineSpawnedAt.load();
  for (;;) {
    if (now - cur < 25000) return;
    if (g_engineSpawnedAt.compare_exchange_weak(cur, now)) break;
  }
  LogMsg("[deps] starting engine backend --as-backend (headless, no browser)");
  SpawnDetached(exe, L"--as-backend", root);
}
static void EngineWatchdog() {
  EnsureEngine();
  while (g_running.load()) {
    Sleep(15000);
    if (!g_running.load()) return;
    EnsureEngine();
  }
}
#endif  

static std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}
static std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}

static void LogMsg(const std::string& s);        
static DWORD g_bootT0 = 0;
static void LogBoot(const char* what) {
  char b[160];
  snprintf(b, sizeof(b), "[perf] %-24s %5lu ms", what,
           (unsigned long)(GetTickCount() - g_bootT0));
  LogMsg(b);
}

static void KillOurChildProcesses(const char* why) {
  DWORD me = GetCurrentProcessId();
  std::vector<DWORD> pids;
  {
    std::lock_guard<std::mutex> lk(g_childMtx);
    pids = g_children;
  }
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
      do {
        if (pe.th32ParentProcessID != me || pe.th32ProcessID == me) continue;
        if (IsOurChildName(pe.szExeFile)) pids.push_back(pe.th32ProcessID);
      } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
  }
  std::sort(pids.begin(), pids.end());
  pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
  int killed = 0, gone = 0;
  for (size_t i = 0; i < pids.size(); i++) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                           FALSE, pids[i]);
    if (!h) { gone++; continue; }                       
    if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) { gone++; CloseHandle(h); continue; }
    if (TerminateProcess(h, 0)) killed++; else gone++;
    CloseHandle(h);
  }
  LogMsg(std::string("[exit] 子进程收尾（") + why + "）：终止 " + std::to_string(killed) +
         " 个，已退出/不可及 " + std::to_string(gone) + " 个（共记账 " +
         std::to_string((int)pids.size()) + "）");
}

static COLORREF MixColor(COLORREF a, COLORREF b, float t) {
  int ar = (int)(a & 0xFF), ag = (int)((a >> 8) & 0xFF), ab = (int)((a >> 16) & 0xFF);
  int br = (int)(b & 0xFF), bg = (int)((b >> 8) & 0xFF), bb = (int)((b >> 16) & 0xFF);
  auto cl = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
  return RGB(cl((int)(ar + (br - ar) * t)), cl((int)(ag + (bg - ag) * t)), cl((int)(ab + (bb - ab) * t)));
}

// ★ 2026-10-25（E2E 偶发误判「引擎回执=无」）：日志是**多线程并发写**的 ——
//   页面 console 消息（UI 线程）与引擎读线程的 ELOG 会同时进来。原来每次 _wfopen("ab")
//   再分 3 次 fwrite（时间戳 / 正文 / 换行），两个线程交错就会把一行切成
//   「[12:46:08.073] [bridge-ui] …深度 19」+「39 2422ms nodes=…」这种残缺行，
//   于是 grep '[eng] ack id=' 抓不到 ⇒ 明明通了却判失败。
//   修法：整条日志先拼成一行，再在一把锁里「打开→一次写完→关闭」。
static std::mutex g_logMu;

static void LogMsg(const std::string& s) {
  wchar_t tmp[MAX_PATH] = {0};
  GetTempPathW(MAX_PATH, tmp);
  std::wstring path = Join(std::wstring(tmp), L"GomokuTrainer.log");
  SYSTEMTIME st; GetLocalTime(&st);
  char head[64];
  snprintf(head, sizeof(head), "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

  std::string line;
  line.reserve(strlen(head) + s.size() + 2);
  line.append(head, strlen(head));
  line += s;
  line += '\n';

  std::lock_guard<std::mutex> lk(g_logMu);
  static DWORD lastLogRotate = 0;
  DWORD nowRot = GetTickCount();
  if (nowRot - lastLogRotate > 30000) {
    lastLogRotate = nowRot;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
        ((((unsigned long long)fad.nFileSizeHigh) << 32) | fad.nFileSizeLow) > (8ull << 20)) {
      DeleteFileW(path.c_str());
    }
  }
  FILE* f = _wfopen(path.c_str(), L"ab");
  if (!f) return;
  fwrite(line.data(), 1, line.size(), f);
  fclose(f);
}

static void PostToPage(const std::string& json) {
  if (!g_webview) { g_pending = json; return; }
  std::wstring w = Utf8ToWide(json);
  g_webview->PostWebMessageAsJson(w.c_str());
}

static const UINT_PTR kMainTimerOutbox = 6;
static const UINT_PTR kMainTimerHook = 7;
static const UINT_PTR kMainTimerPrewarmSet = 15;   // pre-create the settings window at boot
static const UINT_PTR kMainTimerTestSetMin = 16;   // GB_TEST_SET_MINIMIZE: minimize the settings window

static const UINT kMsgFlushOutbox = WM_APP + 2;

static const UINT kMsgVisShot = WM_APP + 7;

static const UINT kMsgLinkSelect = WM_APP + 11;
static const UINT kMsgLinkFrameOpen = WM_APP + 21;   
static const UINT kMsgLinkFramePaint = WM_APP + 12;   

static void LinkParseBest(const std::string& s);
static void ParseBalPairs(const std::string& s, const char* key, std::vector<std::pair<int,int>>& out);

static void PostToLinkPageSoon(const std::string& json);
static void PostToLinkPage(const std::string& json);


static std::atomic<bool> g_linkSession(false);

// ★ 2026-10-18（用户：「连线功能的识别更换为 synchronization 目录下的同步算法」+「真实点击」
//   改为「识别参数」并弹出小浮层）：连线同步识别参数。
//   宿主写盘为单一权威（linkrecog.json，见 SaveLinkRecogPrefs/LoadLinkRecogPrefs），启动读回；
//   每次 LinkScanTick 把 5 项阈值注入请求 body（"algo":"sync","recog":{...}）；
//   syncInterval 接管非锁定扫描周期。
// ★★ 2026-10-18 最终口径（用户：「找棋盘的逻辑用我们昨天的，识别棋子的逻辑用今天新的，
//   识别参数恢复那 6 个参数」）：**定位自制 + 读子同步** 的混合口径 —— 6 项参数全部生效
//   （5 项给 sync 的逐格判定，interval 给宿主扫描周期）。
static std::atomic<int> g_syncInterval(200);   // 同步时间间隔(ms)：非锁定扫描周期
static std::atomic<int> g_syncGray(50);        // 灰度偏差(0-255)
static std::atomic<int> g_syncBlackOff(96);    // 黑色偏差(0-255)
static std::atomic<int> g_syncWhiteOff(96);    // 白色偏差(0-255)
static std::atomic<int> g_syncBlackPct(33);    // 黑色占比(0-100)
static std::atomic<int> g_syncWhitePct(33);    // 白色占比(0-100)

static std::vector<std::string> g_outboxPage;
static std::vector<std::string> g_outboxRv;

static void OutboxPush(std::vector<std::string>& q, const std::string& json) {
  std::lock_guard<std::mutex> lk(g_outboxMtx);
  if (q.size() >= GB_OUTBOX_CAP) {
    q.erase(q.begin());
    
    static DWORD lastCapLog = 0;
    DWORD nowCap = GetTickCount();
    if (nowCap - lastCapLog > 10000) {
      lastCapLog = nowCap;
      LogMsg("[outbox] overflow -> dropped oldest message (page not draining)");
    }
  }
  q.push_back(json);
}

static void OutboxDrain(std::vector<std::string>& q, std::vector<std::string>& out) {
  std::lock_guard<std::mutex> lk(g_outboxMtx);
  out.swap(q);
}

static void PostToPageSoon(const std::string& json) {
  OutboxPush(g_outboxPage, json);
  if (g_hwnd && IsWindow(g_hwnd)) SetTimer(g_hwnd, kMainTimerOutbox, 1, nullptr);
}
static void PostToRvPageSoon(const std::string& json) {
  OutboxPush(g_outboxRv, json);
  if (g_hwnd && IsWindow(g_hwnd)) SetTimer(g_hwnd, kMainTimerOutbox, 1, nullptr);
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

static std::vector<std::string> JsonStrArray(const std::string& s, const char* key) {
  std::vector<std::string> out;
  size_t p = s.find(key);
  if (p == std::string::npos) return out;
  p = s.find('[', p);
  if (p == std::string::npos) return out;
  for (++p; p < s.size() && s[p] != ']';) {
    if (s[p] != '"') { ++p; continue; }
    std::string cur;
    for (++p; p < s.size() && s[p] != '"'; ++p) {
      if (s[p] == '\\' && p + 1 < s.size()) {
        char c = s[++p];
        if (c == 'n') cur += '\n';
        else if (c == 't') cur += '\t';
        else if (c == 'r') cur += '\r';
        else if (c == 'u') p += 4;              
        else cur += c;
      } else cur += s[p];
    }
    if (p < s.size()) ++p;
    out.push_back(cur);
  }
  return out;
}

static std::string JsonStrAfter(const std::string& s, const char* key) {
  size_t p = s.find(key);
  if (p == std::string::npos) return "";
  p += strlen(key);
  if (p >= s.size() || s[p] != '"') return "";
  std::string o;
  for (++p; p < s.size() && s[p] != '"'; ++p) {
    if (s[p] == '\\' && p + 1 < s.size()) ++p;
    o += s[p];
  }
  return o;
}

static void AppendUtf8ForCodePoint(std::string& o, unsigned cp) {
  if (cp < 0x80) {
    o += (char)cp;
  } else if (cp < 0x800) {
    o += (char)(0xC0 | (cp >> 6));
    o += (char)(0x80 | (cp & 0x3F));
  } else {
    o += (char)(0xE0 | (cp >> 12));
    o += (char)(0x80 | ((cp >> 6) & 0x3F));
    o += (char)(0x80 | (cp & 0x3F));
  }
}

static std::string JsonStrAfterDecoded(const std::string& s, const char* key) {
  size_t p = s.find(key);
  if (p == std::string::npos) return "";
  p += strlen(key);
  if (p >= s.size() || s[p] != '"') return "";
  std::string o;
  for (++p; p < s.size() && s[p] != '"'; ++p) {
    if (s[p] != '\\') { o += s[p]; continue; }
    if (++p >= s.size()) break;
    switch (s[p]) {
      case 'n':  o += '\n'; break;
      case 't':  o += '\t'; break;
      case 'r':  o += '\r'; break;
      case 'b':  o += '\b'; break;
      case 'f':  o += '\f'; break;
      case '/':  o += '/';  break;
      case '"':  o += '"';  break;
      case '\\': o += '\\'; break;
      case 'u': {
        unsigned cp = 0;
        for (int k = 0; k < 4 && p + 1 < s.size(); k++) {
          char c = s[++p];
          cp <<= 4;
          if (c >= '0' && c <= '9') cp |= (unsigned)(c - '0');
          else if (c >= 'a' && c <= 'f') cp |= (unsigned)(c - 'a' + 10);
          else if (c >= 'A' && c <= 'F') cp |= (unsigned)(c - 'A' + 10);
        }
        AppendUtf8ForCodePoint(o, cp);
        break;
      }
      default: o += s[p]; break;
    }
  }
  return o;
}

static int JsonIntAfter(const std::string& s, const char* key) {
  size_t p = s.find(key);
  if (p == std::string::npos) return -1;
  p += strlen(key);
  while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
  return atoi(s.c_str() + p);
}

static bool g_uiDark = true;
static COLORREF g_uiBg = RGB(0x26, 0x27, 0x2a);   
static COLORREF g_uiFg = RGB(0x9a, 0xa0, 0xa6);   
static std::wstring g_uiText = L"加载中…";
static bool g_mainPageShown = false;              
static const UINT_PTR kMainTimerReload = 5;
static const UINT_PTR kMainTimerEval = 8;
static const UINT_PTR kMainTimerTestLink = 10;
static const UINT_PTR kMainTimerTestLinkScan = 11;
static const UINT_PTR kMainTimerTestLinkCheck = 12;         
// ★ 2026-10-16：回归探针（GB_TEST_LINK_PROBE=1）——① 空盘帧（应清置换表）
//   ② 主窗口 → 连线窗的规则同步（应清置换表并按新规则重算）。
static const UINT_PTR kMainTimerTestLinkProbe = 13;
static const UINT_PTR kMainTimerTestLinkProbe2 = 14;

static const UINT_PTR kMainTimerSysMem = 9;
static const UINT kSysMemIntervalMs = 3000;
static int g_mainReloads = 0;                    
static int g_mainCtrlRebuilds = 0;               
static std::wstring UiFile();

static std::string JKey(const char* name) {
  return std::string(1, '"') + name + std::string(1, '"') + std::string(1, ':');
}
static void LoadUiPrefs();
static void SaveUiPrefs(const std::string& s);
static void LoadLinkRecogPrefs();
static std::string LinkRecogJson();
static void ClampLinkRecog(int& interval, int& gray, int& blackOff, int& whiteOff,
                           int& blackPct, int& whitePct);
static void ApplyDarkTitleBar(HWND h, bool dark);
static void SetWebviewBg(ICoreWebView2Controller* ctl, COLORREF c);
static void RevealMainPage();
static void RevealRvPage();
static void PaintSplash(HWND h, bool pageShown, const wchar_t* brand);

static std::string JsonSubObject(const std::string& s, const char* key) {
  size_t p = s.find(key);
  if (p == std::string::npos) return "";
  p = s.find('{', p + strlen(key));
  if (p == std::string::npos) return "";
  int depth = 0;
  bool inStr = false, esc = false;
  for (size_t i = p; i < s.size(); ++i) {
    char c = s[i];
    if (inStr) {
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') inStr = false;
      continue;
    }
    if (c == '"') inStr = true;
    else if (c == '{') depth++;
    else if (c == '}') { if (--depth == 0) return s.substr(p, i - p + 1); }
  }
  return "";
}

static std::vector<unsigned char> Base64Decode(const std::string& in) {
  static int8_t tbl[256];
  static bool built = false;
  if (!built) {
    for (int i = 0; i < 256; i++) tbl[i] = -1;
    const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) tbl[(unsigned char)A[i]] = (int8_t)i;
    built = true;
  }
  std::vector<unsigned char> out;
  int val = 0, bits = 0;
  for (size_t i = 0; i < in.size(); i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '=') break;
    int8_t d = tbl[c];
    if (d < 0) continue;                       
    val = (val << 6) | d;
    bits += 6;
    if (bits >= 8) { bits -= 8; out.push_back((unsigned char)((val >> bits) & 0xFF)); }
  }
  return out;
}

static std::wstring PickSavePathAs(const std::wstring& suggested, HWND owner,
                                   const wchar_t* filter, const wchar_t* defExt,
                                   const wchar_t* title) {
  wchar_t buf[MAX_PATH] = {0};
  wcsncpy(buf, suggested.c_str(), MAX_PATH - 1);
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner ? owner : g_hwnd;
  ofn.lpstrFilter = filter;
  ofn.lpstrFile = buf;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrDefExt = defExt;
  ofn.lpstrTitle = title;
  
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
  if (!GetSaveFileNameW(&ofn)) return L"";
  return std::wstring(buf);
}
static std::wstring PickSavePath(const std::wstring& suggested, HWND owner) {
  return PickSavePathAs(suggested, owner,
                        L"PNG 图片 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0", L"png", L"保存局面图片");
}

static std::wstring PickOpenTxtPath(HWND owner) {
  wchar_t buf[MAX_PATH] = {0};
  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner ? owner : g_hwnd;
  ofn.lpstrFilter = L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrDefExt = L"txt";
  ofn.lpstrTitle = L"导入历史（txt 局面代码）";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&ofn)) return L"";
  return std::wstring(buf);
}

static bool TestFlag(const char* name) {
  char v[8] = {0};
  return GetEnvironmentVariableA(name, v, sizeof(v)) && v[0] == '1';
}

static void SavePngFromPage(const std::string& s, HWND owner) {
  std::string name = JsonStrAfter(s, "\"name\":");
  std::string data = JsonStrAfter(s, "\"data\":");
  if (name.empty()) name = "gomoku.png";
  size_t p = data.find("base64,");
  std::vector<unsigned char> png = (p == std::string::npos)
      ? std::vector<unsigned char>() : Base64Decode(data.substr(p + 7));
  if (png.empty()) { LogMsg("[save] png payload is empty - nothing written"); return; }

  std::wstring path;
  char tv[1024] = {0};
  DWORD tn = GetEnvironmentVariableA("GB_TEST_SAVE_PNG", tv, sizeof(tv));
  if (tn > 0 && tn < sizeof(tv)) {
    path = Utf8ToWide(tv);                     
  } else {
    path = PickSavePath(Utf8ToWide(name), owner);
  }
  if (path.empty()) { LogMsg("[save] cancelled by user"); return; }

  FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) {
    LogMsg("[save] cannot open for write: " + WideToUtf8(path));
    return;
  }
  fwrite(png.data(), 1, png.size(), f);
  fclose(f);
  LogMsg("[save] png written: " + WideToUtf8(path) + " (" + std::to_string(png.size()) + " bytes)");
}

struct TxtSaveResult { bool ok; int n; std::wstring path; };

static TxtSaveResult SaveTxtFromPage(const std::string& s, HWND owner) {
  TxtSaveResult r = { false, 0, L"" };
  
  std::string name = JsonStrAfterDecoded(s, "\"name\":");
  std::string data = JsonStrAfterDecoded(s, "\"data\":");
  int n = JsonIntAfter(s, "\"n\":");
  r.n = (n < 0) ? 0 : n;
  if (name.empty()) name = "gomoku-history.txt";

  std::wstring path;
  char tv[1024] = {0};
  DWORD tn = GetEnvironmentVariableA("GB_TEST_SAVE_TXT", tv, sizeof(tv));
  if (tn > 0 && tn < sizeof(tv)) {
    path = Utf8ToWide(tv);                    
  } else {
    path = PickSavePathAs(Utf8ToWide(name), owner,
                          L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0",
                          L"txt", L"导出历史（txt）");
  }
  if (path.empty()) { LogMsg("[hist] export cancelled by user"); return r; }

  FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) { LogMsg("[hist] cannot open for write: " + WideToUtf8(path)); return r; }
  
  const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
  fwrite(bom, 1, sizeof(bom), f);
  fwrite(data.data(), 1, data.size(), f);
  fclose(f);
  r.ok = true;
  r.path = path;
  LogMsg("[hist] txt written: " + WideToUtf8(path) + " (" + std::to_string(data.size()) +
         " bytes, " + std::to_string(r.n) + " games)");
  return r;
}

static std::string ReadTxtMessage(HWND owner) {
  std::wstring path;
  char tv[1024] = {0};
  DWORD tn = GetEnvironmentVariableA("GB_TEST_OPEN_TXT", tv, sizeof(tv));
  if (tn > 0 && tn < sizeof(tv)) {
    path = Utf8ToWide(tv);                    
  } else {
    path = PickOpenTxtPath(owner);
  }
  if (path.empty()) { LogMsg("[hist] import cancelled by user"); return ""; }

  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) { LogMsg("[hist] cannot read: " + WideToUtf8(path)); return ""; }
  std::string txt;
  char buf[4096];
  size_t got;
  while ((got = fread(buf, 1, sizeof(buf), f)) > 0) txt.append(buf, got);
  fclose(f);
  if (txt.size() > 4u * 1024 * 1024) txt.resize(4u * 1024 * 1024);   
  LogMsg("[hist] txt read: " + WideToUtf8(path) + " (" + std::to_string(txt.size()) + " bytes)");
  return "{\"type\":\"histTxt\",\"text\":" + JsonQuote(txt) + "}";
}

static void GdiplusEnsure() {
  static bool done = false;
  if (!done) {
    Gdiplus::GdiplusStartupInput si;
    ULONG_PTR tok = 0;
    GdiplusStartup(&tok, &si, nullptr);
    done = true;                     
  }
}

static bool FindPngEncoderClsid(CLSID* out) {
  UINT num = 0, size = 0;
  
  if (Gdiplus::DllExports::GdipGetImageEncodersSize(&num, &size) != Gdiplus::Ok || !num || !size) return false;
  std::vector<char> buf(size);
  Gdiplus::ImageCodecInfo* ci = (Gdiplus::ImageCodecInfo*)buf.data();
  if (Gdiplus::DllExports::GdipGetImageEncoders(num, size, ci) != Gdiplus::Ok) return false;
  for (UINT i = 0; i < num; i++) {
    if (wcscmp(ci[i].MimeType, L"image/png") == 0) { *out = ci[i].Clsid; return true; }
  }
  return false;
}

static std::string Base64Encode(const std::vector<unsigned char>& in) {
  static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < in.size()) {
    unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
    out += A[(v >> 18) & 63]; out += A[(v >> 12) & 63]; out += A[(v >> 6) & 63]; out += A[v & 63];
    i += 3;
  }
  if (i + 1 == in.size()) {
    unsigned v = in[i] << 16;
    out += A[(v >> 18) & 63]; out += A[(v >> 12) & 63]; out += "==";
  } else if (i + 2 == in.size()) {
    unsigned v = (in[i] << 16) | (in[i + 1] << 8);
    out += A[(v >> 18) & 63]; out += A[(v >> 12) & 63]; out += A[(v >> 6) & 63]; out += '=';
  }
  return out;
}

static std::vector<unsigned char> EncodePngFromHbitmap(HBITMAP hb) {
  std::vector<unsigned char> png;
  GdiplusEnsure();
  CLSID clsid;
  if (FindPngEncoderClsid(&clsid)) {
    
    Gdiplus::Bitmap bmp(hb, nullptr);
    if (bmp.GetLastStatus() == Gdiplus::Ok) {
      wchar_t tmp[MAX_PATH] = { 0 };
      GetTempPathW(MAX_PATH, tmp);
      std::wstring path = Join(std::wstring(tmp), L"gbvis-shot.png");
      DeleteFileW(path.c_str());
      if (bmp.Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok) {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (f) {
          char buf[65536];
          size_t got;
          while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
            png.insert(png.end(), buf, buf + got);
          fclose(f);
        }
        DeleteFileW(path.c_str());
      }
    }
  }
  return png;
}

static std::vector<unsigned char> CaptureRectPng(RECT rc) {
  int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return std::vector<unsigned char>();
  HDC sdc = GetDC(nullptr);
  HDC mdc = CreateCompatibleDC(sdc);
  HBITMAP hb = CreateCompatibleBitmap(sdc, w, h);
  HGDIOBJ old = SelectObject(mdc, hb);
  BitBlt(mdc, 0, 0, w, h, sdc, rc.left, rc.top, SRCCOPY);
  SelectObject(mdc, old);
  DeleteDC(mdc);
  ReleaseDC(nullptr, sdc);
  std::vector<unsigned char> png = EncodePngFromHbitmap(hb);
  DeleteObject(hb);
  return png;
}

static HWND g_visSelHwnd = nullptr;      
static bool g_uiLangEn = false;          
static std::wstring VshT(const wchar_t* zh, const wchar_t* en) {
  return std::wstring(g_uiLangEn ? en : zh);
}

static void MarkerRaiseTopmost(HWND h) {
  if (h && IsWindow(h) && IsWindowVisible(h))
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static LRESULT MarkerOnWndPosChanging(LPARAM lp) {
  WINDOWPOS* p = (WINDOWPOS*)lp;
  if (p && !(p->flags & SWP_NOZORDER)) {
    p->hwndInsertAfter = HWND_TOPMOST;
    p->flags |= SWP_NOOWNERZORDER;
  }
  return 0;
}

static void MarkerExcludeFromCapture(HWND h, const char* tag, bool allowSelf) {
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x11
#endif
  if (allowSelf || !h || !IsWindow(h)) return;
  if (SetWindowDisplayAffinity(h, WDA_EXCLUDEFROMCAPTURE)) return;
  
  static bool told = false;
  if (told) return;
  told = true;
  LogMsg(std::string("[") + tag + "] WDA_EXCLUDEFROMCAPTURE 不受支持（需 Win10 2004+；"
         "Server 2019 / 老 Win10 没有）—— 该窗会进屏幕截图。"
         "规避：用「框选棋盘」锁定 region 缩小抓取范围，或把框挪到不压棋盘的位置");
}

typedef BOOL (WINAPI *FnSetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
typedef UINT (WINAPI *FnGetDpiForWindow)(HWND);
typedef UINT (WINAPI *FnGetDpiForSystem)(void);

static HMODULE GbUser32() {
  static HMODULE m = GetModuleHandleW(L"user32.dll");
  return m;
}

static int GbDpiByGdi() {
  HDC dc = GetDC(nullptr);
  int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
  if (dc) ReleaseDC(nullptr, dc);
  return d >= 96 ? d : 96;
}

static void GbSetDpiAwareness() {
  auto p = (FnSetProcessDpiAwarenessContext)GetProcAddress(GbUser32(), "SetProcessDpiAwarenessContext");
  if (p && p(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE) return;
  SetProcessDPIAware();      
}

static int GbDpiSystem() {
  static FnGetDpiForSystem p = (FnGetDpiForSystem)GetProcAddress(GbUser32(), "GetDpiForSystem");
  UINT d = p ? p() : 0;
  return d >= 96 ? (int)d : GbDpiByGdi();
}

static int GbDpiOfWindow(HWND h) {
  static FnGetDpiForWindow p = (FnGetDpiForWindow)GetProcAddress(GbUser32(), "GetDpiForWindow");
  UINT d = (p && h) ? p(h) : 0;
  return d >= 96 ? (int)d : GbDpiSystem();
}

static int VsDpi() {
  return GbDpiSystem();
}
static int VsS(int v) { return (int)((double)v * VsDpi() / 96.0 + 0.5); }

static const int kMainMinCssW = 680;    
static const int kMainMinCssH = 600;    
static const int kMainDefCssW = 1280;   
static const int kMainDefCssH = 820;    
static const int kVisMinCssW  = 840;    
static const int kVisMinCssH  = 660;

static int GbDpiFor(HWND h) {
  return h ? GbDpiOfWindow(h) : GbDpiSystem();
}

static int GbPx(HWND h, int css) { return MulDiv(css, GbDpiFor(h), 96); }

static void GbClampToWorkArea(HWND h, int& w, int& ht) {
  RECT wa = { 0, 0, 0, 0 };
  if (h) {
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    if (mon && GetMonitorInfoW(mon, &mi)) wa = mi.rcWork;
  }
  if (wa.right <= wa.left || wa.bottom <= wa.top) SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  int aw = wa.right - wa.left, ah = wa.bottom - wa.top;
  if (aw > 0 && w > aw) w = aw;
  if (ah > 0 && ht > ah) ht = ah;
}

static const wchar_t* BrandFontFace();   

static COLORREF VisUiBg()     { return g_uiDark ? RGB(38, 39, 42)    : RGB(246, 246, 244); }
static COLORREF VisUiBtn()    { return g_uiDark ? RGB(58, 59, 63)    : RGB(255, 255, 255); }
static COLORREF VisUiBtnHov() { return g_uiDark ? RGB(72, 73, 78)    : RGB(226, 243, 253); }
static COLORREF VisUiFg()     { return g_uiDark ? RGB(230, 231, 234) : RGB(45, 47, 50); }
static COLORREF VisUiBorder() { return g_uiDark ? RGB(70, 71, 76)    : RGB(210, 210, 206); }
static COLORREF VisSky()      { return RGB(79, 195, 247); }   

static void VisShotDeliver(RECT rc) {
  std::vector<unsigned char> png = CaptureRectPng(rc);
  if (png.empty()) {
    LogMsg("[vis] screenshot failed (no pixels captured)");
    PostToVisPageSoon("{\"type\":\"visShotFail\"}");
    return;
  }
  LogMsg("[vis] screenshot captured (" + std::to_string(png.size()) + " bytes)");
  PostToVisPageSoon("{\"type\":\"visShotData\",\"data\":\"data:image/png;base64," +
                    Base64Encode(png) + "\"}");
}

#define VSEL_TITLE_H (VsS(34))          
#define VSEL_GRIP    (VsS(18))          
#define VSEL_BTN_W   (VsS(96))          
#define VSEL_CLOSE_W (VsS(78))          
#define VSEL_AUTO_W  (VsS(92))          
#define VSEL_BTN_GAP (VsS(10))          
#define VSEL_MIN_BOARD (VsS(300))          
#define VSEL_EDGE    (VsS(10))          

enum { VSEL_NONE = 0, VSEL_MOVE, VSEL_L, VSEL_R, VSEL_T, VSEL_B,
       VSEL_TL, VSEL_TR, VSEL_BL, VSEL_BR,
       VSEL_BTN_SHOT = 100, VSEL_BTN_CLOSE = 101, VSEL_BTN_AUTO = 102 };

static bool VselLoadAuto() {
  wchar_t path[MAX_PATH] = { 0 };
  if (!GetEnvironmentVariableW(L"APPDATA", path, MAX_PATH)) return true;
  std::wstring p = std::wstring(path) + L"\\Meter Gomoku";
  CreateDirectoryW(p.c_str(), nullptr);
  std::wstring f = p + L"\\vsel-auto.cfg";
  FILE* fp = _wfopen(f.c_str(), L"r");
  if (!fp) return true;
  int v = 1;
  if (fscanf(fp, "%d", &v) == 1) { fclose(fp); return v != 0; }
  fclose(fp);
  return true;
}
static void VselSaveAuto(bool on) {
  wchar_t path[MAX_PATH] = { 0 };
  if (!GetEnvironmentVariableW(L"APPDATA", path, MAX_PATH)) return;
  std::wstring p = std::wstring(path) + L"\\Meter Gomoku";
  CreateDirectoryW(p.c_str(), nullptr);
  std::wstring f = p + L"\\vsel-auto.cfg";
  FILE* fp = _wfopen(f.c_str(), L"w");
  if (!fp) return;
  fprintf(fp, "%d", on ? 1 : 0);
  fclose(fp);
}
static bool g_vselAuto = VselLoadAuto();
static RECT g_vselBtnAuto = { 0, 0, 0, 0 };
static std::atomic<bool> g_vselAutoBusy(false);
#define VSEL_WM_SNAP (WM_APP + 9)        

static HBITMAP g_vselBmp = nullptr;      
static unsigned char* g_vselBits = nullptr;
static int g_vselBmpW = 0, g_vselBmpH = 0;
static RECT g_vselBtnShot = { 0, 0, 0, 0 }, g_vselBtnClose = { 0, 0, 0, 0 };
static int g_vselBtnSpan = 0;           
static int g_vselDrag = VSEL_NONE;       
static double g_vselStep = 0;            
static int g_vselHoverBtn = VSEL_NONE;   
static int g_vselPressBtn = VSEL_NONE;   
static POINT g_vselDragPt = { 0, 0 };    
static RECT g_vselDragRect = { 0, 0, 0, 0 };  

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

static void VselClose() {
  if (g_visSelHwnd && IsWindow(g_visSelHwnd)) DestroyWindow(g_visSelHwnd);
  g_visSelHwnd = nullptr;
  if (g_vselBmp) { DeleteObject(g_vselBmp); g_vselBmp = nullptr; }
  g_vselBits = nullptr; g_vselBmpW = 0; g_vselBmpH = 0;
  g_vselDrag = VSEL_NONE; g_vselHoverBtn = VSEL_NONE; g_vselPressBtn = VSEL_NONE;
}

static int VselHit(HWND h, int mx, int my) {
  RECT rc; GetClientRect(h, &rc);
  int W = rc.right, H = rc.bottom;
  if (my >= 0 && my < VSEL_TITLE_H) {
    if (mx >= g_vselBtnAuto.left && mx < g_vselBtnAuto.right &&
        my >= g_vselBtnAuto.top && my < g_vselBtnAuto.bottom) return VSEL_BTN_AUTO;
    if (mx >= g_vselBtnShot.left && mx < g_vselBtnShot.right &&
        my >= g_vselBtnShot.top && my < g_vselBtnShot.bottom) return VSEL_BTN_SHOT;
    if (mx >= g_vselBtnClose.left && mx < g_vselBtnClose.right &&
        my >= g_vselBtnClose.top && my < g_vselBtnClose.bottom) return VSEL_BTN_CLOSE;
    return VSEL_MOVE;                                 
  }
  bool L = (mx < VSEL_GRIP), R = (mx >= W - VSEL_GRIP);
  bool T = (my < VSEL_TITLE_H + VSEL_GRIP), B = (my >= H - VSEL_GRIP);
  if (L && T) return VSEL_TL;
  if (R && T) return VSEL_TR;
  if (L && B) return VSEL_BL;
  if (R && B) return VSEL_BR;
  if (L) return VSEL_L;
  if (R) return VSEL_R;
  if (T) return VSEL_T;
  if (B) return VSEL_B;
  return VSEL_NONE;                                   
}

static void VselCursor(int hit) {
  LPCWSTR c = IDC_ARROW;
  switch (hit) {
    case VSEL_MOVE: c = IDC_SIZEALL; break;
    case VSEL_L: case VSEL_R: c = IDC_SIZEWE; break;
    case VSEL_T: case VSEL_B: c = IDC_SIZENS; break;
    case VSEL_TL: case VSEL_BR: c = IDC_SIZENWSE; break;
    case VSEL_TR: case VSEL_BL: c = IDC_SIZENESW; break;
    default: break;
  }
  SetCursor(LoadCursor(nullptr, c));
}

static void VselRoundPath(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
  if (r > w / 2.0f) r = w / 2.0f;
  if (r > h / 2.0f) r = h / 2.0f;
  p.AddArc(x, y, r * 2, r * 2, 180, 90);
  p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
  p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
  p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
  p.CloseFigure();
}

static void VselPaint(HWND h) {
  RECT rc;
  GetClientRect(h, &rc);
  int W = rc.right - rc.left, H = rc.bottom - rc.top;
  if (W <= 0 || H <= 0) return;
  if (!g_vselBmp || g_vselBmpW != W || g_vselBmpH != H) {
    if (g_vselBmp) DeleteObject(g_vselBmp);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;               
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    g_vselBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void**)&g_vselBits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    g_vselBmpW = W; g_vselBmpH = H;
  }
  if (!g_vselBmp || !g_vselBits) return;

  Gdiplus::Bitmap gb(W, H, W * 4, PixelFormat32bppPARGB, g_vselBits);
  Gdiplus::Graphics g(&gb);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
  
  g.Clear(Gdiplus::Color(0, 0, 0, 0));

  const Gdiplus::Color sky(255, GetRValue(VisSky()), GetGValue(VisSky()), GetBValue(VisSky()));
  {   
    Gdiplus::SolidBrush tb(Gdiplus::Color(236, sky.GetRed(), sky.GetGreen(), sky.GetBlue()));
    g.FillRectangle(&tb, 0.0f, 0.0f, (Gdiplus::REAL)W, (Gdiplus::REAL)VSEL_TITLE_H);
    Gdiplus::Pen ul(Gdiplus::Color(120, 255, 255, 255), 1.0f);
    g.DrawLine(&ul, 0.0f, (Gdiplus::REAL)VSEL_TITLE_H - 0.5f,
               (Gdiplus::REAL)W, (Gdiplus::REAL)VSEL_TITLE_H - 0.5f);
  }
  
  {
    
    int bh = VsS(26), by = (VSEL_TITLE_H - VsS(26)) / 2;
    Gdiplus::FontFamily ff(BrandFontFace());   
    Gdiplus::Font f(&ff, (Gdiplus::REAL)VsS(16), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::StringFormat sf(Gdiplus::StringFormatFlagsNoWrap);
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    bool autoOn = g_vselAuto;
    
    std::wstring ltAuto[2] = { VshT(L"自动 开", L"Auto"), VshT(L"自动 关", L"Off") };
    std::wstring lt[3] = {
      autoOn ? ltAuto[0] : ltAuto[1],
      VshT(L"截图", L"Shot"),
      VshT(L"关闭", L"Close"),
    };
    int base[3] = { VSEL_AUTO_W, VSEL_BTN_W, VSEL_CLOSE_W };
    int bw[3];
    for (int i = 0; i < 3; ++i) {
      Gdiplus::RectF m;
      g.MeasureString(lt[i].c_str(), -1, &f, Gdiplus::PointF(0, 0), &sf, &m);
      bw[i] = (int)(m.Width + 0.999f) + VsS(22);      
      if (bw[i] < base[i]) bw[i] = base[i];
    }
    {   
      int wMax = 0;
      for (int i = 0; i < 2; ++i) {
        Gdiplus::RectF m;
        g.MeasureString(ltAuto[i].c_str(), -1, &f, Gdiplus::PointF(0, 0), &sf, &m);
        int w = (int)(m.Width + 0.999f) + VsS(22);
        if (w > wMax) wMax = w;
      }
      if (wMax > bw[0]) bw[0] = wMax;
    }
    int gap = VSEL_BTN_GAP;
    int span = VSEL_EDGE + bw[0] + bw[1] + bw[2] + gap * 2;
    g_vselBtnSpan = span;
    if (span > W - VsS(4)) {                          
      gap = VsS(4);
      span = VSEL_EDGE + bw[0] + bw[1] + bw[2] + gap * 2;
    }
    int bx3 = W - VSEL_EDGE - bw[2];
    int bx2 = bx3 - gap - bw[1];
    int bx1 = bx2 - gap - bw[0];
    g_vselBtnAuto  = { bx1, by, bx1 + bw[0], by + bh };
    g_vselBtnShot  = { bx2, by, bx2 + bw[1], by + bh };
    g_vselBtnClose = { bx3, by, bx3 + bw[2], by + bh };
    struct BtnDef { RECT r; int id; int rr, gg, bb; const std::wstring* t; int tr, tg, tb; };
    BtnDef defs[3] = {
      
      { g_vselBtnAuto,  VSEL_BTN_AUTO,  autoOn ? 72 : 96, autoOn ? 201 : 118, autoOn ? 186 : 136,
        &lt[0], autoOn ? 10 : 255, autoOn ? 62 : 255, autoOn ? 56 : 255 },
      { g_vselBtnShot,  VSEL_BTN_SHOT,  2, 136, 209, &lt[1], 255, 255, 255 },
      { g_vselBtnClose, VSEL_BTN_CLOSE, 214, 96,  96, &lt[2], 255, 255, 255 },
    };
    for (int i = 0; i < 3; ++i) {
      const BtnDef& d = defs[i];
      bool hov = (g_vselHoverBtn == d.id), prs = (g_vselPressBtn == d.id);
      int r = d.rr, gg = d.gg, bb = d.bb;
      if (hov && !prs) { r += 20; gg += 20; bb += 12; }
      if (prs)         { r -= 26; gg -= 26; bb -= 20; }
      if (r < 0) r = 0; if (gg < 0) gg = 0; if (bb < 0) bb = 0;
      if (r > 255) r = 255; if (gg > 255) gg = 255; if (bb > 255) bb = 255;
      Gdiplus::GraphicsPath p;
      VselRoundPath(p, (float)d.r.left, (float)d.r.top,
                    (float)(d.r.right - d.r.left), (float)(d.r.bottom - d.r.top), (float)VsS(5));
      Gdiplus::SolidBrush fb(Gdiplus::Color(255, (BYTE)r, (BYTE)gg, (BYTE)bb));
      g.FillPath(&fb, &p);
      Gdiplus::SolidBrush tb(Gdiplus::Color(255, (BYTE)d.tr, (BYTE)d.tg, (BYTE)d.tb));
      g.DrawString(d.t->c_str(), -1, &f,
                   Gdiplus::RectF((float)d.r.left, (float)d.r.top,
                                  (float)(d.r.right - d.r.left), (float)(d.r.bottom - d.r.top)),
                   &sf, &tb);
    }
  }
  
  {
    Gdiplus::FontFamily ff(BrandFontFace());
    Gdiplus::Font f(&ff, (Gdiplus::REAL)VsS(17), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush b(Gdiplus::Color(255, 10, 54, 74));
    
    const wchar_t* cands[3] = {
      g_uiLangEn ? L"Drag · Shot" : L"拖到棋盘 · 点截图",
      g_uiLangEn ? L"Shot"        : L"拖到棋盘",
      g_uiLangEn ? L""            : L"截图",
    };
    Gdiplus::StringFormat sf(Gdiplus::StringFormatFlagsNoWrap);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::REAL room = (Gdiplus::REAL)(g_vselBtnAuto.left - VsS(8));
    for (int i = 0; i < 3; ++i) {
      if (!cands[i][0]) break;                       
      Gdiplus::RectF msz;
      g.MeasureString(cands[i], -1, &f, Gdiplus::PointF(0, 0), &sf, &msz);
      if (VsS(12) + msz.Width <= room) {
        g.DrawString(cands[i], -1, &f, Gdiplus::PointF((Gdiplus::REAL)VsS(12),
                     (Gdiplus::REAL)(VSEL_TITLE_H / 2)), &sf, &b);
        break;
      }
    }
  }
  
  {
    Gdiplus::Pen pen(sky, (Gdiplus::REAL)VsS(3));
    g.DrawRectangle(&pen, 1.0f, (Gdiplus::REAL)VSEL_TITLE_H + 1.0f,
                    (Gdiplus::REAL)(W - 2), (Gdiplus::REAL)(H - VSEL_TITLE_H - 2));
    Gdiplus::SolidBrush hb(sky);
    const int hs = VsS(12);     
    float pts[4][2] = {
      {1.0f, (float)VSEL_TITLE_H + 1.0f},
      {(float)(W - 1 - hs), (float)VSEL_TITLE_H + 1.0f},
      {1.0f, (float)(H - 1 - hs)},
      {(float)(W - 1 - hs), (float)(H - 1 - hs)},
    };
    for (int i = 0; i < 4; ++i) g.FillRectangle(&hb, pts[i][0], pts[i][1], (float)hs, (float)hs);
  }
  HDC sdc = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(sdc);
  HGDIOBJ old = SelectObject(mem, g_vselBmp);
  POINT src = { 0, 0 };
  SIZE sz = { W, H };
  BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
  UpdateLayeredWindow(h, sdc, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);
  SelectObject(mem, old);
  DeleteDC(mem);
  ReleaseDC(nullptr, sdc);
}

static bool VselAutoFindRect(HWND h, RECT& br, double& step);
static void VselShot(HWND h) {
  RECT wr;
  GetWindowRect(h, &wr);
  int top = wr.top + VSEL_TITLE_H;
  RECT rc = { wr.left, top, wr.right, wr.bottom };
  if (rc.right - rc.left < 24 || rc.bottom - rc.top < 24) return;
  // Manual box too small to hold a board: run one automatic full-screen board
  // search and use its (larger, 0.6-cell padded) rect instead.
  if (rc.right - rc.left < VSEL_MIN_BOARD || rc.bottom - rc.top < VSEL_MIN_BOARD) {
    RECT br;
    double step = 0;
    if (VselAutoFindRect(h, br, step)) {
      double sp = step > 0 ? step : (double)(br.right - br.left) / 14.0;
      int pad = (int)(sp * 0.6 + 0.5);
      if (pad < 6) pad = 6;
      int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
      int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
      RECT ex = { br.left - pad, br.top - pad, br.right + pad, br.bottom + pad };
      if (ex.left < vx) ex.left = vx;
      if (ex.top < vy) ex.top = vy;
      if (ex.right > vx + vw) ex.right = vx + vw;
      if (ex.bottom > vy + vh) ex.bottom = vy + vh;
      LogMsg("[vis] small manual box -> auto board rect used");
      VisShotDeliver(ex);
      return;
    }
  }
  VisShotDeliver(rc);
}

static std::wstring FindVisionExe();                       

static bool VisionScanImage(const unsigned char* img, int len, std::string& out);
// ★ 2026-10-19：识图识别改为「识别参数里那 5 项阈值」下发给视觉服务的同步读子（algo=sync + recog）。
//   recogJson 为空时自动取宿主当前的 g_visRc* （VisRecogJson）。
static bool VisionRecognize(const unsigned char* img, int len, int size, bool nosnap,
                            const std::string& recogJson, std::string& out);

static bool VselScanRect(const std::string& s, RECT& out, double& stepOut) {
  stepOut = 0;
  size_t i = s.find("\"found\"");
  if (i == std::string::npos) return false;
  i = s.find(':', i);
  if (i == std::string::npos) return false;
  ++i;
  while (i < s.size() && s[i] == ' ') ++i;
  if (s.compare(i, 4, "true") != 0) return false;
  size_t b = s.find("\"board_rect\"");
  if (b == std::string::npos) return false;
  b = s.find('{', b);
  size_t e = s.find('}', b == std::string::npos ? 0 : b);
  if (b == std::string::npos || e == std::string::npos) return false;
  std::string seg = s.substr(b, e - b + 1);
  double v[4] = { 0, 0, 0, 0 };
  const char* keys[4] = { "x", "y", "w", "h" };
  for (int k = 0; k < 4; ++k) {
    std::string kk = std::string("\"") + keys[k] + "\"";
    size_t p = seg.find(kk);
    if (p == std::string::npos) return false;
    p = seg.find(':', p + kk.size());
    if (p == std::string::npos) return false;
    char* end = nullptr;
    v[k] = strtod(seg.c_str() + p + 1, &end);
    if (end == seg.c_str() + p + 1) return false;
  }
  if (v[2] < 150 || v[3] < 150) return false;          
  double aspect = v[2] / v[3];
  if (aspect < 0.5 || aspect > 2.0) return false;      
  size_t sp = s.find("\"spacing\"");
  if (sp != std::string::npos) {
    size_t p = s.find(':', sp + 9);
    if (p != std::string::npos) {
      char* end = nullptr;
      double sv = strtod(s.c_str() + p + 1, &end);
      if (end != s.c_str() + p + 1 && sv > 4 && sv < (double)(v[2] + v[3])) stepOut = sv;
    }
  }
  out.left = (LONG)(v[0] + 0.5);
  out.top = (LONG)(v[1] + 0.5);
  out.right = (LONG)(v[0] + v[2] + 0.5);
  out.bottom = (LONG)(v[1] + v[3] + 0.5);
  return true;
}

// ★★ 2026-10-19（用户：「在识图这个界面上也添加一个识别参数……5 个调整参数（没有时间这个选项）」＋
//   「还有自动吸附找盘，也是没有生效，也是没有用连线的自动吸附棋盘的那套算法导致识别速度很低质量
//   很差」）：
//   识图窗的「识别参数」= 连线那套同步识别阈值里的 5 项（**不含 interval**：识图是单次调用，
//   不需要扫描节拍）。宿主写盘为单一权威（%TEMP%\GomokuTrainer.visrecog，与练习器同名共用 ——
//   两边都是同一个「识图识别参数」，口径一致），启动读回，关软件再打开不回退默认。
//   每次「自动吸附盘」把这 5 项随 "algo":"sync","recog":{...} 下发给视觉服务 —— **服务端自己
//   抓屏**（op=SCAN 命名管道，快车道），省掉一次「整屏 PNG → 共享内存 → 服务端解码 + 兜底起
//   独立进程」的大往返，所以又快又准；拿不到棋盘再回退下面的图像路径。
static std::atomic<int> g_visRcGray(50);        // 灰度偏差(0-255)
static std::atomic<int> g_visRcBlackOff(96);    // 黑色偏差(0-255)
static std::atomic<int> g_visRcWhiteOff(96);    // 白色偏差(0-255)
static std::atomic<int> g_visRcBlackPct(33);    // 黑色占比(0-100)
static std::atomic<int> g_visRcWhitePct(33);    // 白色占比(0-100)

static std::wstring VisRecogPrefFile() {
  wchar_t tmp[MAX_PATH] = { 0 };
  GetTempPathW(MAX_PATH, tmp);
  return Join(std::wstring(tmp), L"GomokuTrainer.visrecog");
}

static void ClampVisRecog(int& gray, int& blackOff, int& whiteOff, int& blackPct, int& whitePct) {
  auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
  gray     = clamp(gray, 0, 255);
  blackOff = clamp(blackOff, 0, 255);
  whiteOff = clamp(whiteOff, 0, 255);
  blackPct = clamp(blackPct, 0, 100);
  whitePct = clamp(whitePct, 0, 100);
}

static std::string VisRecogJson() {
  char b[192];
  snprintf(b, sizeof(b),
           "{\"gray_offset\":%d,\"black_offset\":%d,\"white_offset\":%d,"
           "\"black_percent\":%d,\"white_percent\":%d}",
           g_visRcGray.load(), g_visRcBlackOff.load(), g_visRcWhiteOff.load(),
           g_visRcBlackPct.load(), g_visRcWhitePct.load());
  return std::string(b);
}

static void LoadVisRecogPrefs() {
  FILE* f = _wfopen(VisRecogPrefFile().c_str(), L"rb");
  if (!f) return;
  char buf[64] = { 0 };
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  int gr = 50, bo = 96, wo = 96, bp = 33, wp = 33;
  if (n == 0 || sscanf(buf, "%d|%d|%d|%d|%d", &gr, &bo, &wo, &bp, &wp) < 5) return;
  ClampVisRecog(gr, bo, wo, bp, wp);
  g_visRcGray.store(gr);
  g_visRcBlackOff.store(bo);
  g_visRcWhiteOff.store(wo);
  g_visRcBlackPct.store(bp);
  g_visRcWhitePct.store(wp);
  LogMsg("[vis] loaded recog params: " + VisRecogJson());
}

static void SaveVisRecogPrefs() {
  std::string line = std::to_string(g_visRcGray.load()) + "|" +
                     std::to_string(g_visRcBlackOff.load()) + "|" +
                     std::to_string(g_visRcWhiteOff.load()) + "|" +
                     std::to_string(g_visRcBlackPct.load()) + "|" +
                     std::to_string(g_visRcWhitePct.load());
  FILE* f = _wfopen(VisRecogPrefFile().c_str(), L"wb");
  if (!f) return;
  fwrite(line.data(), 1, line.size(), f);
  fclose(f);
}

static bool VisionScanScreen(const std::string& body, std::string& out);   // 服务端抓屏识别（快车道）

// ★★ 2026-10-19（用户：「还有这个屏幕截图的自动功能，代码仍然是老的，要重新构建」）：
//   「屏幕截图」浮窗的自动吸附找盘 = **只走**连线同一条「服务端抓屏找盘」快车道：请求交给
//   视觉服务、由**服务端自己抓屏**找盘（op=SCAN 命名管道），同步阈值（algo=sync）与识别参数
//   那 5 项一起下发；exclude 把本浮窗那条标题栏挖掉（它压着棋盘上沿，会干扰找盘）。
//   旧的「宿主抓整屏 PNG → 共享内存（VisionScanImage）→ 服务端解码」以及兜底另起独立进程
//   （--scan-image）两条老路径**整体移除**（项目硬约束：识图自动吸附不得保留旧的整屏截图转
//   共享内存实现路径）。服务不可用就是暂时找不到棋盘，手动框选照常可用。
static bool VselAutoFindRect(HWND h, RECT& br, double& step) {
  int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  std::string body = std::string("{\"size\":15,\"algo\":\"sync\",\"recog\":") + VisRecogJson();
  if (h) {
    RECT wr;
    GetWindowRect(h, &wr);
    body += ",\"exclude\":[{\"x\":" + std::to_string(wr.left) +
            ",\"y\":" + std::to_string(wr.top) +
            ",\"w\":" + std::to_string((long long)(wr.right - wr.left)) +
            ",\"h\":" + std::to_string(VSEL_TITLE_H) + "}]";
  }
  body += "}";
  std::string out;
  if (!VisionScanScreen(body, out)) {
    LogMsg("[vis] auto find: vision scan unavailable (fast lane only)");
    return false;
  }
  if (!VselScanRect(out, br, step)) {
    LogMsg("[vis] auto find: no board in scan reply");
    return false;
  }
  br.left += vx; br.right += vx; br.top += vy; br.bottom += vy;
  return true;
}

static void VselAutoScan(HWND h) {
  if (g_vselAutoBusy.exchange(true)) return;
  std::thread([h] {
    RECT br;
    double step = 0;
    bool got = VselAutoFindRect(h, br, step);
    if (got) {
      g_vselStep = step;
      RECT* rp = new RECT(br);
      if (!PostMessageW(h, VSEL_WM_SNAP, 0, (LPARAM)rp)) delete rp;
    }
    g_vselAutoBusy = false;
  }).detach();
}

static LRESULT CALLBACK VselProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    
    case WM_WINDOWPOSCHANGING:
      return MarkerOnWndPosChanging(lp);
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        VselCursor(VselHit(h, pt.x, pt.y));
        return TRUE;
      }
      break;
    case WM_MOUSEMOVE: {
      int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
      if (g_vselDrag != VSEL_NONE && g_vselDrag != VSEL_BTN_SHOT && g_vselDrag != VSEL_BTN_CLOSE) {
        POINT pt; GetCursorPos(&pt);
        int dx = pt.x - g_vselDragPt.x, dy = pt.y - g_vselDragPt.y;
        RECT r = g_vselDragRect;
        switch (g_vselDrag) {
          case VSEL_MOVE: r.left += dx; r.right += dx; r.top += dy; r.bottom += dy; break;
          case VSEL_L: r.left  += dx; break;
          case VSEL_R: r.right += dx; break;
          case VSEL_T: r.top   += dy; break;
          case VSEL_B: r.bottom += dy; break;
          case VSEL_TL: r.left += dx; r.top += dy; break;
          case VSEL_TR: r.right += dx; r.top += dy; break;
          case VSEL_BL: r.left += dx; r.bottom += dy; break;
          case VSEL_BR: r.right += dx; r.bottom += dy; break;
          default: break;
        }
        
        int minW = VsS(330), minH = VSEL_TITLE_H + VsS(60);
        
        if (g_vselBtnSpan > 0 && g_vselBtnSpan + VsS(30) > minW) minW = g_vselBtnSpan + VsS(30);
        if (r.right - r.left < minW) {
          if (g_vselDrag == VSEL_L || g_vselDrag == VSEL_TL || g_vselDrag == VSEL_BL)
            r.left = r.right - minW;
          else r.right = r.left + minW;
        }
        if (r.bottom - r.top < minH) {
          if (g_vselDrag == VSEL_T || g_vselDrag == VSEL_TL || g_vselDrag == VSEL_TR)
            r.top = r.bottom - minH;
          else r.bottom = r.top + minH;
        }
        SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        VselPaint(h);
        return 0;
      }
      int hit = VselHit(h, mx, my);
      int hov = (hit == VSEL_BTN_SHOT || hit == VSEL_BTN_CLOSE || hit == VSEL_BTN_AUTO) ? hit : VSEL_NONE;
      if (hov != g_vselHoverBtn) { g_vselHoverBtn = hov; VselPaint(h); }
      VselCursor(hit);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      int hit = VselHit(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      if (hit == VSEL_BTN_SHOT || hit == VSEL_BTN_CLOSE || hit == VSEL_BTN_AUTO) {
        g_vselPressBtn = hit; VselPaint(h);
        SetCapture(h);
        return 0;
      }
      if (hit == VSEL_NONE) return 0;
      g_vselDrag = hit;
      GetCursorPos(&g_vselDragPt);
      GetWindowRect(h, &g_vselDragRect);
      SetCapture(h);
      return 0;
    }
    case WM_LBUTTONUP: {
      ReleaseCapture();
      if (g_vselPressBtn != VSEL_NONE) {
        int hit = VselHit(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        int pressed = g_vselPressBtn;
        g_vselPressBtn = VSEL_NONE;
        if (hit == pressed) {
          if (pressed == VSEL_BTN_AUTO) {
            g_vselAuto = !g_vselAuto;
            VselSaveAuto(g_vselAuto);        
            LogMsg(std::string("[vis] auto snap ") + (g_vselAuto ? "on" : "off"));
            if (g_vselAuto) VselAutoScan(h); 
            VselPaint(h);
            return 0;
          }
          if (pressed == VSEL_BTN_SHOT) {
            VselShot(h);                     
          } else {
            LogMsg("[vis] hollow select closed");
            VselClose();
            PostToVisPageSoon("{\"type\":\"visShotCancel\"}");
          }
          return 0;
        }
        VselPaint(h);
        return 0;
      }
      g_vselDrag = VSEL_NONE;
      return 0;
    }
    case WM_TIMER:
      if (wp == 1 && g_vselAuto && g_vselDrag == VSEL_NONE) VselAutoScan(h);
      MarkerRaiseTopmost(h);   
      return 0;
    case VSEL_WM_SNAP: {
      
      RECT* rp = (RECT*)lp;
      if (!rp) return 0;
      RECT b = *rp;
      delete rp;
      if (!g_vselAuto || g_vselDrag != VSEL_NONE) return 0;
      
      LONG bw = b.right - b.left, bh = b.bottom - b.top;
      double step = (g_vselStep > 0) ? g_vselStep : (double)bw / 14.0;
      int pad = (int)(step * 0.6 + 0.5);
      if (pad < 6) pad = 6;
      LONG x = b.left - pad, y = b.top - pad - VSEL_TITLE_H;
      LONG w = bw + pad * 2;
      LONG hh = bh + pad * 2 + VSEL_TITLE_H;
      int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
      int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
      int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
      if (y < vy) y = vy;                      
      if (x < vx) { w -= (vx - x); x = vx; }   
      if (y + hh > vy + vh) hh = vy + vh - y;
      if (x + w > vx + vw) w = vx + vw - x;
      RECT cur;
      GetWindowRect(h, &cur);
      int dx = cur.left - x, dy = cur.top - y;
      int dw = (cur.right - cur.left) - (int)w, dh = (cur.bottom - cur.top) - (int)hh;
      if (dx > -6 && dx < 6 && dy > -6 && dy < 6 &&
          dw > -6 && dw < 6 && dh > -6 && dh < 6)
        return 0;                              
      MoveWindow(h, x, y, w, hh, TRUE);
      VselPaint(h);
      LogMsg("[vis] auto snap -> board rect");
      return 0;
    }
    case WM_KEYDOWN:
      if (wp == VK_ESCAPE) {
        LogMsg("[vis] hollow select cancelled (esc)");
        VselClose();
        PostToVisPageSoon("{\"type\":\"visShotCancel\"}");
      }
      return 0;
    case WM_SIZE:
      VselPaint(h);
      return 0;
    
    case WM_CLOSE:
      VselClose();
      return 0;
    case WM_DESTROY:
      KillTimer(h, 1);
      g_visSelHwnd = nullptr;
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

static void RunVisHollowSelect() {
  
  GdiplusEnsure();
  static const wchar_t* kCls = L"GbCalcVisHollow";
  WNDCLASSEXW wc;
  ZeroMemory(&wc, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = VselProc;
  wc.hInstance = g_hInst;
  wc.lpszClassName = kCls;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassExW(&wc);
  RECT wa = { 0, 0, 0, 0 };
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  int w = VsS(480), h = VsS(360) + VSEL_TITLE_H;     
  int x = wa.left + (wa.right - wa.left - w) / 2;
  int y = wa.top + (wa.bottom - wa.top - h) / 2;
  g_visSelHwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
                                 kCls, nullptr, WS_POPUP, x, y, w, h,
                                 nullptr, nullptr, g_hInst, nullptr);
  if (!g_visSelHwnd) {
    LogMsg("[vis] failed to create hollow select window");
    PostToVisPageSoon("{\"type\":\"visShotFail\"}");
    return;
  }
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x11
#endif
  
  {
    char cap[8] = { 0 };
    if (GetEnvironmentVariableA("GB_TEST_CAPTURABLE", cap, sizeof(cap)) && cap[0] == '1') {
      LogMsg("[vis] (test) self-capture allowed -> hollow select left capturable on purpose");
    } else {
      MarkerExcludeFromCapture(g_visSelHwnd, "vis", false);
    }
  }
  ShowWindow(g_visSelHwnd, SW_SHOWNORMAL);
  VselPaint(g_visSelHwnd);
  SetTimer(g_visSelHwnd, 1, 1200, nullptr);   
  LogMsg("[vis] hollow select opened (drag title to move, edges to resize, ESC to cancel)");
  MSG msg;
  while (IsWindow(g_visSelHwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  LogMsg("[vis] hollow select loop ended");
}

static void StartVisShotFlow() {
  LogMsg("[vis] screenshot requested (hollow select)");
  if (g_visSelHwnd && IsWindow(g_visSelHwnd)) return;   
  RunVisHollowSelect();
}

static std::wstring FindVisionExe() {
  
  std::wstring sub = Join(Join(ExeDir(), L"Vision"), L"GomokuVision.exe");
  if (FileExists(sub)) return sub;
  std::wstring c = Join(ExeDir(), L"GomokuVision.exe");
  if (FileExists(c)) return c;
  std::wstring up = ExeDir();
  for (int i = 0; i < 3; ++i) {
    size_t p = up.find_last_of(L"\\/");
    if (p == std::wstring::npos) break;
    up = up.substr(0, p);
    std::wstring c2 = Join(Join(up, L"desktop-vision"), L"build\\GomokuVision.exe");
    if (FileExists(c2)) return c2;
  }
  return L"";
}

static int VisionRecogPort() {
  static int p = 0;
  if (p) return p;
  char v[16] = {0};
  if (GetEnvironmentVariableA("GB_RECOG_PORT", v, sizeof(v)) && v[0]) {
    int n = atoi(v);
    if (n >= 1024 && n <= 65535) p = n;
  }
  if (!p) p = 8974;
  return p;
}
static int VisionScanPort() {
  static int p = 0;
  if (p) return p;
  char v[16] = {0};
  if (GetEnvironmentVariableA("GB_SCAN_PORT", v, sizeof(v)) && v[0]) {
    int n = atoi(v);
    if (n >= 1024 && n <= 65535) p = n;
  }
  if (!p) p = 8975;
  return p;
}

static bool VisionServiceUp(int idx);

// ★★ 2026-10-17（用户：「识别速度到后期会变得相当慢……挤占视觉线路」）：
//   优先级现状 = WebView2 渲染 HIGH / 引擎 ABOVE_NORMAL / **视觉 NORMAL**。
//   引擎 12 线程满核搜索时，NORMAL 的识别进程排在最后 —— 越到中后盘思考越久越明显。
//   视觉是交互实时任务（一次识别几百 ms），抬到 HIGH 让它插队做完；引擎只在这一瞬
//   少一个核，整体 nps 影响 < 10%，远小于识别卡顿的体感损失。
static void BoostVisionPriority(HANDLE h) {
  if (!h) return;
  if (SetPriorityClass(h, HIGH_PRIORITY_CLASS))
    LogMsg("[vis] vision process priority -> HIGH (识别优先于引擎搜索)");
}

static void BoostExistingVisionPriority() {
  static std::atomic<bool> done{ false };
  bool expect = false;
  if (!done.compare_exchange_strong(expect, true)) return;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
  int n = 0;
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, L"GomokuVision.exe") == 0) {
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pe.th32ProcessID);
        if (h) {
          if (SetPriorityClass(h, HIGH_PRIORITY_CLASS)) ++n;
          CloseHandle(h);
        }
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  if (n > 0) LogMsg("[vis] reusing vision service -> priority HIGH (" + std::to_string(n) + " process)");
}

static void VisionSpawnDetached(const std::wstring& exe, const std::wstring& cwd) {
  std::wstring cmd = L"\"" + exe + L"\"";
  STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
  PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
  std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
  
  if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                     cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
    CloseHandle(pi.hThread);
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);   
    ApplyVisionAffinity(pi.hProcess);          
    BoostVisionPriority(pi.hProcess);
    ChildTrack(pi.hProcess);                   
    CloseHandle(pi.hProcess);
  } else {
    LogMsg("[vis] vision service spawn failed err=" + std::to_string(GetLastError()));
  }
}

static std::atomic<unsigned long long> g_visSpawnedAt(0);
static void EnsureVisionServer() {
  static std::atomic<int> seenAlive(0);
  static std::atomic<int> restarts(0);
  
  {
    static bool envPinned = false;
    if (!envPinned) {
      envPinned = true;
      SetEnvironmentVariableA("GB_RECOG_PORT", std::to_string(VisionRecogPort()).c_str());
      SetEnvironmentVariableA("GB_SCAN_PORT", std::to_string(VisionScanPort()).c_str());
      // ★ 2026-10-13：识别进程去 HTTP —— 明确把它钉成 0（即使将来有人给 spawn 加了
      //   --scan/--recognize 单角色旗标，本应用的识别进程也**永远是纯管道、零 TCP**）。
      SetEnvironmentVariableA("GB_VISION_HTTP", "0");
      SetEnvironmentVariableA("GB_PARENT_PID", std::to_string(GetCurrentProcessId()).c_str());
    }
  }
  BoostExistingVisionPriority();
  bool scanAlive = VisionServiceUp(1);              
  bool recogAlive = VisionServiceUp(0);
  if (scanAlive && recogAlive) {
    if (seenAlive.exchange(1) == 0) {
      static std::atomic<bool> told(false);
      if (!told.exchange(true))
        LogMsg("[vis] vision service :" + std::to_string(VisionRecogPort()) + "/:" +
               std::to_string(VisionScanPort()) + " already running, reusing it");
    }
    return;
  }
  if (seenAlive.exchange(0) == 1) {
    LogMsg("[vis] vision service went down (scan=" + std::string(scanAlive ? "up" : "down") +
           " recog=" + std::string(recogAlive ? "up" : "down") + ") -> restarting (restart #" +
           std::to_string(restarts.load() + 1) + ")");
  }
  std::wstring exe = FindVisionExe();
  if (exe.empty()) { LogMsg("[vis] GomokuVision.exe not found, cannot start vision service"); return; }
  unsigned long long now = GetTickCount64(), cur = g_visSpawnedAt.load();
  for (;;) {
    if (now - cur < 25000) return;                        
    if (g_visSpawnedAt.compare_exchange_weak(cur, now)) break;
  }
  std::wstring cwd = exe.substr(0, exe.find_last_of(L"\\/"));
  LogMsg("[vis] starting vision service GomokuVision.exe (both :" +
         std::to_string(VisionRecogPort()) + "/:" + std::to_string(VisionScanPort()) + ")");
  VisionSpawnDetached(exe, cwd);
  restarts.fetch_add(1);
}

static std::atomic<bool> g_visPrewarmStarted{ false };
static void PrewarmVisionServiceAsync() {
  bool expect = false;
  if (!g_visPrewarmStarted.compare_exchange_strong(expect, true)) return;
  std::thread([] {
    EnsureVisionServer();
    for (int i = 0; i < 40; ++i) {                        
      if (VisionServiceUp(0) && VisionServiceUp(1)) {
        LogMsg("[vis] prewarmed (vision service ready before first recognize)");
        return;
      }
      Sleep(300);
    }
    LogMsg("[vis] prewarm: service not ready in 12s (first recognize will retry)");
  }).detach();
}

namespace {

constexpr DWORD kVisPipeShmSize = 64u * 1024 * 1024;   
constexpr unsigned VISOP_SCAN = 2;                      
constexpr unsigned VISOP_SCAN_IMAGE = 3;                
constexpr unsigned VISOP_RECOGNIZE = 4;

struct VisPipeCtx {
  CRITICAL_SECTION cs;
  HANDLE pipe = nullptr;        
  bool hello = false;           
  HANDLE shm = nullptr;         
  void* view = nullptr;
  std::wstring shmName;         
  VisPipeCtx() { InitializeCriticalSection(&cs); }
  ~VisPipeCtx() {
    if (view) UnmapViewOfFile(view);
    if (shm) CloseHandle(shm);
    if (pipe) { DisconnectNamedPipe(pipe); CloseHandle(pipe); }
    DeleteCriticalSection(&cs);
  }
};
VisPipeCtx g_vpipe[2];          

bool VisPipeIo(HANDLE h, void* buf, DWORD n, bool write) {
  char* p = (char*)buf;
  while (n > 0) {
    DWORD x = 0;
    if (!(write ? WriteFile(h, p, n, &x, nullptr) : ReadFile(h, p, n, &x, nullptr)) || x == 0)
      return false;
    p += x; n -= x;
  }
  return true;
}

bool VisPipeFrame(HANDLE h, unsigned opcode, const void* payload, unsigned len, bool write) {
  unsigned char hdr[12];
  memcpy(hdr, "GBV1", 4);
  unsigned op = opcode;                     
  memcpy(hdr + 4, &op, 4);
  memcpy(hdr + 8, &len, 4);
  if (!VisPipeIo(h, hdr, 12, write)) return false;
  if (len && !VisPipeIo(h, (void*)payload, len, write)) return false;
  return true;
}

bool VisPipeConnect(VisPipeCtx& c, int idx) {
  
  wchar_t name[64];
  swprintf_s(name, L"\\\\.\\pipe\\GomokuVision.%s.%d",
             (idx == 0) ? L"recognize" : L"scan",
             (idx == 0) ? VisionRecogPort() : VisionScanPort());
  HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) { c.pipe = nullptr; c.hello = false; return false; }
  c.pipe = h;
  
  unsigned char hdr[12];
  if (!VisPipeFrame(h, 1, nullptr, 0, true) ||          
      !VisPipeIo(h, hdr, 12, false) ||
      memcmp(hdr, "GBV1", 4) != 0) {                    
    DisconnectNamedPipe(h); CloseHandle(h); c.pipe = nullptr; c.hello = false;
    return false;
  }
  unsigned plen = 0;
  memcpy(&plen, hdr + 8, 4);
  unsigned char pl[512];
  if (plen != 8 || plen > sizeof(pl) ||
      !VisPipeIo(h, pl, plen, false)) {                 
    DisconnectNamedPipe(h); CloseHandle(h); c.pipe = nullptr; c.hello = false;
    return false;
  }
  unsigned ver = 0, ready = 0;
  memcpy(&ver, pl, 4);
  memcpy(&ready, pl + 4, 4);
  if (ver != 1 || !ready) {                         
    DisconnectNamedPipe(h); CloseHandle(h); c.pipe = nullptr; c.hello = false;
    return false;
  }
  c.hello = true;
  return true;
}

bool VisPipeEnsureShm(VisPipeCtx& c) {
  if (c.shm && c.view) return true;
  wchar_t name[64];
  swprintf_s(name, L"GBV_SHM_%lu", (unsigned long)GetCurrentProcessId());
  HANDLE s = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                0, kVisPipeShmSize, name);
  if (!s) return false;
  void* v = MapViewOfFile(s, FILE_MAP_WRITE, 0, 0, 0);
  if (!v) { CloseHandle(s); return false; }
  c.shm = s; c.view = v; c.shmName = name;
  return true;
}

bool VisPipeCall(int idx, unsigned opcode, const unsigned char* img, int imgLen,
                 const std::string& extra, std::string& out) {
  VisPipeCtx& c = g_vpipe[idx];
  EnterCriticalSection(&c.cs);
  bool ok = false;
  std::string resp;
  if (VisPipeEnsureShm(c) && imgLen > 0 && (DWORD)imgLen <= kVisPipeShmSize) {
    for (int attempt = 0; attempt < 2 && !ok; ++attempt) {     
      if (!c.pipe && !VisPipeConnect(c, idx)) break;
      
      std::string nameUtf8;
      for (size_t i = 0; i < c.shmName.size(); ++i)
        nameUtf8.push_back((char)(c.shmName[i] & 0xFF));       
      std::vector<unsigned char> pl;
      unsigned nameLen = (unsigned)nameUtf8.size();
      const unsigned char* nm = (const unsigned char*)nameUtf8.data();
      pl.insert(pl.end(), (unsigned char*)&nameLen, (unsigned char*)&nameLen + 4);
      pl.insert(pl.end(), nm, nm + nameLen);
      unsigned off = 0, ilen = (unsigned)imgLen;
      pl.insert(pl.end(), (unsigned char*)&off, (unsigned char*)&off + 4);
      pl.insert(pl.end(), (unsigned char*)&ilen, (unsigned char*)&ilen + 4);
      if (opcode == VISOP_RECOGNIZE) {
        unsigned elen = (unsigned)extra.size();
        pl.insert(pl.end(), (unsigned char*)&elen, (unsigned char*)&elen + 4);
        if (elen) pl.insert(pl.end(), extra.begin(), extra.end());
      }
      
      memcpy(c.view, img, imgLen);
      unsigned char hdr[12];
      if (VisPipeFrame(c.pipe, opcode, pl.data(), (unsigned)pl.size(), true) &&
          VisPipeIo(c.pipe, hdr, 12, false) && memcmp(hdr, "GBV1", 4) == 0) {
        unsigned plen = 0;
        memcpy(&plen, hdr + 8, 4);
          if (plen >= 8 && plen <= 64u * 1024 * 1024) {
            std::vector<unsigned char> rp(plen);
            if (VisPipeIo(c.pipe, rp.data(), plen, false)) {
              unsigned status = 0, rlen = 0;
              memcpy(&status, rp.data(), 4);
              memcpy(&rlen, rp.data() + 4, 4);
              if (status == 0 && rlen + 8 == plen) {
                resp.assign((const char*)rp.data() + 8, rlen);
                ok = true;
              } else if (status == 2) {
                break;                                          
              }
            }
          }
        }
      if (!ok) {                                                
        DisconnectNamedPipe(c.pipe); CloseHandle(c.pipe);
        c.pipe = nullptr; c.hello = false;
      }
    }
  }
  if (ok) out = std::move(resp);
  LeaveCriticalSection(&c.cs);
  return ok;
}

}  

static bool VisionScanImage(const unsigned char* img, int len, std::string& out) {
  if (len <= 0) return false;
  EnsureVisionServer();          
  return VisPipeCall(1, VISOP_SCAN_IMAGE, img, len, std::string(), out);
}

static bool VisionRecognize(const unsigned char* img, int len, int size, bool nosnap,
                            const std::string& recogJson, std::string& out) {
  if (len <= 0) return false;
  EnsureVisionServer();
  // ★ 2026-10-19：algo=sync ⇒ 服务端走「自制定位(findBoardRects+refineGrid) + 同步读子」，
  //   并带上识图窗那 5 项阈值（服务端 forceSync 使单帧图片也直接进 sync 分支）。
  //   这正是「拖动识别参数，识别效果应随之变化」的数据通路。
  std::string recog = recogJson.empty() ? VisRecogJson() : recogJson;
  std::string extra = std::string("{\"size\":") + std::to_string(size) +
                      (nosnap ? ",\"nosnap\":true" : "") +
                      ",\"algo\":\"sync\",\"recog\":" + recog + "}";
  return VisPipeCall(0, VISOP_RECOGNIZE, img, len, extra, out);
}

static bool VisScanCall(const std::string& body, std::string& out) {
  VisPipeCtx& c = g_vpipe[1];                           
  EnterCriticalSection(&c.cs);
  bool ok = false;
  std::string resp;
  for (int attempt = 0; attempt < 2 && !ok; ++attempt) {  
    if (!c.pipe && !VisPipeConnect(c, 1)) break;          
    
    std::vector<unsigned char> pl;
    unsigned blen = (unsigned)body.size();
    pl.insert(pl.end(), (unsigned char*)&blen, (unsigned char*)&blen + 4);
    if (blen) pl.insert(pl.end(), body.begin(), body.end());
    unsigned char hdr[12];
    if (VisPipeFrame(c.pipe, VISOP_SCAN, pl.data(), (unsigned)pl.size(), true) &&
        VisPipeIo(c.pipe, hdr, 12, false) && memcmp(hdr, "GBV1", 4) == 0) {
      unsigned plen = 0;
      memcpy(&plen, hdr + 8, 4);
      if (plen >= 8 && plen <= 64u * 1024 * 1024) {
        std::vector<unsigned char> rp(plen);
        if (VisPipeIo(c.pipe, rp.data(), plen, false)) {
          unsigned status = 0, rlen = 0;
          memcpy(&status, rp.data(), 4);
          memcpy(&rlen, rp.data() + 4, 4);
          if (status == 0 && rlen + 8 == plen) {
            resp.assign((const char*)rp.data() + 8, rlen);
            ok = true;
          } else if (status == 2) {
            break;                                       
          }
        }
      }
    }
    if (!ok) {                                           
      DisconnectNamedPipe(c.pipe); CloseHandle(c.pipe);
      c.pipe = nullptr; c.hello = false;
    }
  }
  if (ok) out = std::move(resp);
  LeaveCriticalSection(&c.cs);
  return ok;
}

// ★★ 2026-10-19（用户：「自动吸附找盘……没有用连线的自动吸附棋盘的那套算法」）：
//   「服务端抓屏识别」的宿主侧入口 —— 走 op=SCAN 命名管道，把「整屏抓图 + 找盘 + 读子」全部交给
//   视觉服务进程（与连线窗自动找盘同一条路），宿主这边不再自己抓屏、不再传整屏 PNG。
//   连不上服务时 VisScanCall 内部会尝试拉起服务并重试一次；真拿不到就返回 false，
//   调用方按原逻辑回退图像路径。
static bool VisionScanScreen(const std::string& body, std::string& out) {
  // ★ 2026-10-19 修复：VisScanCall/VisPipeConnect 自己**不会**拉起服务进程；以前这里没先
  //   EnsureVisionServer，服务没被预热时快车道必然连不上 ⇒ 静默回退到「宿主抓整屏 PNG +
  //   兜底起独立进程」的老慢路径（这正是「自动吸附/识别非常缓慢」的直接原因）。
  EnsureVisionServer();
  return VisScanCall(body, out);
}

static bool VisionServiceUp(int idx) {
  idx &= 1;
  VisPipeCtx& c = g_vpipe[idx];
  EnterCriticalSection(&c.cs);
  bool ok = (c.pipe && c.hello) ? true : VisPipeConnect(c, idx);
  LeaveCriticalSection(&c.cs);
  return ok;
}

static void OpenImageFromPage(const std::string& s) {
  std::string data = JsonStrAfter(s, "\"data\":");
  std::string ext = JsonStrAfter(s, "\"ext\":");
  int idx = JsonIntAfter(s, "\"idx\":");
  for (size_t i = 0; i < ext.size(); i++) {         
    char ch = ext[i];
    if (ch >= 'A' && ch <= 'Z') ext[i] = (char)(ch - 'A' + 'a');
  }
  if (ext.empty() || ext.size() > 5 ||
      ext.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") != std::string::npos) {
    ext = "png";                                  
  }
  size_t p = data.find("base64,");
  std::vector<unsigned char> bytes = (p == std::string::npos)
      ? std::vector<unsigned char>() : Base64Decode(data.substr(p + 7));
  if (bytes.empty()) { LogMsg("[vis] openImage: empty payload - nothing to open"); return; }

  wchar_t tmp[MAX_PATH] = { 0 };
  GetTempPathW(MAX_PATH, tmp);
  std::wstring path = Join(std::wstring(tmp), L"gbvis-open-" +
                           std::to_wstring(idx > 0 ? idx : 1) + L"." + Utf8ToWide(ext));
  {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) { LogMsg("[vis] openImage: cannot write " + WideToUtf8(path)); return; }
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
  }
  if (TestFlag("GB_TEST_OPEN_IMAGE")) {
    LogMsg("[vis] openImage (test hook: not launching) -> " + WideToUtf8(path) +
           " (" + std::to_string(bytes.size()) + " bytes)");
    return;
  }
  HINSTANCE r = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  LogMsg("[vis] openImage -> " + WideToUtf8(path) + " (" + std::to_string(bytes.size()) +
         " bytes), hinst=" + std::to_string((long long)(INT_PTR)r));
}

// ★ 2026-10-19（用户：「只识别了棋盘，常常不会自动识别棋子」）：
//   把识别结果的关键事实打进日志 —— 光看 "recognize ok (244 bytes)" 根本分不清是
//   「没找到盘」还是「找到盘但 0 颗子」还是「sync 松阈值/自适应兜底救回来了」。
//   只做字符串扫，不解析整份 JSON（这里不需要严谨）。
static int VisCountArrObjs(const std::string& s, const char* key) {
  size_t p = s.find(key);
  if (p == std::string::npos) return -1;
  p = s.find('[', p);
  if (p == std::string::npos) return -1;
  size_t q = s.find(']', p);
  if (q == std::string::npos) return -1;
  int n = 0;
  for (size_t i = p; i < q; ++i) if (s[i] == '{') ++n;
  return n;
}

static std::string VisResultSummary(const std::string& js) {
  std::string out = " found=";
  out += (js.find("\"found\": true") != std::string::npos ||
          js.find("\"found\":true") != std::string::npos) ? "1" : "0";
  out += " black=" + std::to_string(VisCountArrObjs(js, "\"black\""));
  out += " white=" + std::to_string(VisCountArrObjs(js, "\"white\""));
  std::string rd = JsonStrAfter(js, "\"reader\":");
  if (!rd.empty()) out += " reader=" + rd;
  if (js.find("\"sync_relaxed\"") != std::string::npos) {
    out += " relax=";
    std::string rb = JsonStrAfter(js, "\"sync_relax_black\":");
    std::string rw = JsonStrAfter(js, "\"sync_relax_white\":");
    out += rb.empty() ? "-" : rb;
    out += "/";
    out += rw.empty() ? "-" : rw;
  }
  if (js.find("\"sync_fallback\"") != std::string::npos) {
    std::string fbv = JsonStrAfter(js, "\"sync_fallback\":");
    out += " fallback=" + (fbv.empty() ? std::string("1") : fbv);
  }
  if (out.find("found=0") != std::string::npos) {
    std::string why = JsonStrAfter(js, "\"reason\":");
    if (!why.empty()) out += " reason=" + why;
  }
  return out;
}

static void RunVisionForVis(std::string req) {
  std::string mode = JsonStrAfter(req, "\"mode\":");
  std::string data = JsonStrAfter(req, "\"data\":");
  std::string seq = JsonStrAfter(req, "\"seq\":");     
  
  std::string nosnap = JsonStrAfter(req, "\"nosnap\":");
  bool skipSnap = nosnap.find("true") != std::string::npos;
  for (size_t i = 0; i < seq.size();) {                
    if (seq[i] < '0' || seq[i] > '9') { seq.erase(i, 1); continue; }
    ++i;
  }
  if (mode != "scan") mode = "image";

  // ★ 2026-10-19（用户：「拖动和改变这 5 个识别参数的参数值，并没有任何变化」）：
  //   识图识别参数（5 项）原本根本没随识别请求下发 —— 宿主这边 extra 只有 size/nosnap，
  //   服务端走的就是旧的 recognize()（暗细节投影那套），阈值对它天然无效。
  //   现在把 5 项随请求下发（缺省回落宿主当前权威值），服务端按 algo=sync + forceSync 走同步读子。
  int rcGray     = JsonIntAfter(req, "\"gray_offset\":");
  int rcBlackOff = JsonIntAfter(req, "\"black_offset\":");
  int rcWhiteOff = JsonIntAfter(req, "\"white_offset\":");
  int rcBlackPct = JsonIntAfter(req, "\"black_percent\":");
  int rcWhitePct = JsonIntAfter(req, "\"white_percent\":");
  if (rcGray < 0)     rcGray     = g_visRcGray.load();
  if (rcBlackOff < 0) rcBlackOff = g_visRcBlackOff.load();
  if (rcWhiteOff < 0) rcWhiteOff = g_visRcWhiteOff.load();
  if (rcBlackPct < 0) rcBlackPct = g_visRcBlackPct.load();
  if (rcWhitePct < 0) rcWhitePct = g_visRcWhitePct.load();
  ClampVisRecog(rcGray, rcBlackOff, rcWhiteOff, rcBlackPct, rcWhitePct);
  std::string recogJson = std::string("{\"gray_offset\":") + std::to_string(rcGray) +
      ",\"black_offset\":" + std::to_string(rcBlackOff) +
      ",\"white_offset\":" + std::to_string(rcWhiteOff) +
      ",\"black_percent\":" + std::to_string(rcBlackPct) +
      ",\"white_percent\":" + std::to_string(rcWhitePct) + "}";

  LogMsg("[vis] recognize requested (mode=" + mode + ", payload=" +
         std::to_string(data.size()) + " chars)");
  auto fail = [&](const char* why) {
    LogMsg(std::string("[vis] recognize failed: ") + why);
    PostToVisPageSoon("{\"type\":\"visResult\",\"mode\":\"" + mode +
                      (seq.empty() ? "" : "\",\"seq\":" + seq) +
                      "\",\"result\":{\"ok\":false,\"err\":" + JsonQuote(why) + "}}");
  };
  size_t p = data.find("base64,");
  std::vector<unsigned char> bytes = (p == std::string::npos)
      ? std::vector<unsigned char>() : Base64Decode(data.substr(p + 7));
  if (bytes.empty()) { fail("bad image payload"); return; }

  // Modular test model: when a Vision-test folder ships with the app, the
  // Vision capture window runs the one-shot Zhizi test recognizer; the shared
  // formal service (used by board-linking) is never touched. Deleting the
  // Vision-test folder restores the formal model for the capture window too.
  std::wstring testExe = Join(Join(ExeDir(), L"Vision-test"), L"GomokuVision.exe");
  bool visUseTest = FileExists(testExe);
  {
    int boardSize = JsonIntAfter(req, "\"size\":");
    if (boardSize < 9 || boardSize > 25) boardSize = 15;   
    std::string js;
    bool ok = false;
    if (!visUseTest) {
      // ★ 2026-10-13：识别进程已**去 HTTP**，这里走的就是纯命名管道 + 共享内存
      //   （VisPipeCall ⇒ \\.\pipe\GomokuVision.<role>.<port>），没有任何 TCP。
      if (mode == "scan")
        ok = VisionScanImage(bytes.data(), (int)bytes.size(), js);
      else
        ok = VisionRecognize(bytes.data(), (int)bytes.size(), boardSize, skipSnap, recogJson, js);
      if (ok) {
        LogMsg("[vis] recognize ok (" + std::to_string(js.size()) +
               " bytes, mode=" + mode + ", via named pipe)" + VisResultSummary(js));
        PostToVisPageSoon("{\"type\":\"visResult\",\"mode\":\"" + mode +
                          (seq.empty() ? "" : "\",\"seq\":" + seq) +
                          "\",\"result\":" + js + "}");
        return;
      }
      LogMsg("[vis] pipe path unavailable -> spawn fallback (mode=" + mode + ")");
    } else {
      LogMsg("[vis] test model (Vision-test) -> one-shot CLI recognizer (mode=" + mode + ")");
    }
  }

  std::wstring exe = visUseTest ? testExe : FindVisionExe();
  if (exe.empty()) { fail("GomokuVision.exe not found"); return; }

  wchar_t tmp[MAX_PATH] = { 0 };
  GetTempPathW(MAX_PATH, tmp);
  std::wstring imgPath = Join(std::wstring(tmp), L"gbvis-req.img");
  {
    FILE* f = _wfopen(imgPath.c_str(), L"wb");
    if (!f) { fail("cannot write temp image"); return; }
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
  }
  
  // ★ 2026-10-19：兜底起独立进程时也要带上 algo=sync + 5 项阈值，否则唯一还能走通道的场景下
  //   识图又退回旧算法（阈值再次失效）。
  std::wstring syncArgs;
  if (mode != "scan") {
    syncArgs = L" --algo sync --recog " +
               Utf8ToWide(std::to_string(rcGray) + "," + std::to_string(rcBlackOff) + "," +
                          std::to_string(rcWhiteOff) + "," + std::to_string(rcBlackPct) + "," +
                          std::to_string(rcWhitePct));
  }
  std::wstring cmd = L"\"" + exe + L"\" " +
                     (mode == "scan" ? L"--scan-image" : L"--recognize-image") +
                     L" \"" + imgPath + L"\"" + (skipSnap ? L" --nosnap" : L"") + syncArgs;
  std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end()); cmdBuf.push_back(0);

  SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) { fail("cannot create pipe"); return; }
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);     
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr;
  si.hStdError = wr;
  PROCESS_INFORMATION pi = {};
  std::wstring cwd = exe.substr(0, exe.find_last_of(L"\\/"));
  
  BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi);
  CloseHandle(wr);                       
  if (!ok) { CloseHandle(rd); fail("spawn failed"); DeleteFileW(imgPath.c_str()); return; }
  CloseHandle(pi.hThread);
  ApplyVisionAffinity(pi.hProcess);      
  if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);   
  ChildTrack(pi.hProcess);               

  std::string out;
  char buf[4096];
  DWORD n = 0;
  while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
  CloseHandle(rd);
  
  DWORD wait = WaitForSingleObject(pi.hProcess, 10000);
  if (wait != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
  CloseHandle(pi.hProcess);
  DeleteFileW(imgPath.c_str());

  while (!out.empty() && (unsigned char)out.front() <= ' ') out.erase(0, 1);
  while (!out.empty() && (unsigned char)out.back() <= ' ') out.pop_back();
  if (out.empty() || out[0] != '{') { fail("no recognizer output"); return; }
  LogMsg("[vis] recognize ok (" + std::to_string(out.size()) + " bytes, mode=" + mode + ")" +
         VisResultSummary(out));
  PostToVisPageSoon("{\"type\":\"visResult\",\"mode\":\"" + mode +
                    (seq.empty() ? "" : "\",\"seq\":" + seq) +
                    "\",\"result\":" + out + "}");
}

static std::wstring ReadClipboardText() {
  std::wstring out;
  if (!OpenClipboard(g_hwnd)) return out;
  HANDLE h = GetClipboardData(CF_UNICODETEXT);
  if (h) {
    const wchar_t* p = (const wchar_t*)GlobalLock(h);
    if (p) { out.assign(p); GlobalUnlock(h); }
  }
  CloseClipboard();
  return out;
}
static bool WriteClipboardText(const std::wstring& s) {
  if (!OpenClipboard(g_hwnd)) return false;
  EmptyClipboard();
  size_t bytes = (s.size() + 1) * sizeof(wchar_t);
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!h) { CloseClipboard(); return false; }
  void* p = GlobalLock(h);
  if (p) {
    memcpy(p, s.c_str(), bytes);
    GlobalUnlock(h);
    SetClipboardData(CF_UNICODETEXT, h);
  } else {
    GlobalFree(h);
  }
  CloseClipboard();
  return true;
}

static int        g_threads = 0;      
static int        g_hashMB = 0;       

static int        g_linkCfgSeen = 0;

// ★ 2026-10-17：**启动时缓存**。任何后续的亲和性/Job 限制都不许再影响这个值 ——
//   否则「钉核 → 核心数变小 → 下拉上限变小 → 用户偏好被夹」这条链会反复出现。
static int CpuCount() {
  static const int cached = [] {
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
  }();
  return cached > 0 ? cached : 4;
}
// 下拉上限 = 本机逻辑核 - 保留核（留给 WebView2 渲染 / 识图 / sub·fwd 车道）。
// ★★ 2026-10-18（用户：「线程多于核心…我想要的是多线程齐心协力，而不是相互打架」）：
//   线程上限从 `n-4` 改成 `n-2`。
//   实测（AMD 8845H 8 核 16 线程，交错 4 轮、冷启动、同盘面 2000ms）：
//     t=10 → nps 2879K / 节点 5587K / depth 22      ← 最差（既没占满 SMT，又被乱撒）
//     t=12 → nps 3567K / 节点 7117K / depth 29      ← 旧上限
//     t=16 → nps 4678K / 节点 9334K / depth 30      ← 满 SMT 最快
//   规律是**两端快、中间慢**：8~12 这种「比物理核多、又没多到占满」的组合才会打架
//   （Windows 会把两个线程放到同一个物理核的两个 SMT 兄弟上，而这台机器兄弟是相邻
//   编号 (0,1)(2,3)…，于是两个线程抢同一组执行单元）。占满全部逻辑核时每个 LP 一个
//   线程，天然一一对应，反而最快。
//   这里留 2 个逻辑核给识别（GomokuVision 钉住 + HIGH，见 CpuSplit），
//   本机 ⇒ 14 线程；4 核小机器 ⇒ 3 线程 + 1 核识别。
static int PhysCoreCount();   // 实现在本文件后段（2295 处先用到）

static int ThreadsMax() {
  // ★★ 2026-10-24（用户：「不仅灵活运用我的开发机，更要动态适应用户机」）：
  //   口径改成**物理核数**，由每台机器自己算出来，不再写死「逻辑核-2」。
  //   依据：`tools/thread-quality.js` 在 8P/16L 上系统实测 —— T1 23.3 / T4 23.3 / T8 24.3 /
  //   T14 22.3，且 1→8 线程落点 100% 相同。逻辑核（SMT 兄弟）对 AB 搜索是**负收益**：
  //   两个线程抢同一组执行单元，主线程反而变慢，还更容易顶到温度墙降频。
  //   · 4 核轻薄本 → 4（留 SMT 不用，但不砍物理核）
  //   · 8 核主流 → 8      · 16 核工作站 → 16
  //   · 2 核以下 → 原样（别把唯一的两条路也砍掉）
  int phys = PhysCoreCount();
  int n    = CpuCount();
  if (n < 1) n = 1;
  int t = (phys >= 2) ? phys : ((n >= 8) ? n - 2 : n - 1);
  if (t < 1) t = 1;
  if (t > n) t = n;
  return t;
}
// ★ 2026-10-16（用户：「速度和深度远不如 4.39」「智力不算高」）：首启默认 = **下拉上限**。
//   v4.39 的口径就是 `S.cores = maxT`（本机 16 逻辑核 ⇒ 12 线程）；后来一段版本把工厂默认
//   改成「一半核」并被 save() 固化进 localStorage ⇒ **所有老用户**都只跑一半线程（6 个），
//   引擎明明加载成功、nps/深度却只有 4.39 的一半。这里与 ThreadsMax() 同口径；界面 host
//   消息与 CalcFullThreads() 的自动档共用同一个值（默认口径只在宿主，界面不自带默认）。
//   **勿改回「一半核」**。
static int ThreadsDefault() {
  int t = ThreadsMax();
  if (t < 1) t = 1;
  return t;
}

static int MemTotalMB() {
  MEMORYSTATUSEX ms;
  ms.dwLength = sizeof(ms);
  if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys > 0)
    return (int)(ms.ullTotalPhys / (1024ull * 1024ull));
  return 4096;
}

static int MemAvailMB() {
  MEMORYSTATUSEX ms;
  ms.dwLength = sizeof(ms);
  if (GlobalMemoryStatusEx(&ms) && ms.ullAvailPhys > 0)
    return (int)(ms.ullAvailPhys / (1024ull * 1024ull));
  return MemTotalMB() / 2;
}

// ★ 2026-10-18（用户：「在不同电脑上要有不同的适应芯片逻辑…要根据英特尔和 AMD 官方写
//   一些优化辅助亲和加速补丁」）：厂商与混合架构探测。
//   · VendorIdentifier = "AuthenticAMD" / "GenuineIntel"（注册表，和 CpuName 同源，零成本）
//   · 混合架构（Intel 12 代+ P/E 核）= 物理核里存在两种 EfficiencyClass。
//     AMD 全为 0 ⇒ 永远判成非混合，走的仍是「每核一个 primary」的策略。
static std::string CpuVendor() {
  HKEY hk = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                    0, KEY_READ, &hk) != ERROR_SUCCESS || !hk)
    return "";
  wchar_t buf[64] = {0};
  DWORD sz = sizeof(buf), ty = 0;
  LONG r = RegQueryValueExW(hk, L"VendorIdentifier", nullptr, &ty, (LPBYTE)buf, &sz);
  RegCloseKey(hk);
  if (r != ERROR_SUCCESS || ty != REG_SZ) return "";
  return WideToUtf8(buf);
}

static bool HasHybridCores() {
  DWORD len = 0;
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len) || !len) return false;
  std::vector<char> buf(len);
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
          (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len)) return false;
  DWORD o = 0;
  bool perf = false, eff = false;
  while (o < len) {
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX pi =
        (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + o);
    if (pi->Relationship == RelationProcessorCore) {
      if (pi->Processor.EfficiencyClass == 0) perf = true; else eff = true;
    }
    o += pi->Size;
  }
  return perf && eff;
}

static std::string CpuName() {
  HKEY hk = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                    0, KEY_READ, &hk) != ERROR_SUCCESS || !hk)
    return "";
  wchar_t buf[256] = {0};
  DWORD sz = sizeof(buf), ty = 0;
  LONG r = RegQueryValueExW(hk, L"ProcessorNameString", nullptr, &ty, (LPBYTE)buf, &sz);
  RegCloseKey(hk);
  if (r != ERROR_SUCCESS || ty != REG_SZ) return "";
  std::string v = WideToUtf8(buf);
  
  std::string o;
  bool sp = false;
  for (size_t i = 0; i < v.size(); ++i) {
    unsigned char c = (unsigned char)v[i];
    if (c == ' ' || c == '	') { if (!o.empty()) sp = true; continue; }
    if (sp) { o += ' '; sp = false; }
    o += (char)c;
  }
  return o;
}

static int PhysCoreCount() {
  DWORD len = 0;
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len)) {
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return (int)std::thread::hardware_concurrency();
  }
  std::vector<char> buf(len ? len : 4096);
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                        (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len))
    return (int)std::thread::hardware_concurrency();
  int cores = 0;
  DWORD off = 0;
  while (off < len) {
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX pi =
        (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
    if (pi->Relationship == RelationProcessorCore) cores++;
    off += pi->Size;
  }
  if (cores < 1) return (int)std::thread::hardware_concurrency();
  return cores;
}

struct CpuSplitInfo {
  bool active = false;
  int  physCores = 0, engCores = 0, visCores = 0;   
  int  engCPUs = 0,  visCPUs = 0;                   
  DWORD_PTR engMask = 0, visMask = 0;
  std::string why;                                  
};

// ★ 2026-10-17：提前声明（CpuSplit 要按「当前用户线程数」算掩码）
static std::atomic<int> g_usrThreads{0};             // 用户线程偏好（0=自动=满核）


static int PopCountPtr(DWORD_PTR v) {
  int n = 0;
  while (v) { v &= (v - 1); ++n; }
  return n;
}

static const CpuSplitInfo& CpuSplit() {
  // ★★ 2026-10-17：掩码按「当前用户线程数」**重算**（不是进程级一次定死）。
  //   用户把线程从 12 调到 8，视觉就应多拿 4 个逻辑核 —— 这是「空余的才给识别」
  //   这句话的字面实现：引擎要几个核就给几个，剩下的全归识别。
  // ★★ 固定按 ThreadsMax() 算（**不随用户当前线程数变**）：掩码必须在进程生命周期内一致，
  //   否则「引擎 spawn 时用一套掩码、宿主 pin 时又用另一套」→ 两组重叠，反而互相抢核。
  //   用户把线程调小时，引擎线程少、掩码不变，视觉的核依旧独占 —— 识别不会变慢。
  int needWanted = ThreadsMax();
  static std::mutex csMu;
  static CpuSplitInfo cache;
  static int cachedNeed = -1;
  std::lock_guard<std::mutex> lk(csMu);
  if (cachedNeed == needWanted) return cache;
  cachedNeed = needWanted;
  cache = [needWanted] {
    CpuSplitInfo inf;
    
    // ★★ 2026-10-17（用户：「为视觉引擎开一些没用到的核心，识别速度永远不会变慢」）：
    //   **默认启用**核隔离 —— 识别进程钉在「引擎线程上限之外」的物理核上，
    //   引擎 12 线程占 12 个逻辑核，识别独占剩下 4 个 ⇒ 搜索再久也挤不到识别。
    //   关闭方式：GB_CPU_SPLIT=0（或排障开关 GB_NO_CPU_SPLIT=1）。
    char on[4] = {0};
    bool want = true;
    if (GetEnvironmentVariableA("GB_CPU_SPLIT", on, sizeof(on)) > 0 && on[0] == '0') want = false;
    if (!want) {
      inf.physCores = PhysCoreCount();
      inf.why = "默认不切核（引擎/识别/宿主共用全部逻辑核；GB_CPU_SPLIT=1 可开启隔离）";
      return inf;
    }
    char off[4] = {0};
    if (GetEnvironmentVariableA("GB_NO_CPU_SPLIT", off, sizeof(off)) > 0 && off[0] == '1') {
      inf.physCores = PhysCoreCount();
      inf.why = "GB_NO_CPU_SPLIT=1（排障杀开关）";
      return inf;
    }
    
    // ★ 2026-10-18：每个物理核连同它的 EfficiencyClass 一起收。
    //   Intel 12 代以后的混合架构（P-core + E-core）靠这个字段区分：
    //   EfficiencyClass 越小越偏性能核。AMD 全为 0，等价于「都是 P 核」。
    //   排序后先给 P 核发线程 —— 否则引擎线程会被 Windows 摊到 E 核上，
    //   同等线程数下整数吞吐差一截（E 核无 SMT、频率与缓冲都更弱）。
    struct CoreGrp { DWORD_PTR mask; int eff; };
    std::vector<CoreGrp> groups;
    DWORD len = 0;
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) len = 0;
    if (len) {
      std::vector<char> buf(len);
      if (GetLogicalProcessorInformationEx(RelationProcessorCore,
              (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len)) {
        DWORD o = 0;
        while (o < len) {
          PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX pi =
              (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + o);
          if (pi->Relationship == RelationProcessorCore && pi->Processor.GroupCount >= 1) {
            if (pi->Processor.GroupCount > 1 || pi->Processor.GroupMask[0].Group != 0) {
              inf.physCores = (int)groups.size();
              inf.why = "多处理器组（>64 逻辑核）";
              return inf;
            }
            groups.push_back({ (DWORD_PTR)pi->Processor.GroupMask[0].Mask,
                               (int)pi->Processor.EfficiencyClass });
          }
          o += pi->Size;
        }
      }
    }
    int n = (int)groups.size();
    if (n < 1) {
      inf.physCores = PhysCoreCount();
      inf.why = "取不到物理核拓扑";
      return inf;
    }
    inf.physCores = n;
    if (n < 3) { inf.why = "物理核不足 3 个"; return inf; }
    std::stable_sort(groups.begin(), groups.end(),
                     [](const CoreGrp& a, const CoreGrp& b) { return a.eff < b.eff; });
    // ★★ 2026-10-17 二次修正（用户：「用户选了 8 个线程，系统里还有十多条线程，把空余的
    //   分几个给视觉，不要耽搁正式的线程」）：上一版按 2/3 分**物理核**，本机只给引擎
    //   6 个物理核（12 逻辑），比全核少 2 个物理核 —— 实测 nps 明显下降。
    //   正确口径：引擎优先拿「每个物理核的第 1 个逻辑 CPU(primary)」，N 个搜索线程各占
    //   一个物理核、互不共享执行单元；线程上限之外的空闲逻辑 CPU（超线程兄弟 + 多余核）
    //   才归识别。引擎可用逻辑核**永远 ≥ 线程上限**，一个正式线程都不耽搁。
    int need = needWanted;                    // 由外层按「当前用户线程数」决定
    if (need < 1) need = 1;
    if (need > (int)CpuCount() - 1) {         // 至少要给识别留 1 个逻辑核
      inf.why = "逻辑核不足（引擎上限 " + std::to_string(need) + " / 共 " +
                std::to_string(CpuCount()) + "）";
      return inf;
    }
    DWORD_PTR all = 0, em = 0;
    for (int i = 0; i < n; ++i) all |= groups[i].mask;
    for (int i = 0; i < n; ++i) {              // 1) 每物理核 1 个 primary（已按 eff 排序 ⇒ 先 P 核）
      DWORD_PTR g = groups[i].mask;
      if (g) em |= (g & (~g + 1));
    }
    int have = PopCountPtr(em);
    for (int i = 0; i < n && have < need; ++i) {   // 2) 不够补超线程兄弟
      DWORD_PTR rest = groups[i].mask & ~em;
      while (rest && have < need) {
        DWORD_PTR low = rest & (~rest + 1);
        em |= low; rest &= ~low; ++have;
      }
    }
    DWORD_PTR vm = all & ~em;                  // 3) 其余全归识别
    inf.engMask = em; inf.visMask = vm;
    inf.engCPUs = PopCountPtr(em); inf.visCPUs = PopCountPtr(vm);
    inf.engCores = inf.engCPUs; inf.visCores = inf.visCPUs;
    inf.active = (em != 0 && vm != 0 && inf.engCPUs >= need && inf.visCPUs >= 1);
    if (!inf.active) inf.why = "掩码构造失败（引擎 " + std::to_string(inf.engCPUs) +
                               " 逻辑 / 识别 " + std::to_string(inf.visCPUs) + " 逻辑）";
    return inf;
  }();
  return cache;
}

static void LogCpuSplitOnce() {
  static std::atomic<bool> told{ false };
  bool e = false;
  if (!told.compare_exchange_strong(e, true)) return;
  const CpuSplitInfo& c = CpuSplit();
  if (!c.active) {
    LogMsg("[cpu] 核配额未启用（" + (c.why.empty() ? std::string("拓扑不支持") : c.why) +
           "）—— 引擎/识别共享全部 " + std::to_string(CpuCount()) + " 逻辑核");
    return;
  }
  char b[256];
  snprintf(b, sizeof(b),
           "[cpu] 核配额启用：物理核 %d → 引擎 %d 核/%d 逻辑(0x%llX) | 识别 %d 核/%d 逻辑(0x%llX)",
           c.physCores, c.engCores, c.engCPUs, (unsigned long long)c.engMask,
           c.visCores, c.visCPUs, (unsigned long long)c.visMask);
  LogMsg(b);
}

// 引擎不钉核 ⇒ 预算按全部逻辑核算（0 = 不限制），线程上限仍由 ThreadsMax() 管
static int EngThreadCPUs() { return 0; }

static std::string CpuSplitStatusJson() {
  const CpuSplitInfo& c = CpuSplit();
  return std::string(",\"cpuSplit\":") + (c.active ? "true" : "false") +
         ",\"engCores\":" + std::to_string(c.engCores) +
         ",\"visCores\":" + std::to_string(c.visCores) +
         ",\"engineCPUs\":" + std::to_string(EngThreadCPUs()) +
         ",\"visionCPUs\":" + std::to_string(c.visCPUs);
}

static bool ApplyEngineAffinity(HANDLE h) {
  if (!h) return false;
  // ★★ 2026-10-18 两轮实测后的最终结论（勿再翻转，详见下方 23:15 真机记录）：
  //   离线基准（powershell 子进程设亲和性）曾显示钉 engMask 快 15%，但那是测量噪声；
  //   真机日志：钉 engMask(14 LP) = 644K nps，全核自由调度 = 3392K nps（5 倍）。
  //   与 2026-10-17 的结论一致：**引擎在全部逻辑核上自由调度最快**，
  //   Windows 调度器自己就会摊开线程；识别仍钉 visMask + HIGH，互不拖累。
  const CpuSplitInfo& c = CpuSplit();
  if (!c.active) return false;
  LogCpuSplitOnce();
  // ★★ 真机实测（2026-10-18 23:15）：把引擎钉到 engMask(0x5FFF, 14 LP) 反而**大幅变慢** ——
  //   同一盘面、同样 2000ms：钉核 Speed 644K / Node 1272K，不钉核 3392K / Node 6690K（5 倍）。
  //   离线基准里「钉核更快」是测量噪声（设亲和性用 powershell 子进程、且各轮之间受功耗墙
  //   漂移影响），真机日志才是可信口径。
  //   结论（与 2026-10-17 的结论一致，勿再翻转）：**引擎在全部逻辑核上自由调度最快**。
  //   Windows 调度器本身就会把线程摊到不同物理核；人为收窄反而压掉了 SMT 的吞吐补偿。
  //   识别进程仍然钉在 visMask（本机 2 个 LP）+ HIGH —— 它占的是「配额里的富余核」，
  //   引擎线程即便跑进那些核，也只会被短促的识别扫描打断一下，代价远小于自断 2 个核。
  SYSTEM_INFO si; GetNativeSystemInfo(&si);
  DWORD_PTR all = (DWORD_PTR)si.dwActiveProcessorMask;
  if (!SetProcessAffinityMask(h, all)) {
    LogMsg("[cpu] 引擎解钉失败 err=" + std::to_string(GetLastError()));
    return false;
  }
  LogMsg("[cpu] 引擎可调度 " + std::to_string(PopCountPtr(all)) +
         " 逻辑核（解回全核，识别独占 " + std::to_string(c.visCPUs) + " 核）");
  return true;
}

static bool ApplyVisionAffinity(HANDLE h) {
  if (!h) return false;
  const CpuSplitInfo& c = CpuSplit();
  if (!c.active) return false;
  LogCpuSplitOnce();
  if (!SetProcessAffinityMask(h, c.visMask)) {
    LogMsg("[cpu] 识别进程钉核失败 err=" + std::to_string(GetLastError()));
    return false;
  }
  return true;
}

static std::atomic<bool> g_hostPinned{ false };
static void CpuSplitPinHost() {
  // ★★ 2026-10-17（「选 1 就触发一连串 bug」的真凶）：**宿主绝对不能钉核**。
  //   钉了之后 GetNativeSystemInfo()/GetLogicalProcessorInformationEx() 只返回
  //   亲和性内的 4 个逻辑核 ⇒ host 消息变成 cpu=4 / threadsMax=3 / memMB=4096(兜底)
  //   ⇒ 页面下拉上限缩成 3，用户选的线程数被反复夹取改写（看起来就是「改了又恢复」）。
  //   只钉识别进程就够了（它才是要被保护的一方）。
  const CpuSplitInfo& c = CpuSplit();
  if (!c.active) return;
  LogCpuSplitOnce();
  LogMsg("[cpu] 宿主不钉核（保持系统探测准确），识别独占 " + std::to_string(c.visCPUs) + " 逻辑核");
  return;
  if (!SetProcessAffinityMask(GetCurrentProcess(), c.visMask)) {
    g_hostPinned.store(false);
    LogMsg("[cpu] 宿主钉核失败 err=" + std::to_string(GetLastError()) + "（引擎配额不受影响）");
    return;
  }
  LogMsg("[cpu] 宿主（截屏/识别驱动/消息泵）已归入识别核组 " + std::to_string(c.visCPUs) + " 逻辑核");
  LogCpuSplitOnce();
}
static void CpuSplitUnpinHost() {
  bool e = true;
  if (!g_hostPinned.compare_exchange_strong(e, false)) return;
  SYSTEM_INFO si; GetNativeSystemInfo(&si);
  if (SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)si.dwActiveProcessorMask))
    LogMsg("[cpu] 原生引擎不可用 → 宿主解钉，回到全核调度");
}

static LONG RegDword(HKEY root, const wchar_t* sub, const wchar_t* val, DWORD& out) {
  HKEY hk = nullptr;
  if (RegOpenKeyExW(root, sub, 0, KEY_READ, &hk) != ERROR_SUCCESS || !hk) return -1;
  DWORD v = 0, sz = sizeof(v), ty = 0;
  LONG r = RegQueryValueExW(hk, val, nullptr, &ty, (LPBYTE)&v, &sz);
  RegCloseKey(hk);
  if (r == ERROR_SUCCESS) out = v;
  return r;
}

// ★ 2026-10-16（性能）：渲染进程**保持 HIGH_PRIORITY_CLASS**。
//   实测教训（勿改）：把它降到 NORMAL 之后，页面**全程沉默** —— 宿主能 serve 出 calc.html /
//   calc.css / calc.js，但渲染进程再没有任何后续请求、也收不到 {type:'ready'}
//   （日志只有 `main page still silent ... re-navigating`），UI 本身是好的（同一份 calc.js
//   解密后在无头 Edge 里 readySent=true / 零异常）。也就是说：这里给的不只是「优先权」，
//   降到 NORMAL 会把 WebView2 渲染进程变成实质不可用。
//   给搜索线程让路的正确做法是**抬引擎**（spawn 后 ABOVE_NORMAL_PRIORITY_CLASS），
//   而不是压渲染进程。
static void BoostWebViewPriority() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  DWORD me = GetCurrentProcessId();
  PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
  int boosted = 0;
  if (Process32FirstW(snap, &pe)) {
    do {
      if (pe.th32ParentProcessID == me && wcscmp(pe.szExeFile, L"msedgewebview2.exe") == 0) {
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pe.th32ProcessID);
        if (h) {
          if (SetPriorityClass(h, HIGH_PRIORITY_CLASS)) boosted++;
          CloseHandle(h);
        }
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  if (boosted > 0) LogMsg("[ai] boosted " + std::to_string(boosted) + " webview child process(es) to High priority");
}

static std::string g_inboxSeen;

static void HttpHistoryServer() {
  WSADATA wsd;
  if (WSAStartup(MAKEWORD(2, 2), &wsd) != 0) { LogMsg("[inbox] WSAStartup failed"); return; }
  SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ls == INVALID_SOCKET) return;
  int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(8972);
  if (bind(ls, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(ls, 8) != 0) {
    LogMsg("[inbox] :8972 bind/listen failed (port busy?)");
    closesocket(ls);
    return;
  }
  LogMsg("[inbox] history endpoint listening on http://127.0.0.1:8972/history");
  while (g_running.load()) {
    SOCKET cs = accept(ls, nullptr, nullptr);
    if (cs == INVALID_SOCKET) break;
    std::string req;
    char buf[4096];
    int n = recv(cs, buf, sizeof(buf) - 1, 0);
    if (n > 0) { buf[n] = 0; req = buf; }
    
    size_t he = req.find("\r\n\r\n");
    std::string body;
    if (he != std::string::npos) body = req.substr(he + 4);
    size_t cl = std::string::npos;
    {
      std::string low = req;
      for (auto& c : low) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
      size_t p = low.find("content-length:");
      if (p != std::string::npos) cl = (size_t)atoi(low.c_str() + p + 15);
    }
    while (cl != std::string::npos && body.size() < cl) {
      int k = recv(cs, buf, sizeof(buf) - 1, 0);
      if (k <= 0) break;
      buf[k] = 0; body += buf;
    }
    std::string resp;
    if (req.rfind("POST", 0) == 0 && !body.empty()) {
      
      std::string msg = "{\"type\":\"historyInbox\",\"text\":";
      msg += JsonQuote(body);
      msg += "}";
      PostToPage(msg);
      LogMsg("[inbox] HTTP /history " + std::to_string(body.size()) + " bytes");
      resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
             "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: POST,OPTIONS\r\n"
             "Access-Control-Allow-Headers: Content-Type\r\n"
             "Access-Control-Allow-Private-Network: true\r\n"
             "Content-Length: 15\r\nConnection: close\r\n\r\n{\"ok\":true}\n";
    } else if (req.rfind("OPTIONS", 0) == 0) {
      resp = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\n"
             "Access-Control-Allow-Methods: POST,OPTIONS\r\n"
             "Access-Control-Allow-Headers: Content-Type\r\n"
             "Access-Control-Allow-Private-Network: true\r\n"
             "Content-Length: 0\r\nConnection: close\r\n\r\n";
    } else {
      resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    }
    send(cs, resp.c_str(), (int)resp.size(), 0);
    closesocket(cs);
  }
  closesocket(ls);
  WSACleanup();
}

static void SendAll(SOCKET cs, const char* p, int n) {
  while (n > 0) {
    int k = send(cs, p, n, 0);
    if (k <= 0) return;
    p += k; n -= k;
  }
}

// ---- Job 对象（KILL_ON_JOB_CLOSE）----
// 引擎子进程 AssignProcessToJobObject 到此 Job：宿主退出（含被硬杀）时由内核回收，
// 不会留下孤儿 RapfiEngine 进程。
static void EnsureJob() {
  if (g_job) return;
  g_job = CreateJobObjectW(nullptr, nullptr);
  if (!g_job) return;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
  jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
    CloseHandle(g_job);
    g_job = nullptr;
  }
}
static void CloseJobHandle() { if (g_job) { CloseHandle(g_job); g_job = nullptr; } }

#if 0   
        
static const char* UiHttpMime(const std::string& p) {
  size_t dot = p.rfind('.');
  std::string e = (dot == std::string::npos) ? "" : p.substr(dot);
  for (auto& c : e) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
  if (e == ".html") return "text/html; charset=utf-8";
  if (e == ".js")   return "text/javascript; charset=utf-8";
  if (e == ".css")  return "text/css; charset=utf-8";
  if (e == ".wasm") return "application/wasm";
  return "application/octet-stream";
}

static void HttpUiServer() {
  WSADATA wsd;
  if (WSAStartup(MAKEWORD(2, 2), &wsd) != 0) { LogMsg("[ui-http] WSAStartup failed"); return; }
  SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ls == INVALID_SOCKET) return;
  int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  
  addr.sin_port = htons(0);
  if (bind(ls, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(ls, 8) != 0) {
    LogMsg("[ui-http] bind/listen failed (even port 0?!)");
    closesocket(ls);
    WSACleanup();
    return;
  }
  sockaddr_in bound = {};
  int blen = sizeof(bound);
  int port = 0;
  if (getsockname(ls, (sockaddr*)&bound, &blen) == 0) port = ntohs(bound.sin_port);
  g_uiPort.store(port > 0 ? port : 8965);
  LogMsg("[ui-http] UI+AI serving on http://127.0.0.1:" + std::to_string(g_uiPort.load()) +
         "/ (COOP/COEP on, port auto-assigned - no collision possible)");
  while (g_running.load()) {
    SOCKET cs = accept(ls, nullptr, nullptr);
    if (cs == INVALID_SOCKET) break;
    std::thread([cs] {
      std::string req;
      char buf[4096];
      for (;;) {
        int n = recv(cs, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { closesocket(cs); return; }
        buf[n] = 0; req += buf;
        if (req.find("\r\n\r\n") != std::string::npos) break;
        if (req.size() > 16384) break;
      }
      size_t sp1 = req.find(' ');
      size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : req.find(' ', sp1 + 1);
      std::string method = (sp1 != std::string::npos) ? req.substr(0, sp1) : "GET";
      std::string path = (sp1 != std::string::npos && sp2 != std::string::npos)
                             ? req.substr(sp1 + 1, sp2 - sp1 - 1) : "";
      std::string query;
      size_t q = path.find('?');
      if (q != std::string::npos) { query = path.substr(q + 1); path = path.substr(0, q); }

      std::string reqBody;
      {
        size_t he = req.find("\r\n\r\n");
        if (he != std::string::npos) {
          reqBody = req.substr(he + 4);
          size_t clPos = req.find("Content-Length:");
          if (clPos == std::string::npos) clPos = req.find("content-length:");
          size_t want = 0;
          if (clPos != std::string::npos) want = (size_t)strtoul(req.c_str() + clPos + 15, nullptr, 10);
          if (want > (size_t)4 << 20) want = (size_t)4 << 20;      
          if (reqBody.size() < want) {
            reqBody.reserve(want);
            while (reqBody.size() < want) {
              int n = recv(cs, buf, sizeof(buf) - 1, 0);
              if (n <= 0) break;
              reqBody.append(buf, n);
            }
          }
          if (reqBody.size() > want && clPos != std::string::npos) reqBody.resize(want);
        }
      }

      std::vector<uint8_t> body;
      const char* mime = "application/octet-stream";
      int status = 200; const char* statusText = "OK";
      std::string extra;

      if (!path.empty() && path[0] == '/') {
        if (path.rfind("/engine/", 0) == 0) {
          
          auto qget = [&](const char* key) -> std::string {
            std::string k = std::string(key) + "=";
            size_t p = query.find(k);
            if (p == std::string::npos) return "";
            size_t e2 = query.find('&', p);
            return query.substr(p + k.size(), e2 == std::string::npos ? std::string::npos : e2 - (p + k.size()));
          };
          std::string route = path.substr(8);
          std::string lane  = qget("lane");
          int idx = lane.empty() ? 0 : nativeeng::LaneIndex(lane);
          std::string payload;
          if (route == "status") {
            nativeeng::Probe();
            MEMORYSTATUSEX ms = {}; ms.dwLength = sizeof(ms);
            GlobalMemoryStatusEx(&ms);
            payload = std::string("{\"available\":") + (nativeeng::g_avail ? "true" : "false") +
                      ",\"variant\":" + JsonQuote(nativeeng::g_variant) +
                      ",\"cpus\":" + std::to_string((int)std::thread::hardware_concurrency()) +
                      ",\"physCores\":" + std::to_string(PhysCoreCount()) +
                      ",\"memAvailMB\":" + std::to_string(MemAvailMB()) +
                      ",\"memMB\":" + std::to_string((long long)(ms.ullTotalPhys / (1024 * 1024))) +
                      
                      ",\"engineThreads\":" + std::to_string(g_threads) +
                      ",\"engineHashMB\":" + std::to_string(g_hashMB) +
                      ",\"userThreads\":" + std::to_string(g_usrThreads.load()) +
                      ",\"userHashMB\":" + std::to_string(nativeeng::g_usrHashMB.load()) +
                      ",\"lanes\":[\"main\",\"sub\",\"fwd\"]}";
          } else if (idx < 0) {
            payload = "{\"error\":\"bad lane\"}";
          } else if (route == "cmd") {
            
            std::string text = reqBody;
            bool ok = nativeeng::WriteEnsured(idx, text);
            payload = std::string("{\"ok\":") + (ok ? "true" : "false") + "}";
          } else if (route == "ensure") {
            
            bool ok = nativeeng::Ensure(idx);
            payload = std::string("{\"ok\":") + (ok ? "true" : "false") + "}";
          } else if (route == "reset") {
            bool ok = nativeeng::Spawn(idx);
            payload = std::string("{\"ok\":") + (ok ? "true" : "false") + "}";
          } else if (route == "out") {
            uint64_t since = (uint64_t)strtoull(qget("since").c_str(), nullptr, 10);
            payload = nativeeng::Poll(idx, since);
          } else {
            payload = "{\"error\":\"unknown engine route\"}";
          }
          mime = "application/json; charset=utf-8";
          extra = "Cache-Control: no-store\r\n";
          body.assign(payload.begin(), payload.end());
        } else if (path.rfind("/ai/", 0) == 0) {
          std::string name = path.substr(4);
          bool nameOk = !name.empty() && name.find("..") == std::string::npos &&
                        name.find('/') == std::string::npos && name.find('\\') == std::string::npos;
          if (nameOk) {
            std::ifstream ff(Join(Join(ExeDir(), L"resources"), Utf8ToWide(name)), std::ios::binary);
            if (ff) {
              body.assign((std::istreambuf_iterator<char>(ff)), std::istreambuf_iterator<char>());
              mime = UiHttpMime(name);
              extra = "Cache-Control: max-age=86400\r\n";
            }
          }
        } else {
          std::string name = path.substr(1);
          bool nameOk = !name.empty() && name.find("..") == std::string::npos &&
                        name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
                        name.find('.') != std::string::npos;
          if (nameOk) {
            // ★ 2026-10-19：先取单一 ui.pak（二进制容器），再回退老的 <name>.enc，最后明文
            std::wstring dir = Join(ExeDir(), L"calc");
            std::wstring enc = Join(dir, Utf8ToWide(name) + L".enc");
            std::vector<uint8_t> dec;
            if (gbCalcReadUiPak(Join(dir, L"ui.pak"), name, dec)) {
              body.assign(dec.begin(), dec.end());
            } else if (FileExists(enc) && gbCalcReadEncFile(enc, body)) {
              
            } else {
            std::ifstream f(Join(dir, Utf8ToWide(name)), std::ios::binary);
            if (f) body.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
          }
          
          extra = "Cache-Control: no-store\r\n";
          mime = UiHttpMime(name);
          }
        }
      }
      if (body.empty()) { status = 404; statusText = "Not Found"; mime = "text/plain"; }
      char head[512];
      snprintf(head, sizeof(head),
               "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n%s"
               "Cross-Origin-Opener-Policy: same-origin\r\n"
               "Cross-Origin-Embedder-Policy: require-corp\r\n"
               "Content-Length: %zu\r\nConnection: close\r\n\r\n",
               status, statusText, mime, extra.c_str(), body.size());
      LogMsg("[ui-http] req " + method + " " + path + (query.empty() ? "" : ("?" + query)));
      std::string h = head;
      SendAll(cs, h.c_str(), (int)h.size());
      if (!body.empty()) SendAll(cs, (const char*)body.data(), (int)body.size());
      closesocket(cs);
    }).detach();
  }
  closesocket(ls);
  WSACleanup();
}
#endif  

static void InboxFileWatcher() {
  wchar_t tmp[MAX_PATH] = {0};
  GetTempPathW(MAX_PATH, tmp);
  std::wstring path = Join(std::wstring(tmp), L"gb-calc-inbox.json");
  while (g_running.load()) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      LARGE_INTEGER sz;
      if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1 << 20)) {
        std::string data((size_t)sz.QuadPart, '\0');
        DWORD rd = 0;
        ReadFile(h, &data[0], (DWORD)sz.QuadPart, &rd, nullptr);
        data.resize(rd);
        if (!data.empty() && data != g_inboxSeen) {
          g_inboxSeen = data;
          std::string msg = "{\"type\":\"historyInbox\",\"text\":";
          msg += JsonQuote(data);
          msg += "}";
          PostToPage(msg);
          LogMsg("[inbox] file inbox picked up " + std::to_string(data.size()) + " bytes");
        }
      }
      CloseHandle(h);
    }
    Sleep(1200);
  }
}

template <class T> class Cb : public T {
 public:
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** pp) override {
    if (pp) *pp = nullptr;
    return E_NOINTERFACE;
  }
};

static std::wstring UiDir() { return Join(ExeDir(), L"calc"); }

static std::wstring UiPathFromUri(const std::wstring& uri) {
  const std::wstring prefix = L"https://gbcalc.test/";
  size_t p = uri.find(prefix);
  std::wstring rest = (p == std::wstring::npos) ? uri : uri.substr(p + prefix.size());
  size_t q = rest.find_first_of(L"?#");
  if (q != std::wstring::npos) rest = rest.substr(0, q);
  while (!rest.empty() && rest[0] == L'/') rest.erase(0, 1);
  if (rest.empty()) rest = L"calc.html";
  if (rest.find(L"..") != std::wstring::npos) rest = L"__deny__";   
  return rest;
}
static const wchar_t* UiMimeFor(const std::wstring& p) {
  if (p.size() >= 5 && p.compare(p.size() - 5, 5, L".html") == 0) return L"text/html; charset=utf-8";
  if (p.size() >= 3 && p.compare(p.size() - 3, 3, L".js") == 0) return L"text/javascript; charset=utf-8";
  if (p.size() >= 4 && p.compare(p.size() - 4, 4, L".css") == 0) return L"text/css; charset=utf-8";
  if (p.size() >= 4 && p.compare(p.size() - 4, 4, L".png") == 0) return L"image/png";
  if (p.size() >= 4 && p.compare(p.size() - 4, 4, L".ico") == 0) return L"image/x-icon";
  return L"application/octet-stream";
}

static void RespondMem(ICoreWebView2WebResourceRequestedEventArgs* args, int status,
                       const wchar_t* reason, const std::wstring& mime,
                       const wchar_t* extraName, const wchar_t* extraValue,
                       const std::vector<uint8_t>& body) {
  if (!g_env || !args) return;
  IStream* st = body.empty() ? SHCreateMemStream((const BYTE*)"", 0)
                             : SHCreateMemStream(body.data(), (UINT)body.size());
  if (!st) { LogMsg("[res] SHCreateMemStream failed"); return; }
  ICoreWebView2WebResourceResponse* resp = nullptr;
  HRESULT hr = g_env->CreateWebResourceResponse(st, status, reason, mime.c_str(), &resp);
  if (SUCCEEDED(hr) && resp) {
    ICoreWebView2HttpResponseHeaders* hs = nullptr;
    if (SUCCEEDED(resp->get_Headers(&hs)) && hs) {
      hs->AppendHeader(L"Cross-Origin-Opener-Policy", L"same-origin");
      hs->AppendHeader(L"Cross-Origin-Embedder-Policy", L"require-corp");
      if (extraName && *extraName) hs->AppendHeader(extraName, extraValue);
      if (!mime.empty()) hs->AppendHeader(L"Content-Type", mime.c_str());
      hs->AppendHeader(L"Cache-Control", L"no-store");
      hs->Release();
    }
    HRESULT pr = args->put_Response(resp);
    if (FAILED(pr)) { char rb[128]; snprintf(rb, sizeof(rb), "[res] put_Response hr=0x%08lX", (unsigned long)pr); LogMsg(rb); }
    resp->Release();
  } else {
    char rb[128]; snprintf(rb, sizeof(rb), "[res] CreateWebResourceResponse hr=0x%08lX", (unsigned long)hr); LogMsg(rb);
  }
  st->Release();
}
static std::wstring AiMimeFor(const std::wstring& p) {
  if (p.size() >= 5 && p.compare(p.size() - 5, 5, L".wasm") == 0) return L"application/wasm";
  if (p.size() >= 3 && p.compare(p.size() - 3, 3, L".js") == 0) return L"text/javascript; charset=utf-8";
  return L"application/octet-stream";
}

// ★ 2026-10-19（用户：「有的电脑会 360 误杀，导致软件打不开」）：
//   UI 包（calc/ui.pak）被杀软隔离时，宿主原来只会 404 → 用户对着白屏，完全不知道发生了什么。
//   这里内置一张极简诊断页：把「文件没了 / 被隔离 / 怎么办」直接写在窗口里。
static const char* kUiMissingHtml =
  "<!doctype html><meta charset=\"utf-8\"><title>\xE7\x95\x8C\xE9\x9D\xA2\xE7\xBC\xBA\xE5\xA4\xB1</title>"
  "<style>html,body{height:100%;margin:0;background:#1b1f24;color:#e6edf3;"
  "font:15px/1.7 'Segoe UI','Microsoft YaHei',sans-serif;display:flex;align-items:center;"
  "justify-content:center}main{max-width:34rem;padding:2rem;border:1px dashed #3d444d;"
  "border-radius:12px}h1{font-size:1.15rem;margin:0 0 .6rem;color:#e5534b}p{margin:.4rem 0;color:#c9d1d9}"
  "code{background:#262c34;padding:.1rem .35rem;border-radius:5px}</style>"
  "<main><h1>\xE7\x95\x8C\xE9\x9D\xA2\xE8\xB5\x84\xE6\xBA\x90\xE7\xBC\xBA\xE5\xA4\xB1</h1>"
  "<p>\xE7\xA8\x8B\xE5\xBA\x8F\xE6\x89\xBE\xE4\xB8\x8D\xE5\x88\xB0 <code>calc\\ui.pak</code>"
  "\xEF\xBC\x88\xE7\x95\x8C\xE9\x9D\xA2\xE8\xB5\x84\xE6\xBA\x90\xE5\x8C\x85\xEF\xBC\x89\xE3\x80\x82</p>"
  "<p>\xE5\xB8\xB8\xE8\xA7\x81\xE5\x8E\x9F\xE5\x9B\xA0\xEF\xBC\x9A\xE5\xAE\x89\xE5\x85\xA8\xE8\xBD\xAF\xE4\xBB\xB6"
  "\xEF\xBC\x88\xE5\xA6\x82 360\xE3\x80\x81\xE9\x82\x81\xE6\x8A\xA4\xEF\xBC\x89\xE5\xB0\x86\xE5\xAE\x83\xE9\x9A\x94"
  "\xE7\xA6\xBB\xE6\x88\x96\xE5\x88\xA0\xE9\x99\xA4\xEF\xBC\x8C\xE6\x88\x96\xE5\x8D\x87\xE7\xBA\xA7\xE5\xAE\x89"
  "\xE8\xA3\x85\xE6\x97\xB6\xE6\xAE\x8B\xE7\x95\x99\xE4\xBA\x86\xE6\x97\xA7\xE7\x9B\xAE\xE5\xBD\x95\xE3\x80\x82</p>"
  "<p>\xE5\xA4\x84\xE7\x90\x86\xEF\xBC\x9A\xE6\x8A\x8A\xE5\xAE\x89\xE8\xA3\x85\xE7\x9B\xAE\xE5\xBD\x95\xE5\x8A\xA0"
  "\xE5\x85\xA5\xE6\x9D\x80\xE8\xBD\xAF\xE7\x99\xBD\xE5\x90\x8D\xE5\x8D\x95\xEF\xBC\x8C\xE7\x84\xB6\xE5\x90\x8E"
  "\xE9\x87\x8D\xE8\xA3\x85\xE4\xB8\x80\xE6\xAC\xA1\xE5\x8D\xB3\xE5\x8F\xAF\xE3\x80\x82</p>"
  "<p style=\"opacity:.7\">UI package missing. Whitelist the install folder in your antivirus, "
  "then reinstall.</p></main>";

class ResHandler : public Cb<ICoreWebView2WebResourceRequestedEventHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) override {
    if (!args) return S_OK;
    ICoreWebView2WebResourceRequest* req = nullptr;
    if (FAILED(args->get_Request(&req)) || !req) return S_OK;
    LPWSTR uriW = nullptr;
    req->get_Uri(&uriW);
    std::wstring uri = uriW ? uriW : L"";
    if (uriW) CoTaskMemFree(uriW);
    if (uri.find(L"gbcalc.test") == std::wstring::npos) { req->Release(); return S_OK; }  
    
    if (uri.find(L"/engine/out") == std::wstring::npos)
      LogMsg("[res] enter " + WideToUtf8(uri).substr(0, 100));

    std::string reqBody;
    {
      IStream* cs = nullptr;
      if (SUCCEEDED(req->get_Content(&cs)) && cs) {
        char cb[8192]; ULONG rd = 0;
        for (;;) {
          if (FAILED(cs->Read(cb, sizeof(cb), &rd)) || rd == 0) break;
          reqBody.append(cb, rd);
          if (reqBody.size() > ((size_t)4 << 20)) break;
        }
        cs->Release();
      }
    }
    req->Release();

    std::wstring path = UiPathFromUri(uri);
    std::string query;
    {
      size_t q = uri.find(L'?');
      if (q != std::wstring::npos) {
        std::wstring qw = uri.substr(q + 1);
        size_t h = qw.find(L'#');
        if (h != std::wstring::npos) qw = qw.substr(0, h);
        query = WideToUtf8(qw);
      }
    }

    if (path.rfind(L"ai/", 0) == 0) {
      std::wstring name = path.substr(3);
      bool nameOk = !name.empty() && name.find(L"..") == std::wstring::npos &&
                    name.find(L'/') == std::wstring::npos && name.find(L'\\') == std::wstring::npos;
      std::vector<uint8_t> ai;
      if (nameOk) {
        std::wstring f = Join(Join(ExeDir(), L"resources"), name);
        if (FileExists(f)) {
          std::ifstream ff(f, std::ios::binary);
          if (ff) ai.assign((std::istreambuf_iterator<char>(ff)), std::istreambuf_iterator<char>());
        }
      }
      if (!ai.empty()) RespondMem(args, 200, L"OK", AiMimeFor(name), L"Cache-Control", L"max-age=86400", ai);
      else             RespondMem(args, 404, L"Not Found", L"text/plain", L"Cache-Control", L"no-store", {});
      return S_OK;
    }
    
    std::vector<uint8_t> content;
    bool ok = false;
    const char* how = "none";
    // ★ 2026-10-19：优先单一 ui.pak（二进制容器，见 ui_crypto.h 头注释）；老包仍回退 .enc，
    //   最后回退明文（开发态直接改 ui/ 里的源文件时走这条）。
    std::wstring enc = Join(UiDir(), path + L".enc");
    if (gbCalcReadUiPak(Join(UiDir(), L"ui.pak"), WideToUtf8(path), content)) { ok = true; how = "pak"; }
    else if (FileExists(enc) && gbCalcReadEncFile(enc, content)) { ok = true; how = "enc"; }
    else if (FileExists(enc)) { how = "enc-decrypt-FAILED"; }
    if (!ok) {
      std::wstring plain = Join(UiDir(), path);
      if (FileExists(plain)) {
        std::ifstream f(plain, std::ios::binary);
        if (f) { content.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); ok = true; how = "plain"; }
      }
    }
    if (!ok || content.empty()) {
      
      // ★ 2026-10-19：calc.html 拿不到 ⇒ 给内置诊断页（白屏最难排查，也最像「软件打不开」）
      if (path == L"calc.html") {
        std::string page(kUiMissingHtml);
        LogMsg("[res] calc.html MISSING -> built-in notice page (ui.pak 被隔离/删除？)");
        RespondMem(args, 200, L"OK", L"text/html; charset=utf-8", L"Cache-Control", L"no-store",
                   std::vector<uint8_t>(page.begin(), page.end()));
        return S_OK;
      }
      LogMsg("[res] MISS " + WideToUtf8(path) + " (404)");
      RespondMem(args, 404, L"Not Found", L"text/plain", L"Cache-Control", L"no-store", {});
      return S_OK;
    }
    { char rb[512];
      snprintf(rb, sizeof(rb), "[res] serve %ls how=%s bytes=%zu", path.c_str(), how, content.size());
      LogMsg(rb); }
    
    RespondMem(args, 200, L"OK", UiMimeFor(path), L"Cache-Control", L"no-store", content);
    return S_OK;
  }
};

static ResHandler g_resHandler;   

static void LayoutWebView() {
  if (!g_controller || !g_hwnd) return;
  RECT r;
  GetClientRect(g_hwnd, &r);
  g_controller->put_Bounds(r);
}

// ★ 2026-10-16：主页面「沉默」时的取证探针。沉默 = 宿主 serve 了 calc.html/css/js，
//   此后页面一条消息都不发、也收不到 ready。两种完全不同的病因必须区分开：
//     · 有回话 ⇒ 渲染进程活着、JS 能跑 ⇒ 卡在 boot() / chrome.webview 缺失；
//     · 没回话 ⇒ 渲染进程根本没起来（浏览器/GPU 进程问题）。
static const char* kSilentProbeJs =
    "(function(){try{return JSON.stringify({rs:document.readyState,"
    "cv:(window.chrome&&window.chrome.webview)?1:0,title:document.title,"
    "boot:!!window.GBUI,errs:(window.__gbErrs||[]).slice(0,3),"
    "htmlLen:document.documentElement?document.documentElement.innerHTML.length:-1});"
    "}catch(e){return 'EVALERR '+e.message}})()";

class MainProbeHandler : public Cb<ICoreWebView2ExecuteScriptCompletedHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, LPCWSTR result) override {
    if (result && *result) LogMsg("[ui] silent-probe reply: " + WideToUtf8(result));
    else LogMsg("[ui] silent-probe: 渲染进程无回话（hr=0x" +
                std::to_string((unsigned)errorCode) + "）⇒ 渲染进程未起来");
    return S_OK;
  }
};

class EvalHandler : public Cb<ICoreWebView2ExecuteScriptCompletedHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, LPCWSTR result) override {
    if (result && *result) LogMsg("[eval] " + WideToUtf8(result));
    else LogMsg("[eval] (empty result)");
    return S_OK;
  }
};

// ★ 2026-10-13：这里的更新链接**不再**跟着 GB_SUITE_ENGINE 走。
//   本 fork（desktop-connect）是「单训练器独立版 / 连线版」，它的构建脚本为了拿到
//   host.cpp 里的引擎看门狗而定义了 /DGB_SUITE_ENGINE —— 于是旧写法把更新链接也一起
//   切成了三件套的 b016l4d3id / 55555，用户点「检查更新」会被导航到**错误的安装包**。
//   链接只认本 fork 自己的身份：连线版 = 训练器独立版 = b016l49aqj / 15
//   （与 desktop-connect/ui/calc.js 的 UPDATE_URL / UPDATE_PWD 一致，
//    也与 tools/build-installer.js 的 TRAINER_URL 一致）。
static const char* kUpdateShareUrl = "https://wwbbj.lanzout.com/b016l49aqj";
static const char* kUpdateSharePwd = "15";

static HWND g_updHwnd = nullptr;                 
static ICoreWebView2Controller* g_updController = nullptr;   
static ICoreWebView2* g_updWebview = nullptr;
static bool g_updCreating = false;

static ICoreWebView2Controller* g_updToolController = nullptr;   
static ICoreWebView2* g_updToolWebview = nullptr;

static ICoreWebView2Controller* g_updSuggestController = nullptr;
static ICoreWebView2* g_updSuggestWebview = nullptr;

static HWND g_updSuggestWnd = nullptr;
static int g_updSuggestH = 300;   
static double g_updZoom = 1.0;   
static void UpdShowSuggest(const std::wstring& json);   

static int UpdBarH(HWND h, int clientPxH) {
  int dpi = GbDpiFor(h);
  int cssH = MulDiv(clientPxH, 96, dpi);
  int css = (cssH < 560) ? 40 : (cssH > 900 ? 48 : 46);
  return MulDiv(css, dpi, 96);
}

static const int kUpdDefCssW = 1100, kUpdDefCssH = 780;
static const int kUpdMinCssW = 520,  kUpdMinCssH = 560;

static bool g_updSearchMode = false;             

static std::wstring UpdUrlEncode(const std::wstring& w) {
  std::string u8 = WideToUtf8(w);
  static const char* hex = "0123456789ABCDEF";
  std::string o;
  for (size_t i = 0; i < u8.size(); ++i) {
    unsigned char c = (unsigned char)u8[i];
    bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) o += (char)c; else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return Utf8ToWide(o);
}

static void UpdNavigateTo(const std::wstring& raw) {
  if (!g_updWebview) return;
  std::wstring u = raw;
  while (!u.empty() && (u[0] == L' ' || u[0] == L'\t')) u.erase(0, 1);
  while (!u.empty() && (u[u.size()-1] == L' ' || u[u.size()-1] == L'\t')) u.erase(u.size()-1);
  if (u.empty()) { g_updWebview->Navigate(Utf8ToWide(std::string(kUpdateShareUrl)).c_str()); return; }
  
  bool hasScheme = u.find(L"://") != std::wstring::npos;
  bool hasHttp = _wcsnicmp(u.c_str(), L"http://", 7) == 0 || _wcsnicmp(u.c_str(), L"https://", 8) == 0;
  
  while (u.size() >= 8 && _wcsnicmp(u.c_str(), L"https://", 8) == 0 &&
         (_wcsnicmp(u.c_str() + 8, L"https://", 8) == 0 || _wcsnicmp(u.c_str() + 8, L"http://", 7) == 0))
    u.erase(0, 8);
  bool looksUrl = !hasScheme && !hasHttp && u.find(L'.') != std::wstring::npos &&
                  u.find(L' ') == std::wstring::npos && u.find(L'\t') == std::wstring::npos;
  if (hasScheme || hasHttp) {
    
  } else if (looksUrl) {
    u = L"https://" + u;
  } else {
    u = L"https://www.bing.com/search?q=" + UpdUrlEncode(u);
  }
  g_updWebview->Navigate(u.c_str());
}

static void UpdAutofillPwd() {
  if (!g_updWebview) return;
  
  LPWSTR cur = nullptr;
  if (FAILED(g_updWebview->get_Source(&cur)) || !cur) return;
  std::wstring url = cur;
  CoTaskMemFree(cur);
  std::wstring low = url;
  for (auto& c : low) c = towlower(c);
  if (low.find(L"lanzou") == std::wstring::npos && low.find(L"lanzo") == std::wstring::npos) return;
  
  static std::wstring lastUrl;
  if (lastUrl == url) return;
  lastUrl = url;
  
  std::wstring js =
    std::wstring(L"(function(){try{var P='") + Utf8ToWide(std::string(kUpdateSharePwd)) + L"';"
    L"var inp=document.querySelector('input[type=password],#pwd,#p,input[name=p],input[name=pwd]');"
    L"if(!inp)return;"
    L"inp.value=P;inp.dispatchEvent(new Event('input',{bubbles:true}));"
    L"var f=inp.closest('form');if(!f)return;"
    L"var b=f.querySelector('input[type=submit],button[type=submit],button');"
    L"if(b){b.click();}else{f.submit();}"
    L"}catch(e){}})();";
  g_updWebview->ExecuteScript(js.c_str(), nullptr);
}

static std::wstring WebView2UserDataDir();   
static const __int64 kUpdCleanLimit = 128LL * 1024 * 1024;   
static const DWORD kUpdCleanMinGap = 60000;                  
static DWORD g_updLastCleanTry = 0;

static __int64 UpdDirSize(const std::wstring& root) {
  __int64 total = 0;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) continue;
    std::wstring full = root + L"\\" + fd.cFileName;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += UpdDirSize(full);
    else total += ((__int64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return total;
}

static void UpdDeleteTree(const std::wstring& root) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) continue;
      std::wstring full = root + L"\\" + fd.cFileName;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) UpdDeleteTree(full);
      else { SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(full.c_str()); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  SetFileAttributesW(root.c_str(), FILE_ATTRIBUTE_NORMAL);
  RemoveDirectoryW(root.c_str());
}

static void UpdSweepCacheDirs() {
  static const wchar_t* dirs[] = {
    L"Cache", L"Code Cache", L"GPUCache", L"DawnCache", L"DawnGraphiteCache",
    L"Service Worker", L"IndexedDB", L"Session Storage", L"CacheStorage", L"SharedWorker"
  };
  static const wchar_t* files[] = { L"Cookies", L"Cookies-journal" };
  std::wstring root = WebView2UserDataDir() + L"\\EBWebView";
  for (auto n : dirs) {
    std::wstring d = root + L"\\" + n;
    DWORD att = GetFileAttributesW(d.c_str());
    if (att == INVALID_FILE_ATTRIBUTES || !(att & FILE_ATTRIBUTE_DIRECTORY)) continue;
    UpdDeleteTree(d);
  }
  for (auto f : files) {
    std::wstring d = root + L"\\" + f;
    DWORD att = GetFileAttributesW(d.c_str());
    if (att == INVALID_FILE_ATTRIBUTES) continue;
    SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(d.c_str());
  }
  LogMsg("[upd] browsing data >128MB auto-swept (search history kept)");
}

static void UpdMaybeCleanData() {
  DWORD now = GetTickCount();
  if (now - g_updLastCleanTry < kUpdCleanMinGap) return;
  g_updLastCleanTry = now;
  std::wstring root = WebView2UserDataDir();
  if (UpdDirSize(root) < kUpdCleanLimit) return;   
  UpdSweepCacheDirs();
}

  static std::wstring UpdToolBarHtml(bool dark, bool langEn) {
    std::wstring bodyCls = dark ? L"" : L" class=\"light\"";   
    std::wstring h =
      L"<!doctype html><html><head><meta charset=\"utf-8\"><style>"
      L":root{--bg:#26272a;--fg:#e6e7ea;--sub:#9aa0a6;--field:#1c1e21;--hover:rgba(255,255,255,.07);--border:rgba(255,255,255,.09);--accent:#2fc1d8}"
      L"body.light{--bg:#f4f5f7;--fg:#2d2f32;--sub:#87888b;--field:#ffffff;--hover:rgba(0,0,0,.05);--border:rgba(0,0,0,.10);--accent:#137f98}"
      L"*{margin:0;padding:0;box-sizing:border-box}"
      L"html,body{height:100%}"
      L"body{background:var(--bg);color:var(--fg);display:flex;align-items:center;gap:6px;padding:0 10px;font:14px/1 'Segoe UI','Microsoft YaHei UI',system-ui,sans-serif;user-select:none}"
      L".nav{width:34px;height:34px;flex:none;border:0;border-radius:10px;background:transparent;color:var(--sub);font-size:15px;cursor:pointer;display:inline-flex;align-items:center;justify-content:center;transition:background .12s,color .12s}"
      L".nav:hover{background:var(--hover);color:var(--fg)}"
      L".nav:disabled{opacity:.25;cursor:default}"
      L"#box{flex:1;display:flex;align-items:center;gap:8px;height:34px;background:var(--field);border:1px solid var(--border);border-radius:16px;padding:0 14px}"
      L"#box:focus-within{border-color:var(--accent);box-shadow:0 0 0 3px color-mix(in srgb,var(--accent) 20%,transparent)}"
      L"#q{flex:1;border:0;outline:0;background:transparent;color:var(--fg);font-size:14px;min-width:0}"
      L"#q::placeholder{color:var(--sub)}"
      L".ico{color:var(--sub);font-size:13px;flex:none}"
      L"</style></head><body" + bodyCls + L">" +
      std::wstring(L"<button class=\"nav\" id=\"bBack\" title=\"") + (langEn ? L"Back" : L"后退") + L"\">&#9664;</button>" +
      std::wstring(L"<button class=\"nav\" id=\"bFwd\" title=\"") + (langEn ? L"Forward" : L"前进") + L"\">&#9654;</button>" +
      std::wstring(L"<button class=\"nav\" id=\"bRel\" title=\"") + (langEn ? L"Refresh" : L"刷新") + L"\">&#10227;</button>" +
      L"<div id=\"box\"><span class=\"ico\">&#128269;</span><input id=\"q\" spellcheck=\"false\" autocomplete=\"off\"></div>"
      L"<script>"
      L"(function(){"
      L"var wv=(window.chrome&&chrome.webview)?chrome.webview:null;"
      L"var Q=document.getElementById('q');"
      L"var hist=[];try{hist=JSON.parse(localStorage.getItem('gbUpdHist')||'[]');}catch(e){hist=[];}"
      L"if(!Array.isArray(hist))hist=[];"
      L"var EN=(navigator.language&&navigator.language.indexOf('en')===0);"
      L"function post(o){if(wv)wv.postMessage(o);}"
      L"function save(){try{localStorage.setItem('gbUpdHist',JSON.stringify(hist.slice(0,30)));}catch(e){}}"
      L"function send(){var v=Q.value.trim();var items=[];if(!v){items=hist.slice(0,30);}else{var lw=v.toLowerCase();for(var i=0;i<hist.length;i++){var t=String(hist[i]);if(t.toLowerCase().indexOf(lw)>=0)items.push(t);}}"
      L"post({tool:'suggest',items:items,act:v,lang:EN?1:0,dark:document.body.classList.contains('light')?0:1});}"
      L"function setLang(en){EN=!!en;Q.placeholder=en?'Search or enter a URL':'搜索或输入网址';}"
      L"Q.addEventListener('keydown',function(e){if(e.key==='Enter'){var q=Q.value.trim();if(q)post({tool:'go',q:q});post({tool:'suggestHide'});}});"
      L"Q.addEventListener('input',function(){send();});"
      L"Q.addEventListener('focus',function(){send();});"
      L"Q.addEventListener('blur',function(){setTimeout(function(){post({tool:'suggestHide'});},150);});"
      L"document.getElementById('bBack').addEventListener('click',function(){post({tool:'back'});});"
      L"document.getElementById('bFwd').addEventListener('click',function(){post({tool:'fwd'});});"
      L"document.getElementById('bRel').addEventListener('click',function(){post({tool:'reload'});});"
      L"if(wv)wv.addEventListener('message',function(e){"
      L"var d=e.data;if(typeof d==='string'){try{d=JSON.parse(d);}catch(x){return;}}"
      L"if(!d)return;"
      L"if(typeof d.url==='string'){Q.value=d.url;hist=[d.url].concat(hist.filter(function(x){return x!==d.url;}));hist=hist.slice(0,30);save();}"
      L"if(typeof d.canBack==='boolean')document.getElementById('bBack').disabled=!d.canBack;"
      L"if(typeof d.canFwd==='boolean')document.getElementById('bFwd').disabled=!d.canFwd;"
      L"if(typeof d.lang==='number')setLang(!!d.lang);"
      L"if(typeof d.dark==='number')document.body.classList.toggle('light',!d.dark);"
      L"});"
      L"if(navigator.language&&navigator.language.indexOf('en')===0)setLang(true);"
      L"})();"
      L"</script></body></html>";
    return h;
  }
  
  static std::wstring UpdSuggestHtml(bool dark, bool langEn) {
    std::wstring bodyCls = dark ? L"" : L" class=\"light\"";
    return
      L"<!doctype html><html><head><meta charset=\"utf-8\"><style>"
      L":root{--bg:#202124;--fg:#e6e7ea;--sub:#9aa0a6;--hover:rgba(255,255,255,.12);--accent:#8ab4f8;--border:rgba(255,255,255,.18);--shadow:rgba(0,0,0,.30)}"
      L"body.light{--bg:#ffffff;--fg:#2d2f32;--sub:#87888b;--hover:rgba(0,0,0,.08);--accent:#1a73e8;--border:rgba(0,0,0,.14);--shadow:rgba(0,0,0,.10)}"
      L"*{margin:0;padding:0;box-sizing:border-box}"
      L"html,body{height:100%}"
      L"body{background:var(--bg);font:14px/1 'Segoe UI','Microsoft YaHei UI',system-ui,sans-serif;user-select:none;padding:0}"
      L"#wrap{background:var(--bg);border:1px solid var(--border);border-radius:10px;height:100%;max-height:100%;overflow:hidden}"
      L"#list{overflow-y:auto;scrollbar-width:thin;scrollbar-color:rgba(128,128,128,.4) transparent;max-height:278px}"
      L"#list::-webkit-scrollbar{width:6px}"
      L"#list::-webkit-scrollbar-thumb{background:rgba(128,128,128,.4);border-radius:3px}"
      L".it{padding:10px 14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;cursor:pointer;color:var(--fg);display:flex;align-items:center;gap:8px;border-radius:6px;font-size:14px;line-height:1.3}"
      L".it:hover{background:var(--hover)}"
      L".it .act{flex:none;font-size:11px;color:var(--accent);margin-left:auto;opacity:.85}"
      L".empty{padding:7px 14px;font-size:12px;color:var(--sub)}"
      L"</style></head><body" + bodyCls + L"><div id=\"wrap\"><div id=\"list\"></div></div><script>"
      L"(function(){"
      L"var L=document.getElementById('list');"
      L"window.__upd=function(d){"
      L"if(!d)return;"
      L"document.body.classList.toggle('light',!(d.dark===undefined||d.dark));"
      L"var EN=!!d.lang;"
      L"L.innerHTML='';"
      L"var items=(d.items||[]),act=String(d.act||'');"
      L"if(!items.length&&!act){var e=document.createElement('div');e.className='empty';e.textContent=EN?'No suggestions':'暂无建议';L.appendChild(e);return;}"
      L"if(act){var a=document.createElement('div');a.className='it';a.innerHTML='<span></span><span class=\"act\">'+(EN?'Search':'搜索')+'</span>';a.firstChild.textContent=act;a.setAttribute('data-q',act);L.appendChild(a);}"
      L"for(var i=0;i<items.length;i++){var d2=document.createElement('div');d2.className='it';d2.textContent=items[i];d2.setAttribute('data-q',items[i]);L.appendChild(d2);}"
      L"};"
      L"document.addEventListener('click',function(e){"
      L"var t=e.target&&e.target.closest?e.target.closest('.it'):null;if(!t)return;var wv=window.chrome&&chrome.webview?chrome.webview:null;if(wv)wv.postMessage({tool:'pick',q:t.getAttribute('data-q')});});"
      L"})();"
      L"</script></body></html>";
  }
  
  static std::wstring JsonQuoteW(const std::wstring& s) {
    std::wstring o = L"\"";
    for (wchar_t c : s) { if (c == L'"' || c == L'\\') o += L'\\'; o += c; }
    o += L"\"";
    return o;
  }
  
  static void UpdNavSync() {
    if (!g_updToolWebview || !g_updWebview) return;
    LPWSTR u = nullptr;
    std::wstring url;
    if (SUCCEEDED(g_updWebview->get_Source(&u)) && u) { url = u; CoTaskMemFree(u); }
    BOOL canBack = FALSE, canFwd = FALSE;
    g_updWebview->get_CanGoBack(&canBack);
    g_updWebview->get_CanGoForward(&canFwd);
    std::wstring j = L"{\"url\":" + JsonQuoteW(url) +
                     L",\"canBack\":" + (canBack ? L"true" : L"false") +
                     L",\"canFwd\":" + (canFwd ? L"true" : L"false") + L"}";
    g_updToolWebview->PostWebMessageAsJson(j.c_str());
  }
  
  class UpdToolMsgHandler : public Cb<ICoreWebView2WebMessageReceivedEventHandler> {
   public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) override {
      LPWSTR raw = nullptr;
      if (FAILED(a->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
      std::wstring s = raw;
      CoTaskMemFree(raw);
      if (s.find(L"\"tool\":\"back\"") != std::wstring::npos && g_updWebview) g_updWebview->GoBack();
      else if (s.find(L"\"tool\":\"fwd\"") != std::wstring::npos && g_updWebview) g_updWebview->GoForward();
      else if (s.find(L"\"tool\":\"reload\"") != std::wstring::npos && g_updWebview) g_updWebview->Reload();
      else if (s.find(L"\"tool\":\"suggest\"") != std::wstring::npos) { UpdShowSuggest(s); }
      else if (s.find(L"\"tool\":\"panel\"") != std::wstring::npos) { UpdShowSuggest(s); }
      else if (s.find(L"\"tool\":\"unfav\"") != std::wstring::npos) {
        if (g_updToolWebview) {
          std::wstring js = L"try{window.__unfav&&window.__unfav(" + s + L");}catch(e){}";
          g_updToolWebview->ExecuteScript(js.c_str(), nullptr);
        }
      }
      else if (s.find(L"\"tool\":\"suggestHide\"") != std::wstring::npos) {
        if (g_updSuggestController) g_updSuggestController->put_IsVisible(FALSE);
        if (g_updSuggestWnd) ShowWindow(g_updSuggestWnd, SW_HIDE);
      }
      else if (s.find(L"\"tool\":\"go\"") != std::wstring::npos) {
        size_t p = s.find(L"\"q\":\"");
        if (p != std::wstring::npos) {
          std::wstring q = s.substr(p + 5);
          size_t e = q.find(L'"');
          if (e != std::wstring::npos) q = q.substr(0, e);
          std::wstring dq;
          for (size_t i = 0; i < q.size(); ++i) {
            if (q[i] == L'\\' && i + 1 < q.size()) { dq += q[i + 1]; ++i; } else dq += q[i];
          }
          UpdNavigateTo(dq);
        }
      }
      return S_OK;
    }
  };
  
  static void UpdLayoutSuggest(int h) {
    if (!g_updHwnd || !g_updSuggestWnd) return;
    RECT rc;
    GetClientRect(g_updHwnd, &rc);
    RECT rc0; GetClientRect(g_updHwnd, &rc0);
    int bar = UpdBarH(g_updHwnd, rc0.bottom);
    
    int x = GbPx(g_updHwnd, 130);
    int w = rc.right - GbPx(g_updHwnd, 140);
    POINT pt = {x, bar - GbPx(g_updHwnd, 6)};   
    ClientToScreen(g_updHwnd, &pt);
    SetWindowPos(g_updSuggestWnd, nullptr, pt.x, pt.y, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    
    if (g_updSuggestController) {
      RECT cr;
      GetClientRect(g_updSuggestWnd, &cr);
      g_updSuggestController->put_Bounds(RECT{0, 0, cr.right, cr.bottom});
    }
  }
  
  static void UpdShowSuggest(const std::wstring& json) {
    (void)json;   
    return;
    if (!g_updSuggestWebview) { LogMsg("[upd] SUGGEST skip: no suggest webview"); return; }
    
    int rows = 0;
    size_t ap = json.find(L"\"act\":\"");
    if (ap != std::wstring::npos) { size_t av = ap + 7; if (av < json.size() && json[av] != L'"') rows++; }
    size_t ip = json.find(L"\"items\":[");
    if (ip != std::wstring::npos) {
      size_t st = ip + 9, en = json.find(L']', st);
      if (en != std::wstring::npos && en > st) {
        std::wstring seg = json.substr(st, en - st);
        int n = 1;
        for (wchar_t c : seg) if (c == L',') n++;
        rows += n;
      }
    }
    int sh = rows > 0 ? (42 * (rows > 8 ? 8 : rows)) : 1;   
    g_updSuggestH = GbPx(g_updHwnd, sh);
    
    UpdLayoutSuggest(g_updSuggestH);
    if (g_updSuggestWnd) {
      SetWindowPos(g_updSuggestWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      ShowWindow(g_updSuggestWnd, SW_SHOW);
    }
    if (g_updSuggestController) g_updSuggestController->put_IsVisible(TRUE);
    std::wstring js = L"try{window.__upd&&window.__upd(" + json + L");}catch(e){}";
    g_updSuggestWebview->ExecuteScript(js.c_str(), nullptr);
  }
  
  class UpdSuggestMsgHandler : public Cb<ICoreWebView2WebMessageReceivedEventHandler> {
   public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) override {
      LPWSTR raw = nullptr;
      if (FAILED(a->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
      std::wstring s = raw;
      CoTaskMemFree(raw);
      if (g_updSuggestController) g_updSuggestController->put_IsVisible(FALSE);
      if (g_updSuggestWnd) ShowWindow(g_updSuggestWnd, SW_HIDE);
      if (s.find(L"\"tool\":\"pick\"") != std::wstring::npos) {
        size_t p = s.find(L"\"q\":\"");
        if (p != std::wstring::npos) {
          std::wstring q = s.substr(p + 5);
          size_t e = q.find(L'"');
          if (e != std::wstring::npos) q = q.substr(0, e);
          std::wstring dq;
          for (size_t i = 0; i < q.size(); ++i) {
            if (q[i] == L'\\' && i + 1 < q.size()) { dq += q[i + 1]; ++i; } else dq += q[i];
          }
          UpdNavigateTo(dq);
        }
      }
      return S_OK;
    }
  };
  
  class UpdSuggestNavHandler : public Cb<ICoreWebView2NavigationCompletedEventHandler> {
   public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) override {
      
      if (g_updSuggestWnd) ShowWindow(g_updSuggestWnd, SW_HIDE);
      if (g_updSuggestController) g_updSuggestController->put_IsVisible(FALSE);
      return S_OK;
    }
  };
  
  class UpdSuggestCtlHandler : public Cb<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
   public:
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* ctl) override {
      if (FAILED(hr) || !ctl || !g_updSuggestWnd) return S_OK;
      g_updSuggestController = ctl;
      ctl->AddRef();
      ctl->get_CoreWebView2(&g_updSuggestWebview);
      if (!g_updSuggestWebview) return S_OK;
      g_updSuggestWebview->AddRef();
      g_updSuggestWebview->add_WebMessageReceived(new UpdSuggestMsgHandler(), nullptr);
      SetWebviewBg(ctl, g_uiDark ? RGB(32, 33, 36) : RGB(255, 255, 255));   
      RECT rc;
      GetClientRect(g_updSuggestWnd, &rc);
      ctl->put_Bounds(RECT{0, 0, rc.right, rc.bottom});   
      
      g_updSuggestWebview->add_NavigationCompleted(new UpdSuggestNavHandler(), nullptr);
      ctl->put_IsVisible(TRUE);
      g_updSuggestWebview->NavigateToString(UpdSuggestHtml(g_uiDark, g_uiLangEn).c_str());
      return S_OK;
    }
  };
  
  class UpdToolCtlHandler : public Cb<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
   public:
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* ctl) override {
      if (FAILED(hr) || !ctl || !g_updHwnd) return S_OK;
      g_updToolController = ctl;
      ctl->AddRef();
      ctl->get_CoreWebView2(&g_updToolWebview);
      if (!g_updToolWebview) return S_OK;
      g_updToolWebview->AddRef();
      g_updToolWebview->add_WebMessageReceived(new UpdToolMsgHandler(), nullptr);
      RECT rc;
      GetClientRect(g_updHwnd, &rc);
      RECT rcT; GetClientRect(g_updHwnd, &rcT);
      int bar = UpdBarH(g_updHwnd, rcT.bottom);
      ctl->put_Bounds(RECT{0, 0, rc.right, bar});
      ctl->put_IsVisible(TRUE);
      g_updToolWebview->NavigateToString(UpdToolBarHtml(g_uiDark, g_uiLangEn).c_str());
      
      if (!g_updSuggestWnd) {
        WNDCLASSEXW sw = {};
        sw.cbSize = sizeof(sw);
        sw.lpfnWndProc = DefWindowProcW;
        sw.hInstance = g_hInst;
        sw.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        sw.lpszClassName = L"GbUpdSuggestWnd";
        RegisterClassExW(&sw);
        
        g_updSuggestWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"GbUpdSuggestWnd", L"",
                                          WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_hInst, nullptr);   
      }
      if (g_env && g_updSuggestWnd) g_env->CreateCoreWebView2Controller(g_updSuggestWnd, new UpdSuggestCtlHandler());
      return S_OK;
    }
  };

class UpdNavDoneHandler : public Cb<ICoreWebView2NavigationCompletedEventHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) override {
    if (!g_updWebview) return S_OK;
    UpdAutofillPwd();
    UpdNavSync();
    UpdMaybeCleanData();   
    return S_OK;
  }
};

class UpdHistHandler : public Cb<ICoreWebView2HistoryChangedEventHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, IUnknown*) override {  
    UpdNavSync();
    return S_OK;
  }
};

class UpdDownloadHandler : public Cb<ICoreWebView2DownloadStartingEventHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* a) override {
    if (!a || !g_updHwnd) return S_OK;
    ICoreWebView2DownloadOperation* op = nullptr;
    if (FAILED(a->get_DownloadOperation(&op)) || !op) return S_OK;
    std::wstring defName = L"download";
    LPWSTR rfp = nullptr;
    if (SUCCEEDED(op->get_ResultFilePath(&rfp)) && rfp) {
      std::wstring p = rfp;
      CoTaskMemFree(rfp);
      size_t bs = p.find_last_of(L'\\\\'), sl = p.find_last_of(L'/');
      size_t at = (bs == std::wstring::npos) ? sl : ((sl == std::wstring::npos) ? bs : (bs > sl ? bs : sl));
      if (at != std::wstring::npos && at + 1 < p.size()) defName = p.substr(at + 1);
    }
    op->Release();
    std::wstring path = PickSavePathAs(defName, g_updHwnd,
                                       L"所有文件 (*.*)\0*.*\0", L"",
                                       g_uiLangEn ? L"Save Download As" : L"保存下载文件");
    if (path.empty()) { a->put_Cancel(TRUE); return S_OK; }
    a->put_ResultFilePath(path.c_str());
    return S_OK;
  }
};

class UpdCtlHandler : public Cb<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* ctl) override {
    g_updCreating = false;
    if (FAILED(hr) || !ctl || !g_updHwnd) { LogMsg("[upd] popup controller failed"); return S_OK; }
    g_updController = ctl;
    ctl->AddRef();
    ctl->get_CoreWebView2(&g_updWebview);
    if (!g_updWebview) return S_OK;
    g_updWebview->AddRef();
    EventRegistrationToken tok = {};
    g_updWebview->add_NavigationCompleted(new UpdNavDoneHandler(), &tok);
    g_updWebview->add_HistoryChanged(new UpdHistHandler(), &tok);       
    UpdMaybeCleanData();   
    
    ICoreWebView2_4* wv2 = nullptr;
    if (SUCCEEDED(g_updWebview->QueryInterface(IID_PPV_ARGS(&wv2)))) {
      wv2->add_DownloadStarting(new UpdDownloadHandler(), &tok);
      wv2->Release();
    }
    SetWebviewBg(ctl, g_uiBg);
    RECT rc;
    GetClientRect(g_updHwnd, &rc);
    int bar = UpdBarH(g_updHwnd, rc.bottom);
    ctl->put_Bounds(RECT{0, bar, rc.right, rc.bottom});   
    ctl->put_IsVisible(TRUE);
    
    if (g_updSearchMode) g_updWebview->Navigate(L"https://www.bing.com/");
    else g_updWebview->Navigate(Utf8ToWide(std::string(kUpdateShareUrl)).c_str());
    
    if (g_env) g_env->CreateCoreWebView2Controller(g_updHwnd, new UpdToolCtlHandler());
    return S_OK;
  }
};

static LRESULT CALLBACK UpdWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CREATE:
      SetTimer(h, 2, 400, NULL);   
      return 0;
    case WM_TIMER:
      if (w == 2) {
        double z = 1.0;
        if (g_updController && SUCCEEDED(g_updController->get_ZoomFactor(&z)) && z != g_updZoom) {
          g_updZoom = z;
          if (g_updSuggestController) g_updSuggestController->put_IsVisible(FALSE);
          if (g_updSuggestWnd) ShowWindow(g_updSuggestWnd, SW_HIDE);
          g_updSuggestH = 0;
        }
      }
      return 0;
    case WM_SIZE: {
      RECT rc;
      GetClientRect(h, &rc);
      int bar = UpdBarH(h, rc.bottom);            
      
      if (g_updController) g_updController->put_Bounds(RECT{0, bar, rc.right, rc.bottom});
      if (g_updToolController) g_updToolController->put_Bounds(RECT{0, 0, rc.right, bar});
      
      if (g_updSuggestController) g_updSuggestController->put_IsVisible(FALSE);
      if (g_updSuggestWnd) ShowWindow(g_updSuggestWnd, SW_HIDE);
      g_updSuggestH = 0;
      UpdLayoutSuggest(0);
      return 0;
    }
    case WM_MOVE:
      UpdLayoutSuggest(g_updSuggestH);
      return 0;
    case WM_GETMINMAXINFO: {   
      MINMAXINFO* mm = (MINMAXINFO*)l;
      if (mm) { mm->ptMinTrackSize.x = GbPx(h, kUpdMinCssW); mm->ptMinTrackSize.y = GbPx(h, kUpdMinCssH); }
      return 0;
    }
    case WM_CLOSE:
      if (g_updSuggestController) g_updSuggestController->put_IsVisible(FALSE);
      if (g_updSuggestWnd) ShowWindow(g_updSuggestWnd, SW_HIDE);
      ShowWindow(h, SW_HIDE);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    default:
      return DefWindowProcW(h, m, w, l);
  }
  return DefWindowProcW(h, m, w, l);
}

static void OpenUpdateWindow(bool asSearch) {
  if (!g_env) { LogMsg("[upd] webview env not ready"); return; }
  g_updSearchMode = asSearch;
  
  if (!asSearch) WriteClipboardText(Utf8ToWide(std::string(kUpdateSharePwd)));
  if (!g_updHwnd) {
    if (g_updCreating) return;
    static const wchar_t* kUpdCls = L"GbUpdWnd";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = UpdWndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = kUpdCls;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    int w = GbPx(nullptr, kUpdDefCssW), hgt = GbPx(nullptr, kUpdDefCssH);
    GbClampToWorkArea(nullptr, w, hgt);   
    RECT mw = {0, 0, 0, 0};
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (g_hwnd && GetWindowRect(g_hwnd, &mw)) {
      x = mw.left + ((mw.right - mw.left) - w) / 2;
      y = mw.top + ((mw.bottom - mw.top) - hgt) / 3;
    }
    g_updHwnd = CreateWindowExW(0, kUpdCls,
                                asSearch ? L"全网搜索 · Web Search"
                                         : (g_uiLangEn ? L"Get Latest Version" : L"获取最新版本"),
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                x, y, w, hgt, nullptr, nullptr, g_hInst, nullptr);
    if (!g_updHwnd) { LogMsg("[upd] CreateWindowEx failed"); return; }
    
    g_updCreating = true;
    g_env->CreateCoreWebView2Controller(g_updHwnd, new UpdCtlHandler());
    LogMsg("[upd] embedded-browser window created (toolbar via 2nd WebView2)");
  } else {
    
    SetWindowTextW(g_updHwnd, asSearch ? L"全网搜索 · Web Search"
                                      : (g_uiLangEn ? L"Get Latest Version" : L"获取最新版本"));
    if (g_updWebview) {
      if (asSearch) g_updWebview->Navigate(L"https://www.bing.com/");
      else g_updWebview->Navigate(Utf8ToWide(std::string(kUpdateShareUrl)).c_str());
    }
  }
  ShowWindow(g_updHwnd, SW_SHOWNORMAL);
  SetForegroundWindow(g_updHwnd);
}

// ================================================================ 引擎层宿主回调
// engine_host.cpp 与宿主之间的**唯一**反向依赖（面小、易审计）。
static std::string SysInfoJson();   // 前置：OnVariantChanged 要在定义之前用它（定义在 4200 行附近）
namespace eng {
namespace host {

HANDLE Job() { return g_job; }
void PostPage(const std::string& json) { PostToPageSoon(json); }
void PostLink(const std::string& json) { PostToLinkPageSoon(json); }
void Log(const std::string& s) { LogMsg(s); }
std::wstring EngineDir() { return ExeDir(); }
void ChildTracked(HANDLE h) { ChildTrack(h); }
bool ApplyAffinity(HANDLE h) { return ApplyEngineAffinity(h); }
void PinHostAway() { CpuSplitPinHost(); }
void UnpinHost() { CpuSplitUnpinHost(); }
// ★ 2026-10-24（用户：「用户选择什么引擎，就在指令集卡片动态更新展示」）：
//   变体热切换（用户手选 / auto-bench 选优）后立刻重推 sysInfo —— 指令集卡片里的
//   「引擎变体 avx512vnni（原生引擎）」行随选择实时刷新，不用等重启。
void OnVariantChanged() { PostToPage(SysInfoJson()); }

int PhysCores() { return PhysCoreCount(); }
int LogicCores() { return CpuCount(); }
int ThreadsMaxHost() { return ThreadsMax(); }
long long TotalPhysKB() { return (long long)MemTotalMB() * 1024; }

}  // namespace host
}  // namespace eng

class MsgHandler : public Cb<ICoreWebView2WebMessageReceivedEventHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* e) override {
    LPWSTR raw = nullptr;
    if (FAILED(e->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
     std::string s = WideToUtf8(raw);
     CoTaskMemFree(raw);
     LogMsg("[msg] " + s.substr(0, 200));
    
    // ★ 2026-10-19 引擎调用层重建：页面只发一个 op（ask/stop/caps/config），
    //   协议编排与解析**全在宿主**（engine_host.cpp），宿主是唯一解析器。
    if (s.find("\"engine\"") != std::string::npos && s.find("\"op\":") != std::string::npos) {
      std::string op = JsonStrAfter(s, "\"op\":");
      int id = JsonIntAfter(s, "\"id\":");
      eng::HandleOp(op, id, s, false);
      // ★ 2026-10-24（用户：「用户选择什么引擎，就在指令集卡片动态更新展示」）：
      //   config（含引擎变体切换）后立刻重推 sysInfo，指令集卡片的「引擎变体」行实时刷新。
      if (op == "config") PostToPage(SysInfoJson());
      return S_OK;
    }
    if (s.find("\"engineConfig\"") != std::string::npos) {
      int t = JsonIntAfter(s, "\"threads\":");
      int h = JsonIntAfter(s, "\"hashMB\":");
      bool cfgChanged = (t != g_threads || h != g_hashMB);
      g_threads = t; g_hashMB = h;
      LogMsg("[ai] engineConfig threads=" + std::to_string(t) + " hashMB=" + std::to_string(h) + " (in-page)");
      // 用户偏好进宿主引擎层（THREAD_NUM / HASH_SIZE 与 clamp 都在那边）
      eng::SetUserPrefs(t, h);
      if (cfgChanged || g_linkCfgSeen == 0) {
        g_linkCfgSeen = 1;
        PostToLinkPageSoon(std::string("{\"type\":\"engineCfg\",\"threads\":") +
                           std::to_string(t) + ",\"hashMB\":" + std::to_string(h) + "}");
      }
      return S_OK;
    }

    if (s.find("\"ready\"") != std::string::npos) {
      if (!g_pending.empty()) {
        std::string p = g_pending; g_pending.clear();
        PostToPage(p);
      }
      
      SaveUiPrefs(s);
      g_uiDark = (JsonStrAfterDecoded(s, JKey("theme").c_str()) != "light");
      ApplyDarkTitleBar(g_hwnd, g_uiDark);
      RevealMainPage();
      KillTimer(g_hwnd, kMainTimerReload);
      LogBoot("PAGE READY");
      LogMsg("[ui] main page ready - splash removed");
      
      SetTimer(g_hwnd, kMainTimerHook, 50, nullptr);
      
      PrewarmReviewWindow();
      return S_OK;
    }
    
    if (s.find("uiTheme") != std::string::npos) {
      std::string th = JsonStrAfterDecoded(s, JKey("theme").c_str());
      if (th != "light" && th != "dark") th = "dark";
      
      std::string lg = JsonStrAfterDecoded(s, JKey("lang").c_str());
      if (lg == "en" || lg == "zh") g_uiLangEn = (lg == "en");
      SaveUiPrefs(s);
      g_uiDark = (th != "light");
      ApplyDarkTitleBar(g_hwnd, g_uiDark);
      ApplyDarkTitleBar(g_rvHwnd, g_uiDark);
      ApplyDarkTitleBar(g_visHwnd, g_uiDark);
      PostToRvPageSoon(std::string("{") + JKey("type") + JsonQuote(std::string("uiTheme")) + "," +
                       JKey("theme") + JsonQuote(th) + "," +
                       JKey("lang") + JsonQuote(g_uiLangEn ? "en" : "zh") + "}");
      PostToVisPageSoon(std::string("{") + JKey("type") + JsonQuote(std::string("uiTheme")) + "," +
                        JKey("theme") + JsonQuote(th) + "," +
                        JKey("lang") + JsonQuote(g_uiLangEn ? "en" : "zh") + "}");
      
      if (g_updToolWebview) {
        std::wstring tj = g_uiLangEn ? L"{\"lang\":1,\"dark\":" : L"{\"lang\":0,\"dark\":";
        tj += g_uiDark ? L"1}" : L"0}";
        g_updToolWebview->PostWebMessageAsJson(tj.c_str());
      }
      LogMsg("[ui] theme switched to " + th + " (all windows)");
      return S_OK;
    }
    
    if (s.find("\"ensureEngine\"") != std::string::npos) {
      
      LogMsg("[ai] ensureEngine requested (no host engine anymore, ignored)");
      return S_OK;
    }
    
    if (s.find("\"paste\"") != std::string::npos) {
      std::wstring txt = ReadClipboardText();
      LogMsg("[clip] paste -> " + std::to_string(txt.size()) + " chars");
      PostToPageSoon("{\"type\":\"clip\",\"text\":" + JsonQuote(WideToUtf8(txt)) + "}");
      return S_OK;
    }
    
    if (s.find("\"copy\"") != std::string::npos) {
      std::string t = JsonStrAfter(s, "\"text\":");
      if (!t.empty()) WriteClipboardText(Utf8ToWide(t));
      LogMsg("[clip] copy -> " + std::to_string(t.size()) + " chars");
      return S_OK;
    }
    
    if (s.find("\"checkUpdate\"") != std::string::npos) {
      OpenUpdateWindow(false);
      LogMsg("[update] share page window requested");
      return S_OK;
    }
    
    if (s.find("\"openShell\"") != std::string::npos) {
      OpenUpdateWindow(true);
      LogMsg("[shell] web search window requested");
      return S_OK;
    }
    
    if (s.find("\"openUrl\"") != std::string::npos) {
      std::string u = JsonStrAfter(s, "\"url\":");
      if (!u.empty() && (u.rfind("https://", 0) == 0 || u.rfind("http://", 0) == 0)) {
        ShellExecuteW(nullptr, L"open", Utf8ToWide(u).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        LogMsg("[update] open url -> " + u);
      } else {
        LogMsg("[update] open url rejected (not http/https)");
      }
      return S_OK;
    }
    
    if (s.find("\"openReview\"") != std::string::npos) {
      OpenReviewWindow(JsonSubObject(s, "\"record\""));
      return S_OK;
    }
    
    if (s.find("\"openVis\"") != std::string::npos) {
      
      if (g_linkHwnd) PostMessageW(g_linkHwnd, WM_CLOSE, 0, 0);
      OpenVisWindow();
      return S_OK;
    }
    
    if (s.find("\"openSettings\"") != std::string::npos) {
      OpenSetWindow();
      return S_OK;
    }
    if (s.find("\"mainSync\"") != std::string::npos) {
      // the main window changed settings (AI sides, think time, ...): the settings window
      // is the master panel and follows immediately
      std::string patch = JsonStrAfterDecoded(s, "\"patch\":");
      if (!patch.empty() && g_setWebview) {
        g_setWebview->PostWebMessageAsJson(
            Utf8ToWide("{\"type\":\"setSync\",\"patch\":" + patch + "}").c_str());
      }
      return S_OK;
    }
    if (s.find("\"openLink\"") != std::string::npos) {
      
      if (g_visHwnd) PostMessageW(g_visHwnd, WM_CLOSE, 0, 0);
      OpenLinkWindow();
      return S_OK;
    }
    
    // ★ 2026-10-16（用户：「变规则的时候不能顺利的切换规则而评估」）：
    //   主窗口改规则时 calc.js 会发 {type:'linkCfgSync', rule:N}（见 sel_rule.onchange /
    //   setLinkMode），但 MsgHandler 里**从来没有这个分支** —— 消息被静默吞掉，连线窗
    //   永远不知道主界面把规则改了，继续按旧规则算平衡点与评估。这里补上转发：
    //   连线页 calc.js 收到后 lkBridgeRule() → 高亮 + BS.rule 同步 → bridge.js 清置换表重算。
    if (s.find("\"linkCfgSync\"") != std::string::npos) {
      std::string payload = s;
      size_t p = payload.find('{');
      if (p != std::string::npos) payload = payload.substr(p);
      PostToLinkPageSoon(payload);
      LogMsg("[link] rule sync main -> link: " + payload.substr(0, 120));
      return S_OK;
    }
    
    if (s.find("\"linkDrop\"") != std::string::npos) {
      int lx = JsonIntAfter(s, "\"x\":"), ly = JsonIntAfter(s, "\"y\":");
      
      bool lforce = s.find("\"force\":true") != std::string::npos;
      
      bool lreal = s.find("\"real\":true") != std::string::npos;
      LogMsg("[link] AI drop received at (" + std::to_string(lx) + "," + std::to_string(ly) +
             (lforce ? ") [force]" : "") + (lreal ? "[real]" : "") + " -> target click");
      if (lx >= 0 && lx < 25 && ly >= 0 && ly < 25) LinkAutoDropAt(lx, ly, lforce, lreal);
      return S_OK;
    }
    
    if (s.find("\"linkStats\"") != std::string::npos ||
        s.find("\"linkEval\"") != std::string::npos) {
      std::string payload = s;
      size_t pp = payload.find('{');
      if (pp != std::string::npos) payload = payload.substr(pp);
      PostToLinkPageSoon(payload);
      
      if (s.find("\"linkStats\"") != std::string::npos) LinkParseBest(s);
      return S_OK;
    }
    
    if (s.find("\"savePng\"") != std::string::npos) {
      SavePngFromPage(s, g_hwnd);
      return S_OK;
    }
    
    if (s.find("\"saveTxt\"") != std::string::npos) {
      TxtSaveResult r = SaveTxtFromPage(s, g_hwnd);
      if (r.ok) {
        PostToPage("{\"type\":\"histExported\",\"n\":" + std::to_string(r.n) + "}");
        
        if (TestFlag("GB_TEST_HIST_ROUNDTRIP")) PostToPage("{\"type\":\"testHistNowImport\"}");
      }
      return S_OK;
    }
    
    if (s.find("\"openTxt\"") != std::string::npos) {
      std::string m = ReadTxtMessage(g_hwnd);
      if (!m.empty()) PostToPage(m);
      return S_OK;
    }
    
    if (s.find("\"histImported\"") != std::string::npos) {
      LogMsg("[hist] imported n=" + std::to_string(JsonIntAfter(s, "\"n\":")) +
             " fp=" + JsonStrAfter(s, "\"fp\":"));
      return S_OK;
    }
    return S_OK;
  }
};

class PermHandler : public Cb<ICoreWebView2PermissionRequestedEventHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*,
                                   ICoreWebView2PermissionRequestedEventArgs* e) override {
    COREWEBVIEW2_PERMISSION_KIND k = COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
    if (SUCCEEDED(e->get_PermissionKind(&k)) && k == COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ) {
      e->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
    }
    return S_OK;
  }
};

static void RemoveDirRecursive(const std::wstring& dir) {
  WIN32_FIND_DATAW fd = {};
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    std::wstring p = dir + L"\\" + name;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirRecursive(p);
    else DeleteFileW(p.c_str());
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  RemoveDirectoryW(dir.c_str());
}

static long long DirSizeRecursive(const std::wstring& dir) {
  long long sum = 0;
  WIN32_FIND_DATAW fd = {};
  HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") continue;
    std::wstring p = dir + L"\\" + name;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) sum += DirSizeRecursive(p);
    else sum += ((long long)fd.nFileSizeHigh << 32) | (long long)fd.nFileSizeLow;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return sum;
}

// ★ 2026-10-16（用户：「Webview 渲染进程使用最新最科学的渲染方式」）：
//   原来**每次启动**都无条件删掉 Cache / Code Cache / GPUCache / ShaderCache。
//   那是「渲染不科学」的经典反例：Code Cache 一没，整个 calc.js（32 万字节）首次
//   解析/编译全部重来；GPUCache + ShaderCache 一没，GPU 进程每次都要重建着色器缓存，
//   轻则首屏慢、重则 GPU 进程起不来直接退软渲染。改为**按体积阈值**清理：
//   只有缓存涨到 96 MB 以上才清（长期使用仍然不会无限膨胀），正常启动一律保留热缓存。
static const long long kCachePurgeMinBytes = 96LL * 1024 * 1024;
static void ClearWebViewCache(const std::wstring& udf, const char* when) {
  if (udf.empty()) return;
  static const wchar_t* kDirs[] = {
    L"Cache", L"Code Cache", L"GPUCache", L"ShaderCache", L"VideoDecodeStats",
    L"blob_storage", L"Service Worker\\CacheStorage", L"Service Worker\\ScriptCache",
  };
  long long total = 0;
  for (const wchar_t* d : kDirs) total += DirSizeRecursive(Join(udf, d));
  if (total < kCachePurgeMinBytes) {
    LogMsg("[wv2] " + std::string(when) + " 缓存 " + std::to_string(total / (1024 * 1024)) +
           " MB < " + std::to_string(kCachePurgeMinBytes / (1024 * 1024)) +
           " MB 阈值，保留（热 Code/GPU/Shader 缓存 = 首屏更快、无需重建着色器）");
    return;
  }
  long long freed = 0;
  int n = 0;
  for (const wchar_t* d : kDirs) {
    std::wstring p = Join(udf, d);
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    long long sz = DirSizeRecursive(p);
    RemoveDirRecursive(p);
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) { freed += sz; n++; }
  }
  if (n > 0)
    LogMsg("[wv2] " + std::string(when) + " 清掉 WebView2 缓存 " + std::to_string(n) +
           " 项目，约 " + std::to_string(freed / (1024 * 1024)) + " MB");
}

static std::string SysInfoJson() {
  std::string o = "{\"type\":\"sysInfo\"";

  o += ",\"cpuName\":" + JsonQuote(CpuName());
  o += ",\"physCores\":" + std::to_string(PhysCoreCount());
  o += ",\"logiCores\":" + std::to_string((int)std::thread::hardware_concurrency());
  DWORD mhz = 0;
  if (RegDword(HKEY_LOCAL_MACHINE,
               L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz", mhz) != ERROR_SUCCESS)
    mhz = 0;
  o += ",\"mhz\":" + std::to_string((int)mhz);

  eng::Caps ec = eng::GetCaps();
  o += std::string(",\"isa\":{\"avx2\":") + (ec.hasAVX2 ? "true" : "false") +
       ",\"avx512\":" + (ec.hasAVX512 ? "true" : "false") +
       ",\"avx512vnni\":" + (ec.hasAVX512VNNI ? "true" : "false") +
       ",\"avxvnni\":" + (ec.hasAVXVNNI ? "true" : "false") + "}";


  int l1 = 0, l2 = 0, l3 = 0;
  {
    DWORD len = 0;
    if (!GetLogicalProcessorInformationEx(RelationCache, nullptr, &len) &&
        GetLastError() == ERROR_INSUFFICIENT_BUFFER && len > 0) {
      std::vector<char> buf(len);
      if (GetLogicalProcessorInformationEx(RelationCache,
              (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len)) {
        DWORD off = 0;
        while (off < len) {
          PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX pi =
              (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
          if (pi->Relationship == RelationCache) {
            int kb = (int)(pi->Cache.CacheSize / 1024);
            if (pi->Cache.Level == 1 && kb > l1) l1 = kb;
            else if (pi->Cache.Level == 2 && kb > l2) l2 = kb;
            else if (pi->Cache.Level == 3 && kb > l3) l3 = kb;
          }
          if (pi->Size == 0) break;
          off += pi->Size;
        }
      }
    }
  }
  o += std::string(",\"cache\":{\"l1\":") + std::to_string(l1) +
       ",\"l2\":" + std::to_string(l2) + ",\"l3\":" + std::to_string(l3) + "}";

  MEMORYSTATUSEX ms = {};
  ms.dwLength = sizeof(ms);
  GlobalMemoryStatusEx(&ms);
  o += ",\"memTotalMB\":" + std::to_string((long long)(ms.ullTotalPhys / (1024 * 1024)));
  o += ",\"memAvailMB\":" + std::to_string((long long)(ms.ullAvailPhys / (1024 * 1024)));
  o += ",\"memLoad\":" + std::to_string((int)ms.dwMemoryLoad);

  HKEY hk = nullptr;
  std::wstring prod, build;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hk) == ERROR_SUCCESS && hk) {
    wchar_t b1[256] = {0}; DWORD sz = sizeof(b1), ty = 0;
    if (RegQueryValueExW(hk, L"ProductName", nullptr, &ty, (LPBYTE)b1, &sz) == ERROR_SUCCESS) prod = b1;
    wchar_t b2[64] = {0}; sz = sizeof(b2);
    if (RegQueryValueExW(hk, L"CurrentBuildNumber", nullptr, &ty, (LPBYTE)b2, &sz) == ERROR_SUCCESS) build = b2;
    else { DWORD nb = 0; if (RegDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber", nb) == ERROR_SUCCESS) build = std::to_wstring(nb); }
    RegCloseKey(hk);
  }
  o += ",\"osName\":" + JsonQuote(WideToUtf8(prod));
  o += ",\"osBuild\":" + JsonQuote(WideToUtf8(build));

  {
    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t root[4] = {exe[0], L':', L'\\', 0};
    ULARGE_INTEGER freeBytes = {}, totalBytes = {}, totalFree = {};
    if (root[0] && GetDiskFreeSpaceExW(root, &freeBytes, &totalBytes, &totalFree))
      o += ",\"diskFreeGB\":" + std::to_string((long long)(totalFree.QuadPart / (1024ull * 1024 * 1024)));
    else
      o += ",\"diskFreeGB\":0";
  }

  o += ",\"variant\":" + JsonQuote(ec.variant);
  // ★ 2026-10-24：本机实测最快变体（auto-bench 缓存，空 = 未测）——指令集卡片标注「实测最快」
  o += ",\"variantBench\":" + JsonQuote(ec.benchVariant);
  o += ",\"native\":" + std::string(ec.available ? "true" : "false");

  o += "}";
  return o;
}

static std::string SysMemJson() {
  MEMORYSTATUSEX ms;
  ms.dwLength = sizeof(ms);
  int totalMB = 0, availMB = 0, load = 0;
  if (GlobalMemoryStatusEx(&ms)) {
    totalMB = (int)(ms.ullTotalPhys / (1024ull * 1024ull));
    availMB = (int)(ms.ullAvailPhys / (1024ull * 1024ull));
    load = (int)ms.dwMemoryLoad;
  }
  return std::string("{\"type\":\"sysMem\",\"memTotalMB\":") + std::to_string(totalMB) +
         ",\"memAvailMB\":" + std::to_string(availMB) +
         ",\"memLoad\":" + std::to_string(load) + "}";
}

// host handshake: CPU/engine capability snapshot shared by the main page and the settings page
static std::string HostHandshakeJson() {
  eng::Caps ec = eng::GetCaps();
  char b[640];
  snprintf(b, sizeof(b),
           "{\"type\":\"host\",\"cpu\":%d,\"threadsDefault\":%d,\"threadsMax\":%d,"
           "\"physCores\":%d,\"memMB\":%d,\"memAvailMB\":%d,\"native\":%s,"
           // 用户线程/哈希偏好随握手下发，页面以宿主为唯一事实来源（0 = 未设置）
           "\"userThreads\":%d,\"userHashMB\":%d,"
           "\"variant\":%s,\"cpuName\":%s,"
           // ★ 2026-10-24（用户：「引擎变体仍然无法手动切换」）：CPU 指令集能力随 host 握手一起下发。
           //   之前页面只能靠 `engine caps`（要先等引擎探测/冒烟，3~8 秒），经常拿不到 ⇒
           //   变体下拉永远只剩「自动」。host 消息是页面 ready 后立刻发的，同步可达。
           "\"sse\":%s,\"avx2\":%s,\"avx512\":%s,\"avx512vnni\":%s,\"avxvnni\":%s,"
           "\"variantBench\":%s}",
           CpuCount(), ThreadsDefault(), ThreadsMax(), PhysCoreCount(),
           MemTotalMB(), MemAvailMB(),
           ec.available ? "true" : "false",
           ec.userThreads, ec.userHashMB,
           JsonQuote(ec.variant).c_str(), JsonQuote(CpuName()).c_str(),
           ec.hasSSE ? "true" : "false", ec.hasAVX2 ? "true" : "false",
           ec.hasAVX512 ? "true" : "false", ec.hasAVX512VNNI ? "true" : "false",
           ec.hasAVXVNNI ? "true" : "false",
           JsonQuote(ec.benchVariant).c_str());

  LogMsg(std::string("[host] probe cpu=") + std::to_string(CpuCount()) +
         " threadsDefault=" + std::to_string(ThreadsDefault()) +
         " threadsMax=" + std::to_string(ThreadsMax()) +
         " phys=" + std::to_string(PhysCoreCount()) +
         " smt=" + std::to_string(PhysCoreCount() > 0 ? CpuCount() / PhysCoreCount() : 0) +
         " vendor=" + CpuVendor() +
         " hybrid=" + std::to_string(HasHybridCores() ? 1 : 0) +
         " memMB=" + std::to_string(MemTotalMB()) +
         " hw=" + std::to_string((int)std::thread::hardware_concurrency()));
  return std::string(b);
}

class DomHandler : public Cb<ICoreWebView2DOMContentLoadedEventHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2DOMContentLoadedEventArgs*) override {

    PostToPage(HostHandshakeJson());
    PostToPage(SysInfoJson());   
    
    if (g_hwnd && IsWindow(g_hwnd)) SetTimer(g_hwnd, kMainTimerSysMem, kSysMemIntervalMs, nullptr);
    // pre-create the settings window shortly after boot so the first open is instant
    if (g_hwnd && IsWindow(g_hwnd)) SetTimer(g_hwnd, kMainTimerPrewarmSet, 1500, nullptr);
    LogBoot("dom content loaded");
    FireTestHooks();
    return S_OK;
  }
};

static bool g_testHooksFired = false;
static void FireTestHooks() {
  if (g_testHooksFired) return;
  g_testHooksFired = true;
  
  char tv[8] = {0};
  
  if (GetEnvironmentVariableA("GB_TEST_OPEN_REVIEW", tv, sizeof(tv)) && tv[0] == '1') {
    char dv[16] = {0};
    UINT delayMs = 0;
    if (GetEnvironmentVariableA("GB_TEST_OPEN_REVIEW_DELAY_MS", dv, sizeof(dv))) {
      delayMs = (UINT)atoi(dv);
    }
    if (delayMs && g_hwnd && IsWindow(g_hwnd)) {
      SetTimer(g_hwnd, kRvTimerTestOpen, delayMs, nullptr);
      LogMsg("[rv] test hook: opening a review window in " + std::to_string(delayMs) + " ms");
    } else {
      PostToPage("{\"type\":\"testOpenReview\"}");
      LogMsg("[rv] test hook: asked the page to open a review window");
    }
  }
  memset(tv, 0, sizeof(tv));
  if (GetEnvironmentVariableA("GB_TEST_OPEN_SETTINGS", tv, sizeof(tv)) && tv[0] == '1') {
    OpenSetWindow();
    LogMsg("[set] test hook: settings window opened");
  }
  memset(tv, 0, sizeof(tv));
  if (GetEnvironmentVariableA("GB_TEST_SET_MINIMIZE", tv, sizeof(tv)) && tv[0] == '1') {
    UINT delayMs = 4000;                 // the settings window prewarm lands ~1.5 s after boot
    char dv[16] = {0};
    if (GetEnvironmentVariableA("GB_TEST_SET_MINIMIZE_DELAY_MS", dv, sizeof(dv)) && atoi(dv) > 0)
      delayMs = (UINT)atoi(dv);
    SetTimer(g_hwnd, kMainTimerTestSetMin, delayMs, nullptr);
    LogMsg("[set] test hook: minimizing the settings window in " + std::to_string(delayMs) + " ms");
  }
  memset(tv, 0, sizeof(tv));
  if (GetEnvironmentVariableA("GB_TEST_SAVE_POS", tv, sizeof(tv)) && tv[0] == '1') {
    PostToPage("{\"type\":\"testSavePos\"}");
    LogMsg("[save] test hook: asked the page to save the position");
  }
  
  memset(tv, 0, sizeof(tv));
  if (GetEnvironmentVariableA("GB_TEST_HIST_ROUNDTRIP", tv, sizeof(tv)) && tv[0] == '1') {
    PostToPage("{\"type\":\"testHistRoundtrip\"}");
    LogMsg("[hist] test hook: asked the page to export the history as txt");
  }
  
  memset(tv, 0, sizeof(tv));
  if (GetEnvironmentVariableA("GB_TEST_AI_BOTH", tv, sizeof(tv)) && tv[0] == '1') {
    PostToPage("{\"type\":\"testAiBoth\"}");
    LogMsg("[ai] test hook: asked the page to run the AI-vs-AI self-play walkthrough");
  }

  // ★ 2026-10-16：连线链路 E2E 测试钩子（无需真实对局窗口）——
  //   GB_TEST_OPEN_LINK=1：延时打开连线窗，页面就绪后投「我执黑 + 合成 4B4W 扫描帧」，
  //   走真实 analyze → 引擎 ack → engineOut 推送 → 推荐落点 全链路。
  memset(tv, 0, sizeof(tv));
  if (GetEnvironmentVariableA("GB_TEST_OPEN_LINK", tv, sizeof(tv)) && tv[0] == '1') {
    UINT delayMs = 2500;
    char dv[16] = {0};
    if (GetEnvironmentVariableA("GB_TEST_LINK_DELAY_MS", dv, sizeof(dv))) delayMs = (UINT)atoi(dv);
    if (delayMs < 500) delayMs = 500;
    if (g_hwnd && IsWindow(g_hwnd)) {
      SetTimer(g_hwnd, kMainTimerTestLink, delayMs, nullptr);
      LogMsg("[test] GB_TEST_OPEN_LINK: link window opens in " + std::to_string(delayMs) + " ms");
    }
  }

  // ★ 2026-10-17：持久化回归钩子 —— 把一段 JS 注入主页面（用于「改下拉 → 重启 → 值还在」这类
  //   只能真机验证的场景）。只在设了 GB_TEST_EVAL 时生效，正常启动零开销。
  {
    char ev[2048] = {0};
    if (GetEnvironmentVariableA("GB_TEST_EVAL", ev, sizeof(ev)) && ev[0] && g_webview) {
      g_webview->ExecuteScript(Utf8ToWide(ev).c_str(), nullptr);
      LogMsg("[test] GB_TEST_EVAL injected (" + std::to_string(strlen(ev)) + " chars)");
    }
  }
}

static unsigned long long UiStamp() {
  static unsigned long long stamp = 0;
  if (!stamp) {
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (GetFileAttributesExW(exe, GetFileExInfoStandard, &fa))
      stamp = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) |
              (unsigned long long)fa.ftLastWriteTime.dwLowDateTime;
    if (!stamp) stamp = 1;
  }
  return stamp;
}

static std::wstring MainNavUrl(const wchar_t* def) {
  char nu[300] = { 0 };
  if (GetEnvironmentVariableA("GB_NAV_URL", nu, sizeof(nu)) && nu[0]) return Utf8ToWide(nu);
  std::wstring url(def);
  url += (url.find(L'?') == std::wstring::npos) ? L'?' : L'&';
  url += L"v=" + std::to_wstring(UiStamp());
  return url;
}

class NavHandler : public Cb<ICoreWebView2NavigationCompletedEventHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*,
                                   ICoreWebView2NavigationCompletedEventArgs* e) override {
    BOOL ok = FALSE;
    if (e) e->get_IsSuccess(&ok);
    if (ok) {
      LogBoot("navigation completed");
    } else {
      COREWEBVIEW2_WEB_ERROR_STATUS es = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
      if (e) e->get_WebErrorStatus(&es);
      LogMsg("[boot] navigation FAILED err=" + std::to_string((int)es));
    }
    return S_OK;
  }
};

class ProcFailHandler : public Cb<ICoreWebView2ProcessFailedEventHandler> {
 public:
  explicit ProcFailHandler(std::wstring url) : url_(std::move(url)) {}
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* e) override {
    COREWEBVIEW2_PROCESS_FAILED_KIND k = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
    if (e) e->get_ProcessFailedKind(&k);
    if (k == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE ||
        k == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED) {
      LogMsg("[wv2] render process died (kind=" + std::to_string((int)k) + ") - re-navigating");
      if (sender) sender->Navigate(url_.c_str());
    }
    return S_OK;
  }
 private:
  std::wstring url_;
};

static std::wstring UiBaseUrl(const char* tag) {
  std::wstring url = L"https://gbcalc.test/calc.html";
  if (tag && std::string(tag) == "rv")  url += L"?rv=1";
  if (tag && std::string(tag) == "vis") url += L"?vis=1";
  
  if (tag && std::string(tag) == "link") url += L"?link=1";
  if (tag && std::string(tag) == "set") url += L"?set=1";
  return url;
}

static void SetupUiServing(ICoreWebView2* wv, const char* tag) {
  std::wstring base = UiBaseUrl(tag);
  
  EventRegistrationToken rtok = {};
  HRESULT rh = wv->add_WebResourceRequested(&g_resHandler, &rtok);
  HRESULT hf = wv->AddWebResourceRequestedFilter(L"https://gbcalc.test/*",
                                                 COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
  char bb[220];
  snprintf(bb, sizeof(bb),
           "[%s] UI via in-process interceptor (zero-TCP hardlink) add=0x%08lX filter=0x%08lX base=%ls",
           tag ? tag : "wv2", (unsigned long)rh, (unsigned long)hf, base.c_str());
  LogMsg(bb);
  EventRegistrationToken ptok = {};
  wv->add_ProcessFailed(new ProcFailHandler(base), &ptok);
}

// （前向声明：定义在 EnvHandler 之前，但 CtlHandler 里就要用）
static void HardenWebviewSettings(ICoreWebView2* wv, const char* tag);

class CtlHandler : public Cb<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* ctl) override {
    if (FAILED(hr) || !ctl) { LogMsg("[wv2] controller creation failed"); return S_OK; }
    g_controller = ctl;
    ctl->AddRef();
    ctl->get_CoreWebView2(&g_webview);
    if (g_webview) {
      g_webview->AddRef();
      HardenWebviewSettings(g_webview, "wv2");
      
      ICoreWebView2_2* wv2 = nullptr;
      if (SUCCEEDED(g_webview->QueryInterface(__uuidof(ICoreWebView2_2), (void**)&wv2)) && wv2) {
        EventRegistrationToken t = {};
        wv2->add_DOMContentLoaded(new DomHandler(), &t);
        wv2->Release();
      }
      EventRegistrationToken tok = {};
      g_webview->add_WebMessageReceived(new MsgHandler(), &tok);
      EventRegistrationToken ntok = {};
      g_webview->add_NavigationCompleted(new NavHandler(), &ntok);
      EventRegistrationToken ptok = {};
      g_webview->add_PermissionRequested(new PermHandler(), &ptok);
      
      SetupUiServing(g_webview, "wv2");
      LogBoot("controller ready");
      g_webview->Navigate(MainNavUrl(UiBaseUrl(nullptr).c_str()).c_str());
      LogMsg("[wv2] navigated to " + WideToUtf8(MainNavUrl(UiBaseUrl(nullptr).c_str())));   
    }
    
    SetWebviewBg(ctl, g_uiBg);
    ctl->put_IsVisible(TRUE);
    LayoutWebView();
    return S_OK;
  }
};

// ★ 2026-10-16（用户：「Webview 渲染进程使用最新最科学的渲染方式」）：
//   WebView2 的三类「科学渲染 / 不节流」开关。以前 CreateCoreWebView2EnvironmentWithOptions
//   第 3 参是 nullptr ⇒ 完全走 Chromium 默认：桌面应用里默认值恰恰是最差的一组 ——
//   （1）被遮挡/最小化的窗口会被判定为 occluded/backgrounded，渲染进程降优先级、定时器
//       被节流到 1Hz 甚至冻结。本程序的主界面经常被「连线选框窗 / 识别窗 / 复盘窗」盖住，
//       bridge.js 的 60ms 轮询、waitForMove 的 5ms 轮询一旦被节流 ⇒ 引擎明明在算、
//       页面却读不到输出 = 用户报的「引擎连不上 / 迟迟不评估」。
//   （2）canvas 曲线/棋盘走软件光栅，引擎吃满 CPU 时界面明显掉帧。
//   这里显式关掉节流、开 GPU 光栅 + 零拷贝，并禁掉 Windows 原生遮挡计算（该特性会暂停
//   被遮窗口的画面提交，对「叠加窗压在主窗上」这种布局是纯负作用）。
//   另可用 GB_WV2_ARGS 环境变量整体覆盖（排查用），或 GB_WV2_ARGS_EXTRA 追加。
static std::wstring GbWv2BrowserArgs() {
  auto getenvW = [](const char* n) -> std::wstring {
    char b[2048] = {0};
    DWORD n2 = GetEnvironmentVariableA(n, b, sizeof(b));
    return (n2 > 0 && n2 < sizeof(b)) ? Utf8ToWide(std::string(b, n2)) : std::wstring();
  };
  std::wstring over = getenvW("GB_WV2_ARGS");
  // GB_WV2_ARGS=none ⇒ 显式「不带任何附加参数」（排查用；空值会被 Windows 当成未设置）
  if (_wcsicmp(over.c_str(), L"none") == 0) return L"";
  if (!over.empty()) return over;
  // 注意：Chromium 只认**最后一个** --enable-features / --disable-features，
  //       所以每类只能出现一次（多特性用逗号连）。
  std::wstring a =
      L"--disable-background-timer-throttling"          // setInterval/setTimeout 不被降频
      L" --disable-backgrounding-occluded-windows"      // 被遮挡也保持正常优先级
      L" --disable-renderer-backgrounding"              // 后台/遮挡不降渲染优先级
      L" --disable-features=CalculateNativeWinOcclusion" // 遮挡不暂停画面提交
      L" --enable-gpu-rasterization"                    // GPU 光栅（棋盘/曲线更顺）
      L" --enable-zero-copy"                            // 零拷贝合成
      L" --enable-features=CanvasOopRasterization"      // canvas 走 OOP 光栅
      L" --ignore-gpu-blocklist";                       // 别因驱动版本号被降级到软渲染
  std::wstring extra = getenvW("GB_WV2_ARGS_EXTRA");
  if (!extra.empty()) { a += L" "; a += extra; }
  return a;
}

// ICoreWebView2EnvironmentOptions 的最小实现（只用来塞 AdditionalBrowserArguments）。
// ★ TargetCompatibleBrowserVersion 必须给一个**低于本机运行时**的值：给高了会让
//   环境创建直接失败（整套 UI 起不来），给低了只是「不排除任何已装运行时」。
class EnvOptions : public ICoreWebView2EnvironmentOptions {
 public:
  explicit EnvOptions(std::wstring args) : m_args(std::move(args)) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_rc); }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = InterlockedDecrement(&m_rc);
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    if (riid == __uuidof(ICoreWebView2EnvironmentOptions) || riid == IID_IUnknown) {
      *pp = static_cast<ICoreWebView2EnvironmentOptions*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE get_AdditionalBrowserArguments(LPWSTR* value) override {
    return Dup(value, m_args);
  }
  HRESULT STDMETHODCALLTYPE put_AdditionalBrowserArguments(LPCWSTR value) override {
    m_args = value ? value : L"";
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_Language(LPWSTR* value) override { return Dup(value, L""); }
  HRESULT STDMETHODCALLTYPE put_Language(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE get_TargetCompatibleBrowserVersion(LPWSTR* value) override {
    return Dup(value, L"100.0.0.0");
  }
  HRESULT STDMETHODCALLTYPE put_TargetCompatibleBrowserVersion(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL* allow) override {
    if (allow) *allow = FALSE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL) override { return S_OK; }
  std::wstring args() const { return m_args; }

 private:
  static HRESULT Dup(LPWSTR* value, const std::wstring& s) {
    if (!value) return E_POINTER;
    size_t n = s.size() + 1;
    *value = (LPWSTR)CoTaskMemAlloc(n * sizeof(wchar_t));
    if (!*value) return E_OUTOFMEMORY;
    memcpy(*value, s.c_str(), n * sizeof(wchar_t));
    return S_OK;
  }
  ULONG m_rc = 1;
  std::wstring m_args;
};

// 每个 WebView2 都套一遍：统一「关掉不需要的内建 UI/交互 + 保持工程化缩放口径」。
// 只加**不会改变现有交互**的项：状态栏、右键菜单、缩放、捏合、滑动导航、自动填充、
// 拖放文件、内建错误页。刻意**不**动 AreBrowserAcceleratorKeysEnabled（关了会连
// Ctrl+C 一起废掉，「复制代码」按钮的键盘通路就没了）。
static void HardenWebviewSettings(ICoreWebView2* wv, const char* tag) {
  if (!wv) return;
  ICoreWebView2Settings* st = nullptr;
  if (FAILED(wv->get_Settings(&st)) || !st) return;
  st->put_IsStatusBarEnabled(FALSE);
  st->put_AreDefaultContextMenusEnabled(FALSE);
  ICoreWebView2Settings3* st3 = nullptr;
  if (SUCCEEDED(st->QueryInterface(__uuidof(ICoreWebView2Settings3), (void**)&st3)) && st3) {
    st3->put_IsZoomControlEnabled(FALSE);
    st3->Release();
  }
  ICoreWebView2Settings4* st4 = nullptr;
  if (SUCCEEDED(st->QueryInterface(__uuidof(ICoreWebView2Settings4), (void**)&st4)) && st4) {
    st4->put_IsGeneralAutofillEnabled(FALSE);
    st4->put_IsPasswordAutosaveEnabled(FALSE);
    st4->Release();
  }
  ICoreWebView2Settings5* st5 = nullptr;
  if (SUCCEEDED(st->QueryInterface(__uuidof(ICoreWebView2Settings5), (void**)&st5)) && st5) {
    st5->put_IsPinchZoomEnabled(FALSE);
    st5->Release();
  }
  ICoreWebView2Settings6* st6 = nullptr;
  if (SUCCEEDED(st->QueryInterface(__uuidof(ICoreWebView2Settings6), (void**)&st6)) && st6) {
    st6->put_IsSwipeNavigationEnabled(FALSE);
    st6->Release();
  }
  st->Release();
  LogMsg(std::string("[wv2] ") + (tag ? tag : "?") + " settings hardened (throttle-free render)");
}

class EnvHandler : public Cb<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment* env) override {
    if (FAILED(hr) || !env) {
      char hrbuf[24] = {0};
      sprintf_s(hrbuf, "0x%08X", (unsigned)hr);
      LogMsg(std::string("[wv2] environment creation failed - is WebView2 Runtime installed? hr=") + hrbuf);
      // ★ 2026-10-13（用户：「确保在其他电脑都可以正常且顺利的运行（Windows 10 以上）」）：
      //   旧代码只写一行日志就 return —— 在没装 WebView2 运行时的机器上（Win10 精简版 /
      //   LTSC / 从未装过 Edge），用户只会看到一个空白死窗口，既不知道发生了什么也不知道
      //   怎么修；安装程序里点「否」跳过运行时检测也会走到这里。这里给一个明确的中英双语
      //   提示 + 一键打开官方下载页（与安装脚本 MeterGomokuLink.iss 用的是同一个链接）。
      static const wchar_t* kUrl = L"https://go.microsoft.com/fwlink/p/?LinkId=2124703";
      std::wstring msg =
          L"本软件需要「Microsoft Edge WebView2 运行时」。\n"
          L"Windows 11 及绝大多数 Windows 10 已随 Edge 预装，本机未检测到。\n\n"
          L"点「是」立即打开官方下载页，装完后重新启动本软件即可。\n\n"
          L"This app needs the Microsoft Edge WebView2 Runtime, which was not found on "
          L"this machine.\nClick Yes to open the official download page, then restart the "
          L"app.\n\n";
      msg += kUrl;
      int r = MessageBoxW(g_hwnd, msg.c_str(),
                          L"Meter Gomoku - 缺少 WebView2 运行时 / WebView2 Runtime missing",
                          MB_ICONERROR | MB_YESNO | MB_SETFOREGROUND);
      if (r == IDYES)
        ShellExecuteW(nullptr, L"open", kUrl, nullptr, nullptr, SW_SHOWNORMAL);
      return S_OK;
    }
    g_env = env;
    env->AddRef();
    LogBoot("wv2 env ready");
    // ★ 2026-10-16：把真正在跑的 WebView2 运行时版本记进日志 —— 「最新最科学的渲染方式」
    //   这件事必须可证：日志里能直接看到渲染内核版本，而不是靠猜。
    {
      LPWSTR bv = nullptr;
      if (SUCCEEDED(env->get_BrowserVersionString(&bv)) && bv) {
        LogMsg("[wv2] runtime " + WideToUtf8(bv));
        CoTaskMemFree(bv);
      }
      LogMsg("[wv2] browser args: " + WideToUtf8(GbWv2BrowserArgs()));
    }
    env->CreateCoreWebView2Controller(g_hwnd, new CtlHandler());
    return S_OK;
  }
};

static std::wstring UiFile() {
  wchar_t tmp[MAX_PATH] = {0};
  GetTempPathW(MAX_PATH, tmp);
  return Join(std::wstring(tmp), L"GomokuTrainer.ui");
}

static void LoadUiPrefs() {
  FILE* f = _wfopen(UiFile().c_str(), L"rb");
  if (!f) return;
  char buf[128] = {0};
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  std::string s(buf, n);
  std::string theme = "dark", bg, lang = "zh";
  size_t p1 = s.find('|');
  if (p1 != std::string::npos) {
    theme = s.substr(0, p1);
    size_t p2 = s.find('|', p1 + 1);
    if (p2 != std::string::npos) {
      bg = s.substr(p1 + 1, p2 - p1 - 1);
      lang = s.substr(p2 + 1);
    } else {
      bg = s.substr(p1 + 1);
    }
  }
  while (!lang.empty() && (unsigned char)lang[lang.size() - 1] <= 32) lang.erase(lang.size() - 1);
  g_uiDark = (theme != "light");
  g_uiFg = g_uiDark ? RGB(0x9a, 0xa0, 0xa6) : RGB(0x8a, 0x8a, 0x86);
  if (bg.size() >= 7 && bg[0] == '#') {
    unsigned r = 0, g = 0, b = 0;
    if (sscanf(bg.substr(0, 7).c_str(), "#%2x%2x%2x", &r, &g, &b) == 3) {
      g_uiBg = RGB((BYTE)r, (BYTE)g, (BYTE)b);
    }
  }
  g_uiText = (lang.rfind("en", 0) == 0) ? L"Loading…" : L"加载中…";
  
  g_uiLangEn = (lang.rfind("en", 0) == 0);
  LogMsg("[ui] splash prefs loaded (theme=" + theme + " bg=" + bg + " lang=" + lang + ")");
}

static void SaveUiPrefs(const std::string& s) {
  std::string theme = JsonStrAfterDecoded(s, JKey("theme").c_str());
  std::string bg = JsonStrAfterDecoded(s, JKey("bg").c_str());
  std::string lang = JsonStrAfterDecoded(s, JKey("lang").c_str());
  if (!lang.empty()) g_uiLangEn = (lang.rfind("en", 0) == 0);
  if (theme != "light" && theme != "dark") theme = "dark";
  if (lang != "en") lang = "zh";
  std::string line = theme + "|" + bg + "|" + lang;
  FILE* f = _wfopen(UiFile().c_str(), L"wb");
  if (!f) return;
  fwrite(line.data(), 1, line.size(), f);
  fclose(f);
}

// Persist point for the user's engine prefs (threads / hash) - the single source of truth.
// Root cause of "thread count is not saved": the value only lived in page localStorage,
// which Chromium commits lazily (~5s) while the app terminates the WebView2 child on exit,
// so the write was lost (observed: set 5 -> restart read back 3). engineConfig reaches the
// host synchronously, so writing here bypasses that async commit entirely.
// Persist point for the link "识别参数" overlay (single source of truth). Same rationale as
// EnginePrefFile: the host gets the message synchronously, so writing here beats the page's
// lazily-committed localStorage. Format: `interval|gray|blackOff|whiteOff|blackPct|whitePct`.
static std::wstring LinkRecogPrefFile() {
  wchar_t tmp[MAX_PATH] = {0};
  GetTempPathW(MAX_PATH, tmp);
  return Join(std::wstring(tmp), L"GomokuTrainer.linkrecog");
}

static void ClampLinkRecog(int& interval, int& gray, int& blackOff, int& whiteOff,
                           int& blackPct, int& whitePct) {
  auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
  interval = clamp(interval, 40, 2000);   // 同步时间间隔(ms)：下界 40，避免空转
  gray     = clamp(gray, 0, 255);
  blackOff = clamp(blackOff, 0, 255);
  whiteOff = clamp(whiteOff, 0, 255);
  blackPct = clamp(blackPct, 0, 100);
  whitePct = clamp(whitePct, 0, 100);
}

static std::string LinkRecogJson() {
  char b[256];
  snprintf(b, sizeof(b),
           "{\"interval\":%d,\"gray_offset\":%d,\"black_offset\":%d,"
           "\"white_offset\":%d,\"black_percent\":%d,\"white_percent\":%d}",
           g_syncInterval.load(), g_syncGray.load(), g_syncBlackOff.load(),
           g_syncWhiteOff.load(), g_syncBlackPct.load(), g_syncWhitePct.load());
  return std::string(b);
}

static void LoadLinkRecogPrefs() {
  FILE* f = _wfopen(LinkRecogPrefFile().c_str(), L"rb");
  if (!f) return;
  char buf[96] = {0};
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  int iv = 200, gr = 50, bo = 96, wo = 96, bp = 33, wp = 33;
  if (n == 0 || sscanf(buf, "%d|%d|%d|%d|%d|%d", &iv, &gr, &bo, &wo, &bp, &wp) < 1) return;
  ClampLinkRecog(iv, gr, bo, wo, bp, wp);
  g_syncInterval.store(iv);
  g_syncGray.store(gr);
  g_syncBlackOff.store(bo);
  g_syncWhiteOff.store(wo);
  g_syncBlackPct.store(bp);
  g_syncWhitePct.store(wp);
  LogMsg("[link] loaded recog prefs: " + LinkRecogJson());
}

static void SaveLinkRecogPrefs() {
  std::string line = std::to_string(g_syncInterval.load()) + "|" +
                     std::to_string(g_syncGray.load()) + "|" +
                     std::to_string(g_syncBlackOff.load()) + "|" +
                     std::to_string(g_syncWhiteOff.load()) + "|" +
                     std::to_string(g_syncBlackPct.load()) + "|" +
                     std::to_string(g_syncWhitePct.load());
  FILE* f = _wfopen(LinkRecogPrefFile().c_str(), L"wb");
  if (!f) return;
  fwrite(line.data(), 1, line.size(), f);
  fflush(f);                        // flush now: survives a hard kill
  fclose(f);
}

// ★★ 2026-10-19（用户：「点击读档，会展开一个抽屉，里面就是之前用户保存的参数名称，最多只能保存
//   50 个」）：连线「识别参数」的**存档列表**（最多 50 条，形如
//   [{"name":"…","recog":{"interval":..,…},"ts":1690000000000}, …]）。
//   本页 localStorage 是延迟提交的（宿主退出时直接杀掉 WebView2 子进程，写入常丢 —— 见
//   EnginePrefFile 的注释），所以这里同样以宿主落盘为**单一权威**：
//     · 页面只在「存档 / 删除 / 撤销」时把整包 JSON 发过来（linkRecogSlots），宿主原样写盘；
//     · 连线窗 ready 时宿主把这包原样回填给页面。
//   宿主**不解析**这包内容：页面用 JSON.stringify 生成、用 JSON.parse 读回，C++ 侧只校验
//   「是个数组、体积合理」，省掉一个 JSON 解析器（名称里的引号/反斜杠由 JSON 的转义规则保证往返）。
static std::wstring LinkRecogSlotsFile() {
  wchar_t tmp[MAX_PATH] = {0};
  GetTempPathW(MAX_PATH, tmp);
  return Join(std::wstring(tmp), L"GomokuTrainer.linkrecog.slots");
}

static std::string ReadLinkRecogSlots() {
  FILE* f = _wfopen(LinkRecogSlotsFile().c_str(), L"rb");
  if (!f) return std::string("[]");
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  size_t p = out.find_first_not_of(" \r\n\t");
  if (p == std::string::npos || out[p] != '[') return std::string("[]");   // 写坏/被手改成别的东西
  return out;
}

static void WriteLinkRecogSlots(const std::string& blob) {
  FILE* f = _wfopen(LinkRecogSlotsFile().c_str(), L"wb");
  if (!f) return;
  fwrite(blob.data(), 1, blob.size(), f);
  fflush(f);                        // flush now: survives a hard kill
  fclose(f);
}

static void ApplyDarkTitleBar(HWND h, bool dark) {
  if (!h || !IsWindow(h)) return;
  BOOL v = dark ? TRUE : FALSE;
  if (FAILED(DwmSetWindowAttribute(h, 20, &v, sizeof(v)))) {
    DwmSetWindowAttribute(h, 19, &v, sizeof(v));
  }
}

static void SetWebviewBg(ICoreWebView2Controller* ctl, COLORREF c) {
  if (!ctl) return;
  ICoreWebView2Controller2* c2 = nullptr;
  if (FAILED(ctl->QueryInterface(__uuidof(ICoreWebView2Controller2), (void**)&c2)) || !c2) return;
  COREWEBVIEW2_COLOR col = { 255, GetRValue(c), GetGValue(c), GetBValue(c) };   
  c2->put_DefaultBackgroundColor(col);
  c2->Release();
}

static void RevealMainPage() {
  g_mainPageShown = true;
  if (g_controller) g_controller->put_IsVisible(TRUE);
  if (g_hwnd && IsWindow(g_hwnd)) InvalidateRect(g_hwnd, nullptr, TRUE);
}

static const wchar_t* kBrandName = L"Meter Gomoku";   

static const wchar_t* kBrandNameRv = L"Gomoku Review";

static COLORREF BrandFg() {
  return g_uiDark ? RGB(0x8c, 0xa6, 0xc0) : RGB(0x5b, 0x7a, 0x99);
}

static const wchar_t* BrandFontFace() {
  static const wchar_t* kFaces[] = {L"Source Han Sans SC", L"Source Han Sans CN", L"Noto Sans SC",
                                    L"思源黑体", L"Source Han Sans", L"Microsoft YaHei", L"Segoe UI"};
  static const wchar_t* picked = nullptr;
  if (picked) return picked;
  struct Ctx { const wchar_t* want; bool found; };
  for (const wchar_t* face : kFaces) {
    Ctx ctx{face, false};
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face);
    HDC screen = GetDC(nullptr);
    EnumFontFamiliesExW(screen, &lf,
        [](const LOGFONTW* f, const TEXTMETRICW*, DWORD, LPARAM lp) -> int {
          Ctx* c = (Ctx*)lp;
          if (f && _wcsicmp(f->lfFaceName, c->want) == 0) { c->found = true; return 0; }
          return 1;
        },
        (LPARAM)&ctx, 0);
    ReleaseDC(nullptr, screen);
    if (ctx.found) { picked = face; return picked; }
  }
  picked = L"Segoe UI";
  return picked;
}

static void PaintSplash(HWND h, bool pageShown, const wchar_t* brand) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  RECT rc;
  GetClientRect(h, &rc);
  HBRUSH br = CreateSolidBrush(g_uiBg);
  FillRect(dc, &rc, br);
  DeleteObject(br);
  if (!pageShown) {
    
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    HFONT f = CreateFontW(-MulDiv(22, dpi, 72), 0, 0, 0, FW_SEMIBOLD, TRUE , FALSE, FALSE,
                          ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, BrandFontFace());
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    
    const wchar_t* kMeter = L"Meter";
    size_t mlen = wcslen(kMeter);
    bool hasMeter = (wcsncmp(brand, kMeter, mlen) == 0 && brand[mlen] == L' ');
    const wchar_t* rest = hasMeter ? (brand + mlen) : brand;   
    SIZE sm = { 0, 0 }, sr = { 0, 0 };
    GetTextExtentPoint32W(dc, kMeter, (int)mlen, &sm);
    GetTextExtentPoint32W(dc, rest, (int)wcslen(rest), &sr);
    RECT rc2;
    GetClientRect(h, &rc2);
    
    int totalW = hasMeter ? (sm.cx + sr.cx) : sr.cx;
    int tx = rc2.left + ((rc2.right - rc2.left) - totalW) / 2;
    int ty = rc2.top + ((rc2.bottom - rc2.top) - sr.cy) / 2;
    if (hasMeter) {
      SetTextColor(dc, RGB(0x3d, 0x9b, 0xd6));                 
      TextOutW(dc, tx, ty, kMeter, (int)mlen);
    }
    SetTextColor(dc, BrandFg());
    if (hasMeter) tx += sm.cx;   
    TextOutW(dc, tx, ty, rest, (int)wcslen(rest));
    SelectObject(dc, old);
    DeleteObject(f);
  }
  EndPaint(h, &ps);
}

static const wchar_t* kRvCls = L"GbCalcReview";
static const wchar_t* kRvTitle = L"复盘 · Gomoku Review";

static std::wstring RvWinPosFile() {
  wchar_t tmp[MAX_PATH] = {0};
  GetTempPathW(MAX_PATH, tmp);
  return Join(std::wstring(tmp), L"GomokuReview.pos");
}
static void PostToRvPage(const std::string& json) {
  if (!g_rvWebview) { g_rvPending = json; return; }
  std::wstring w = Utf8ToWide(json);
  g_rvWebview->PostWebMessageAsJson(w.c_str());
}

static void PostToOwner(HWND owner, const std::string& json) {
  if (owner && owner == g_rvHwnd) PostToRvPage(json); else PostToPage(json);
}
static void LayoutRvWebView() {
  if (!g_rvController || !g_rvHwnd) return;
  RECT r;
  GetClientRect(g_rvHwnd, &r);
  g_rvController->put_Bounds(r);
}
static void SaveRvWinPos() {
  if (!g_rvHwnd || IsIconic(g_rvHwnd)) return;
  WINDOWPLACEMENT wp = {};
  wp.length = sizeof(wp);
  if (!GetWindowPlacement(g_rvHwnd, &wp)) return;
  FILE* f = _wfopen(RvWinPosFile().c_str(), L"wb");
  if (!f) return;
  fwrite(&wp, 1, sizeof(wp), f);
  fclose(f);
}

static bool g_rvWanted = false;      
static bool g_rvShown = false;       
static bool g_rvPageReady = false;   
static const UINT_PTR kRvTimerPrewarm = 2;   

static const UINT kMsgPrewarmRv = WM_APP + 1;

static void SetRvBackground(const std::string& hex) {
  if (!g_rvController || hex.size() < 7 || hex[0] != '#') return;
  ICoreWebView2Controller2* c2 = nullptr;
  if (FAILED(g_rvController->QueryInterface(__uuidof(ICoreWebView2Controller2), (void**)&c2)) || !c2) return;
  unsigned r = 0, g = 0, b = 0;
  if (sscanf(hex.c_str(), "#%2x%2x%2x", &r, &g, &b) == 3) {
    COREWEBVIEW2_COLOR col = { 255, (BYTE)r, (BYTE)g, (BYTE)b };   
    c2->put_DefaultBackgroundColor(col);
    LogMsg("[rv] default background = " + hex);
  }
  c2->Release();
}

static void ShowReviewWindow() {
  if (!g_rvHwnd || g_rvShown) return;
  g_rvShown = true;
  ShowWindow(g_rvHwnd, SW_SHOWNORMAL);
  SetForegroundWindow(g_rvHwnd);
  LogMsg("[rv] review window shown");
}

static void RevealRvPage() {
  if (g_rvController) g_rvController->put_IsVisible(TRUE);
  if (g_rvHwnd && IsWindow(g_rvHwnd)) InvalidateRect(g_rvHwnd, nullptr, TRUE);
}

static void ScheduleRvPrewarm(UINT ms) {
  if (g_hwnd && IsWindow(g_hwnd)) SetTimer(g_hwnd, kRvTimerPrewarm, ms, nullptr);
}

static LRESULT CALLBACK RvWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_ERASEBKGND:
      return 1;                                  
    case WM_PAINT:
      PaintSplash(h, g_rvPageReady, kBrandNameRv);  
      return 0;
    case WM_SIZE:
      LayoutRvWebView();
      return 0;
    
    case WM_GETMINMAXINFO: {
      MINMAXINFO* mm = (MINMAXINFO*)l;
      
      int mw = GbPx(h, 520), mh = GbPx(h, 620);
      GbClampToWorkArea(h, mw, mh);
      mm->ptMinTrackSize.x = mw;
      mm->ptMinTrackSize.y = mh;
      return 0;
    }
    case WM_CLOSE:
      SaveRvWinPos();
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      SaveRvWinPos();
      if (g_rvController) { g_rvController->Release(); g_rvController = nullptr; }
      if (g_rvWebview) { g_rvWebview->Release(); g_rvWebview = nullptr; }
      g_rvHwnd = nullptr;
      g_rvCreating = false;
      g_rvPending.clear();
      g_rvWanted = false;
      g_rvShown = false;
      g_rvPageReady = false;
      LogMsg("[rv] review window destroyed");
      
      PostMessageW(g_hwnd, kMsgPrewarmRv, 0, 0);
      return 0;                          
    default:
      break;
  }
  return DefWindowProcW(h, m, w, l);
}

class RvMsgHandler : public Cb<ICoreWebView2WebMessageReceivedEventHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*,
                                   ICoreWebView2WebMessageReceivedEventArgs* e) override {
    LPWSTR raw = nullptr;
    if (FAILED(e->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
    std::string s = WideToUtf8(raw);
    CoTaskMemFree(raw);
    if (s.find("\"ready\"") != std::string::npos) {
      g_rvPageReady = true;
      ApplyDarkTitleBar(g_rvHwnd, g_uiDark);           
      RevealRvPage();                                 
      SetRvBackground(JsonStrAfter(s, "\"bg\":"));     
      if (!g_rvWanted) {
        
        LogMsg("[rv] prewarmed and hidden (page ready, waiting for the user)");
        return S_OK;
      }
      LogMsg("[rv] review page ready");
      if (!g_rvPending.empty()) {
        std::string p = g_rvPending;
        g_rvPending.clear();
        PostToRvPage(p);
      }
      ShowReviewWindow();                              
      return S_OK;
    }
    
    if (s.find("\"reviewAck\"") != std::string::npos) {
      int n = 0;
      size_t p = s.find("\"moves\":");
      if (p != std::string::npos) n = atoi(s.c_str() + p + 8);
      LogMsg("[rv] ack moves=" + std::to_string(n));
      return S_OK;
    }
    if (s.find("\"closeReview\"") != std::string::npos) {
      LogMsg("[rv] page asked to close the review window");
      if (g_rvHwnd) PostMessageW(g_rvHwnd, WM_CLOSE, 0, 0);
      return S_OK;
    }
    if (s.find("\"savePng\"") != std::string::npos) {
      SavePngFromPage(s, g_rvHwnd);       
      return S_OK;
    }
    
    if (s.find("\"saveTxt\"") != std::string::npos) {
      TxtSaveResult r = SaveTxtFromPage(s, g_rvHwnd);
      if (r.ok) PostToOwner(g_rvHwnd, "{\"type\":\"histExported\",\"n\":" + std::to_string(r.n) + "}");
      return S_OK;
    }
    if (s.find("\"openTxt\"") != std::string::npos) {
      std::string m = ReadTxtMessage(g_rvHwnd);
      if (!m.empty()) PostToOwner(g_rvHwnd, m);
      return S_OK;
    }
    if (s.find("\"histImported\"") != std::string::npos) {
      LogMsg("[hist] imported (review) n=" + std::to_string(JsonIntAfter(s, "\"n\":")) +
             " fp=" + JsonStrAfter(s, "\"fp\":"));
      return S_OK;
    }
    return S_OK;
  }
};

class RvCtlHandler : public Cb<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* ctl) override {
    g_rvCreating = false;
    if (FAILED(hr) || !ctl || !g_rvHwnd) {
      LogMsg("[rv] review controller creation failed");
      return S_OK;
    }
    g_rvController = ctl;
    ctl->AddRef();
    ctl->get_CoreWebView2(&g_rvWebview);
    if (!g_rvWebview) return S_OK;
    g_rvWebview->AddRef();
    HardenWebviewSettings(g_rvWebview, "rv");
    
    SetupUiServing(g_rvWebview, "rv");
    EventRegistrationToken tok = {};
    g_rvWebview->add_WebMessageReceived(new RvMsgHandler(), &tok);
    g_rvWebview->Navigate(UiBaseUrl("rv").c_str());
    
    SetWebviewBg(ctl, g_uiBg);
    ctl->put_IsVisible(FALSE);
    LayoutRvWebView();
    LogMsg("[rv] navigated to calc.html?rv=1");
    return S_OK;
  }
};

static void CreateReviewWindow() {
  if (g_rvHwnd || g_rvCreating || !g_env) return;

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = RvWndProc;
  wc.hInstance = g_hInst;
  wc.lpszClassName = kRvCls;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
  wc.hIconSm = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
  RegisterClassExW(&wc);

  int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
  int w = GbPx(nullptr, 900), h = GbPx(nullptr, 980);
  bool placed = false;
  {
    FILE* f = _wfopen(RvWinPosFile().c_str(), L"rb");
    if (f) {
      WINDOWPLACEMENT wp = {};
      if (fread(&wp, 1, sizeof(wp), f) == sizeof(wp) && wp.length == sizeof(wp)) {
        RECT r = wp.rcNormalPosition;
        x = r.left; y = r.top;
        w = r.right - r.left; h = r.bottom - r.top;
        placed = true;
      }
      fclose(f);
    }
  }
  RECT work = {0, 0, 0, 0};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  if (!placed) {
    
    RECT mw = {0, 0, 0, 0};
    if (g_hwnd) GetWindowRect(g_hwnd, &mw);
    x = mw.right + 12;
    if (x + w > work.right) x = mw.left - w - 12;
    y = mw.top;
  }
  int maxW = work.right - work.left, maxH = work.bottom - work.top;
  if (w > maxW) w = maxW;
  if (h > maxH) h = maxH;
  if (x < work.left) x = work.left;
  if (y < work.top) y = work.top;
  if (x + w > work.right) x = work.right - w;
  if (y + h > work.bottom) y = work.bottom - h;

  g_rvHwnd = CreateWindowExW(0, kRvCls, kRvTitle,
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             x, y, w, h, nullptr, nullptr, g_hInst, nullptr);
  if (!g_rvHwnd) { LogMsg("[rv] CreateWindowEx failed"); return; }
  g_rvCreating = true;
  g_rvShown = false;
  g_rvPageReady = false;
  LogMsg("[rv] review window created (independent top-level window)");
  LogMsg("[rv] window kept hidden until the page reports ready (no black flash)");
  g_env->CreateCoreWebView2Controller(g_rvHwnd, new RvCtlHandler());
}

static void PrewarmReviewWindow() {
  if (g_rvHwnd || g_rvCreating || !g_env) return;
  LogMsg("[rv] prewarming the review window (hidden, so the first open is instant)");
  CreateReviewWindow();
}

static void OpenReviewWindow(const std::string& recordJson) {
  std::string rec = recordJson.empty() ? std::string("{\"moves\":[]}") : recordJson;
  LogMsg("[rv] openReview record " + std::to_string(rec.size()) + " bytes");
  std::string msg = "{\"type\":\"reviewData\",\"record\":" + rec + "}";
  g_rvWanted = true;
  if (g_rvHwnd) {                                
    if (IsIconic(g_rvHwnd)) ShowWindow(g_rvHwnd, SW_RESTORE);
    if (g_rvPageReady) PostToRvPage(msg);
    else g_rvPending = msg;                      
    
    ShowReviewWindow();
    
    SetForegroundWindow(g_rvHwnd);
    return;
  }
  g_rvPending = msg;
  if (g_rvCreating || !g_env) {                  
    LogMsg(g_rvCreating ? "[rv] queued (window is being created)"
                        : "[rv] queued (webview2 env not ready)");
    
    if (g_rvHwnd) ShowReviewWindow();
    return;
  }
  CreateReviewWindow();
  if (g_rvHwnd) {
    
    ShowReviewWindow();
  }
}

static const wchar_t* kVisCls = L"GbCalcVis";
static const wchar_t* kVisTitle = L"识图 · Gomoku Vision";

static const wchar_t* kBrandNameVis = L"Gomoku Vision";

static std::wstring VisWinPosFile() {
  wchar_t tmp[MAX_PATH] = { 0 };
  GetTempPathW(MAX_PATH, tmp);
  return Join(std::wstring(tmp), L"GomokuVis.pos");
}
static void PostToVisPage(const std::string& json) {
  if (!g_visWebview) { g_visPending = json; return; }
  std::wstring w = Utf8ToWide(json);
  g_visWebview->PostWebMessageAsJson(w.c_str());
}

static void PostToVisPageSoon(const std::string& json) {
  OutboxPush(g_outboxVis, json);
  if (g_hwnd && IsWindow(g_hwnd)) PostMessageW(g_hwnd, kMsgFlushOutbox, 0, 0);
}
static void LayoutVisWebView() {
  if (!g_visController || !g_visHwnd) return;
  RECT r;
  GetClientRect(g_visHwnd, &r);
  g_visController->put_Bounds(r);
}
static void SaveVisWinPos() {
  if (!g_visHwnd || IsIconic(g_visHwnd)) return;
  WINDOWPLACEMENT wp = {};
  wp.length = sizeof(wp);
  if (!GetWindowPlacement(g_visHwnd, &wp)) return;
  FILE* f = _wfopen(VisWinPosFile().c_str(), L"wb");
  if (!f) return;
  fwrite(&wp, 1, sizeof(wp), f);
  fclose(f);
}
static void ShowVisWindow() {
  if (!g_visHwnd || g_visShown) return;
  g_visShown = true;
  ShowWindow(g_visHwnd, SW_SHOWNORMAL);
  SetForegroundWindow(g_visHwnd);
  LogMsg("[vis] vision window shown");
}
static void RevealVisPage() {
  if (g_visController) g_visController->put_IsVisible(TRUE);
  if (g_visHwnd && IsWindow(g_visHwnd)) InvalidateRect(g_visHwnd, nullptr, TRUE);
}
static void SetVisBackground(const std::string& hex) {
  if (!g_visController || hex.size() < 7 || hex[0] != '#') return;
  ICoreWebView2Controller2* c2 = nullptr;
  if (FAILED(g_visController->QueryInterface(__uuidof(ICoreWebView2Controller2), (void**)&c2)) || !c2) return;
  unsigned r = 0, g = 0, b = 0;
  if (sscanf(hex.c_str(), "#%2x%2x%2x", &r, &g, &b) == 3) {
    COREWEBVIEW2_COLOR col = { 255, (BYTE)r, (BYTE)g, (BYTE)b };   
    c2->put_DefaultBackgroundColor(col);
    LogMsg("[vis] default background = " + hex);
  }
  c2->Release();
}

static void VselCloseWithVisWindow() {
  if (g_visSelHwnd && IsWindow(g_visSelHwnd)) {
    LogMsg("[vis] vision window gone -> closing the hollow select box");
    PostMessageW(g_visSelHwnd, WM_CLOSE, 0, 0);
  }
}

static LRESULT CALLBACK VisWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      
      PaintSplash(h, g_visPageReady, kBrandNameVis);
      return 0;
    case WM_SIZE:
      LayoutVisWebView();
      return 0;
    case WM_GETMINMAXINFO: {
      MINMAXINFO* mm = (MINMAXINFO*)l;
      
      int mw = GbPx(h, kVisMinCssW), mh = GbPx(h, kVisMinCssH);
      GbClampToWorkArea(h, mw, mh);
      mm->ptMinTrackSize.x = mw;
      mm->ptMinTrackSize.y = mh;
      return 0;
    }
    case WM_CLOSE:
      SaveVisWinPos();
      VselCloseWithVisWindow();
      
      PostToPageSoon("{\"type\":\"visClosed\"}");
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      SaveVisWinPos();
      VselCloseWithVisWindow();
      if (g_visController) { g_visController->Release(); g_visController = nullptr; }
      if (g_visWebview) { g_visWebview->Release(); g_visWebview = nullptr; }
      g_visHwnd = nullptr;
      g_visCreating = false;
      g_visPending.clear();
      g_visShown = false;
      g_visPageReady = false;
      LogMsg("[vis] vision window destroyed");
      return 0;                          
    default:
      break;
  }
  return DefWindowProcW(h, m, w, l);
}

class VisMsgHandler : public Cb<ICoreWebView2WebMessageReceivedEventHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*,
                                   ICoreWebView2WebMessageReceivedEventArgs* e) override {
    LPWSTR raw = nullptr;
    if (FAILED(e->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
    std::string s = WideToUtf8(raw);
    CoTaskMemFree(raw);
    if (s.find("\"ready\"") != std::string::npos) {
      g_visPageReady = true;
      ApplyDarkTitleBar(g_visHwnd, g_uiDark);
      RevealVisPage();
      SetVisBackground(JsonStrAfter(s, "\"bg\":"));
      LogMsg("[vis] vision page ready");
      if (!g_visPending.empty()) {
        std::string p = g_visPending;
        g_visPending.clear();
        PostToVisPage(p);
      }
      ShowVisWindow();
      // ★★ 2026-10-19：把宿主落盘的「识图识别参数」回填给识图页（页面自身 localStorage 不可靠，
      //   宿主才是单一权威 —— 与连线窗 linkRecog 同一口径）。
      PostToVisPage("{\"type\":\"visRecog\",\"recog\":" + VisRecogJson() + "}");
      return S_OK;
    }
    
    if (s.find("\"visShot\"") != std::string::npos) {
      PostMessageW(g_hwnd, kMsgVisShot, 0, 0);
      return S_OK;
    }
    
    // ★★ 2026-10-19（用户：「识图这个界面上也添加一个识别参数……」）：识图参数的值一动就发一条 ——
    //   宿主把值写进「有效参数」**并落盘**（单一权威：关软件再打开不回退默认，与连线窗
    //   linkRecogPreview 同一口径）。下一拍「自动吸附盘」就用新阈值下发给视觉服务。
    //   注意这条要排在 "visRecognize" 之前判：两种消息串不会互相包含，顺序仅为保险。
    if (s.find("\"visRecogPreview\"") != std::string::npos) {
      int gr = JsonIntAfter(s, "\"gray_offset\":");    if (gr < 0) gr = g_visRcGray.load();
      int bo = JsonIntAfter(s, "\"black_offset\":");   if (bo < 0) bo = g_visRcBlackOff.load();
      int wo = JsonIntAfter(s, "\"white_offset\":");   if (wo < 0) wo = g_visRcWhiteOff.load();
      int bp = JsonIntAfter(s, "\"black_percent\":");  if (bp < 0) bp = g_visRcBlackPct.load();
      int wp = JsonIntAfter(s, "\"white_percent\":");  if (wp < 0) wp = g_visRcWhitePct.load();
      ClampVisRecog(gr, bo, wo, bp, wp);
      g_visRcGray.store(gr);
      g_visRcBlackOff.store(bo);
      g_visRcWhiteOff.store(wo);
      g_visRcBlackPct.store(bp);
      g_visRcWhitePct.store(wp);
      SaveVisRecogPrefs();
      return S_OK;
    }

    if (s.find("\"openImage\"") != std::string::npos) {
      OpenImageFromPage(s);
      return S_OK;
    }
    
    if (s.find("\"visRecognize\"") != std::string::npos) {
      std::thread(RunVisionForVis, s).detach();
      return S_OK;
    }
    
    if (s.find("\"visToTrainer\"") != std::string::npos) {
      std::string payload = JsonSubObject(s, "\"payload\"");
      if (!payload.empty()) {
        PostToPageSoon("{\"type\":\"external\",\"payload\":" + JsonQuote(payload) + "}");
        LogMsg("[vis] forwarded board to the trainer (" + std::to_string(payload.size()) + " bytes)");
      }
      return S_OK;
    }
    
    if (s.find("uiTheme") != std::string::npos) {
      std::string lg = JsonStrAfterDecoded(s, JKey("lang").c_str());
      if (!lg.empty()) g_uiLangEn = (lg.rfind("en", 0) == 0);
      std::string th = JsonStrAfterDecoded(s, JKey("theme").c_str());
      if (th != "light" && th != "dark") th = "dark";
      SaveUiPrefs(s);
      g_uiDark = (th != "light");
      ApplyDarkTitleBar(g_visHwnd, g_uiDark);
      LogMsg("[vis] ui lang -> " + std::string(g_uiLangEn ? "en" : "zh"));
      return S_OK;
    }
    if (s.find("\"closeVis\"") != std::string::npos) {
      LogMsg("[vis] page asked to close the vision window");
      if (g_visHwnd) PostMessageW(g_visHwnd, WM_CLOSE, 0, 0);
      return S_OK;
    }
    return S_OK;
  }
};

class VisCtlHandler : public Cb<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* ctl) override {
    g_visCreating = false;
    if (FAILED(hr) || !ctl || !g_visHwnd) {
      LogMsg("[vis] vision controller creation failed");
      return S_OK;
    }
    g_visController = ctl;
    ctl->AddRef();
    ctl->get_CoreWebView2(&g_visWebview);
    if (!g_visWebview) return S_OK;
    g_visWebview->AddRef();
    HardenWebviewSettings(g_visWebview, "vis");
    SetupUiServing(g_visWebview, "vis");
    EventRegistrationToken tok = {};
    g_visWebview->add_WebMessageReceived(new VisMsgHandler(), &tok);
    g_visWebview->Navigate(UiBaseUrl("vis").c_str());
    
    SetWebviewBg(ctl, g_uiBg);
    ctl->put_IsVisible(FALSE);
    LayoutVisWebView();
    LogMsg("[vis] navigated to calc.html?vis=1");
    return S_OK;
  }
};

static void CreateVisWindow() {
  if (g_visHwnd || g_visCreating || !g_env) return;

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = VisWndProc;
  wc.hInstance = g_hInst;
  wc.lpszClassName = kVisCls;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
  wc.hIconSm = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
  RegisterClassExW(&wc);

  int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1180, h = 820;
  bool placed = false;
  {
    FILE* f = _wfopen(VisWinPosFile().c_str(), L"rb");
    if (f) {
      WINDOWPLACEMENT wp = {};
      if (fread(&wp, 1, sizeof(wp), f) == sizeof(wp) && wp.length == sizeof(wp)) {
        RECT r = wp.rcNormalPosition;
        x = r.left; y = r.top;
        w = r.right - r.left; h = r.bottom - r.top;
        placed = true;
      }
      fclose(f);
    }
  }
  RECT work = { 0, 0, 0, 0 };
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  if (!placed) {
    
    RECT mw = { 0, 0, 0, 0 };
    if (g_hwnd) GetWindowRect(g_hwnd, &mw);
    x = mw.right + 12;
    if (x + w > work.right) x = mw.left - w - 12;
    y = mw.top;
  }
  int maxW = work.right - work.left, maxH = work.bottom - work.top;
  if (w > maxW) w = maxW;
  if (h > maxH) h = maxH;
  if (x < work.left) x = work.left;
  if (y < work.top) y = work.top;
  if (x + w > work.right) x = work.right - w;
  if (y + h > work.bottom) y = work.bottom - h;

  g_visHwnd = CreateWindowExW(0, kVisCls, kVisTitle,
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              x, y, w, h, nullptr, nullptr, g_hInst, nullptr);
  if (!g_visHwnd) { LogMsg("[vis] CreateWindowEx failed"); return; }
  g_visCreating = true;
  g_visShown = false;
  g_visPageReady = false;
  LogMsg("[vis] vision window created (independent top-level window)");
  LogMsg("[vis] window kept hidden until the page reports ready (no black flash)");
  g_env->CreateCoreWebView2Controller(g_visHwnd, new VisCtlHandler());
}

static void OpenVisWindow() {
  LogMsg("[vis] openVis requested");
  PrewarmVisionServiceAsync();   
  if (g_visHwnd) {
    if (IsIconic(g_visHwnd)) ShowWindow(g_visHwnd, SW_RESTORE);
    ShowVisWindow();
    SetForegroundWindow(g_visHwnd);
    return;
  }
  if (g_visCreating || !g_env) {
    LogMsg(g_visCreating ? "[vis] queued (window is being created)"
                         : "[vis] queued (webview2 env not ready)");
    return;
  }
  CreateVisWindow();
  if (g_visHwnd) ShowVisWindow();
}

static const wchar_t* kLinkCls = L"GbCalcLink";
static const wchar_t* kLinkTitle = L"Link";
static const wchar_t* kBrandNameLink = L"Gomoku Link";

static ICoreWebView2Controller* g_linkController = nullptr;
static ICoreWebView2* g_linkWebview = nullptr;
static std::string g_linkPending;
static bool g_linkCreating = false, g_linkShown = false, g_linkPageReady = false;

static std::atomic<bool> g_linkAuto(false);         
static std::atomic<int>  g_linkClicks(1);           
static RECT g_mainRectBeforeLink = {0, 0, 0, 0};    

static std::wstring LinkWinPosFile() {
  wchar_t tmp[MAX_PATH] = { 0 };
  GetTempPathW(MAX_PATH, tmp);
  return Join(std::wstring(tmp), L"GomokuLink.pos");
}
static void PostToLinkPage(const std::string& json) {
  if (!g_linkWebview) { g_linkPending = json; return; }
  std::wstring w = Utf8ToWide(json);
  g_linkWebview->PostWebMessageAsJson(w.c_str());
}
static void PostToLinkPageSoon(const std::string& json) {
  OutboxPush(g_outboxLink, json);     
  if (g_hwnd && IsWindow(g_hwnd)) PostMessageW(g_hwnd, kMsgFlushOutbox, 0, 0);
}
static void LayoutLinkWebView() {
  if (!g_linkController || !g_linkHwnd) return;
  RECT r;
  GetClientRect(g_linkHwnd, &r);
  g_linkController->put_Bounds(r);
}
static void SaveLinkWinPos() {
  if (!g_linkHwnd || IsIconic(g_linkHwnd)) return;
  WINDOWPLACEMENT wp = {};
  wp.length = sizeof(wp);
  if (!GetWindowPlacement(g_linkHwnd, &wp)) return;
  FILE* f = _wfopen(LinkWinPosFile().c_str(), L"wb");
  if (!f) return;
  fwrite(&wp, 1, sizeof(wp), f);
  fclose(f);
}
static void ShowLinkWindow() {
  if (!g_linkHwnd || g_linkShown) return;
  g_linkShown = true;
  ShowWindow(g_linkHwnd, SW_SHOWNORMAL);
  SetForegroundWindow(g_linkHwnd);
  MarkerRaiseTopmost(g_linkHwnd);      
  LogMsg("[link] link window shown");
}
static void RevealLinkPage() {
  if (g_linkController) g_linkController->put_IsVisible(TRUE);
  if (g_linkHwnd && IsWindow(g_linkHwnd)) InvalidateRect(g_linkHwnd, nullptr, TRUE);
}
static void SetLinkBackground(const std::string& hex) {
  if (!g_linkController || hex.size() < 7 || hex[0] != '#') return;
  ICoreWebView2Controller2* c2 = nullptr;
  if (FAILED(g_linkController->QueryInterface(__uuidof(ICoreWebView2Controller2), (void**)&c2)) || !c2) return;
  unsigned r = 0, g = 0, b = 0;
  if (sscanf(hex.c_str(), "#%2x%2x%2x", &r, &g, &b) == 3) {
    COREWEBVIEW2_COLOR col = { 255, (BYTE)r, (BYTE)g, (BYTE)b };
    c2->put_DefaultBackgroundColor(col);
  }
  c2->Release();
}

static void ApplyMainLinkAspect(bool on) {
  if (!g_hwnd || !IsWindow(g_hwnd)) return;
  if (on) {
    if (g_mainRectBeforeLink.right > g_mainRectBeforeLink.left) return;   
    GetWindowRect(g_hwnd, &g_mainRectBeforeLink);
    LONG hgt = g_mainRectBeforeLink.bottom - g_mainRectBeforeLink.top;
    LONG wid = (LONG)((double)hgt * 1.16 + 0.5);
    RECT work = { 0, 0, 0, 0 };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (wid > work.right - work.left) wid = work.right - work.left;
    SetWindowPos(g_hwnd, nullptr, g_mainRectBeforeLink.left, g_mainRectBeforeLink.top,
                 wid, hgt, SWP_NOZORDER);
    LogMsg("[link] main window -> 1.16:1 (w=" + std::to_string(wid) + " h=" + std::to_string(hgt) + ")");
  } else if (g_mainRectBeforeLink.right > g_mainRectBeforeLink.left) {
    SetWindowPos(g_hwnd, nullptr,
                 g_mainRectBeforeLink.left, g_mainRectBeforeLink.top,
                 g_mainRectBeforeLink.right - g_mainRectBeforeLink.left,
                 g_mainRectBeforeLink.bottom - g_mainRectBeforeLink.top, SWP_NOZORDER);
    LogMsg("[link] main window restored");
    g_mainRectBeforeLink = { 0, 0, 0, 0 };
  }
}

static HWND g_fsWnd = nullptr;                 
static RECT g_fsRect = {0, 0, 0, 0};           
static bool g_fsDragging = false;
static POINT g_fsAnchor = {0, 0};
static HWND g_lselWnd = nullptr;               
static RECT g_scanRegion = {0, 0, 0, 0};       
static bool g_lselPinned = false;              
static bool g_lselOk = false;                  

static std::atomic<int> g_linkMissStreak(0);
static bool g_lselGrown = false;               

static std::atomic<bool> g_linkScanRun(false);

static HANDLE g_linkWatchEvt = nullptr;      
static std::atomic<bool> g_linkWatchRun(false);

static std::atomic<bool> g_linkRescanSoon{false};

static DWORD WINAPI LinkFrameWatchProc(LPVOID) {
  const int WH = 96;                        
  unsigned char prev[WH * WH];
  bool havePrev = false;
  RECT cached = {0, 0, 0, 0};               
  HDC screen = nullptr, mem = nullptr;
  HBITMAP bmp = nullptr;
  HGDIOBJ oldBmp = nullptr;
  void* bits = nullptr;

  auto releaseGdi = [&]() {
    if (mem && oldBmp) { SelectObject(mem, oldBmp); oldBmp = nullptr; }
    if (bmp) { DeleteObject(bmp); bmp = nullptr; }
    if (mem) { DeleteDC(mem); mem = nullptr; }
    if (screen) { ReleaseDC(NULL, screen); screen = nullptr; }
    bits = nullptr;
    havePrev = false;
  };
  auto ensureGdi = [&]() -> bool {
    if (mem) return true;
    screen = GetDC(NULL);
    if (!screen) return false;
    mem = CreateCompatibleDC(screen);
    if (!mem) { ReleaseDC(NULL, screen); screen = nullptr; return false; }
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = WH;
    bi.bmiHeader.biHeight = -WH;            
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { releaseGdi(); return false; }
    oldBmp = SelectObject(mem, bmp);
    SetStretchBltMode(mem, COLORONCOLOR);   
    return true;
  };

  LogMsg("[link] frame watcher started (30fps pixel-diff on scan rect; full scan only on change)");

  while (g_running.load()) {
    Sleep(33);                              
    if (!g_linkSession.load() || !g_lselGrown) { havePrev = false; continue; }
    
    RECT b = g_scanRegion;
    if (b.right <= b.left || b.bottom <= b.top) { havePrev = false; continue; }
    int mx = (b.right - b.left) / 20 + 8, my = (b.bottom - b.top) / 20 + 8;
    RECT watch = {b.left - mx, b.top - my, b.right + mx, b.bottom + my};
    if (!havePrev && !EqualRect(&watch, &cached)) { cached = watch; havePrev = false; }

    if (!ensureGdi()) { releaseGdi(); continue; }
    if (!StretchBlt(mem, 0, 0, WH, WH, screen,
                    watch.left, watch.top,
                    watch.right - watch.left, watch.bottom - watch.top,
                    SRCCOPY | CAPTUREBLT)) {
      releaseGdi();                         
      continue;
    }
    
    const unsigned char* p = (const unsigned char*)bits;
    int changed = 0;
    for (int i = 0; i < WH * WH; ++i) {
      const unsigned char* q = p + i * 4;   
      int g = (q[0] * 77 + q[1] * 151 + q[2] * 28) >> 8;
      if (havePrev) {
        int d = g - prev[i];
        if (d < 0) d = -d;
        if (d > 24) ++changed;
      }
      prev[i] = (unsigned char)g;
    }
    havePrev = true;
    
    if (changed >= 12 && g_linkWatchEvt) SetEvent(g_linkWatchEvt);
  }
  releaseGdi();
  return 0;
}

static void StartLinkFrameWatch() {
  if (g_linkWatchRun.exchange(true)) return;
  if (!g_linkWatchEvt) g_linkWatchEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  std::thread(LinkFrameWatchProc, nullptr).detach();
  LogMsg("[link] frame watch thread started");
}
static bool g_linkDropOnce = false;          

static std::vector<std::pair<int,int>> g_lkBalB, g_lkBalW;

static std::vector<int> g_lkBalBSeq, g_lkBalWSeq;
static int  g_linkBoard[15][15] = {{0}};       
static std::atomic<bool> g_lselMarkOnly(false);  
static std::atomic<int>  g_linkFastLeft(0);      
static std::vector<double> g_geoX, g_geoY;     
static double g_geoSpacing = 0;

#define GB_LINK_NO_BOARD_KEY "\x01"
static std::string g_lastBoardKey = GB_LINK_NO_BOARD_KEY;   
static std::mutex g_linkGeoMtx;                

static std::atomic<int> g_linkBestX(-1), g_linkBestY(-1);

static RECT g_tightPendRect = {0, 0, 0, 0};
static int  g_tightPendStreak = 0;

static void LinkParseBest(const std::string& s) {
  int bxx = -1, byy = -1;
  size_t bqp = s.find("\"best\":\"");
  if (bqp != std::string::npos) {
    size_t qq = bqp + 8;
    if (qq < s.size() && s[qq] >= 'a' && s[qq] <= 'o') {
      bxx = s[qq] - 'a';
      int num = 0;
      for (size_t k = qq + 1; k < s.size() && s[k] >= '0' && s[k] <= '9' && num < 100; ++k)
        num = num * 10 + (s[k] - '0');
      if (num >= 1 && num <= 15) byy = 15 - num; else bxx = -1;
    }
  }
  g_linkBestX.store(bxx); g_linkBestY.store(byy);
  if (bxx >= 0) {
    LogMsg("[link] engine best received: " + std::string(1, (char)('a' + bxx)) +
           std::to_string(15 - byy) + " -> recommended dot on immune layer");
    if (g_lselWnd && IsWindow(g_lselWnd))
      PostMessageW(g_lselWnd, kMsgLinkFramePaint, 0, 0);
  }
}

static void ParseBalPairs(const std::string& s, const char* key, std::vector<std::pair<int,int>>& out) {
  out.clear();
  size_t p = s.find(key);
  if (p == std::string::npos) return;
  p = s.find('[', p);
  if (p == std::string::npos) return;
  while (p < s.size()) {
    while (p < s.size() && s[p] != '[') ++p;
    if (p >= s.size()) break;
    size_t e = s.find(']', p);
    if (e == std::string::npos) break;
    int v[2] = {0, 0}; int got = 0; size_t q = p + 1;
    while (q < e && got < 2) {
      while (q < e && (s[q] < '0' || s[q] > '9') && s[q] != '-') ++q;
      if (q >= e) break;
      int sign = 1; if (s[q] == '-') { sign = -1; ++q; }
      int val = 0;
      while (q < e && s[q] >= '0' && s[q] <= '9') { val = val * 10 + (s[q] - '0'); ++q; }
      v[got++] = val * sign;
    }
    if (got == 2 && v[0] >= 0 && v[0] <= 14 && v[1] >= 0 && v[1] <= 14)
      out.push_back({ v[0], v[1] });
    p = e + 1;
  }
}

static bool LinkScreenToGrid(double sx, double sy, int& gx, int& gy) {
  std::vector<double> lx, ly;
  {
    std::lock_guard<std::mutex> lk(g_linkGeoMtx);
    lx = g_geoX; ly = g_geoY;
  }
  if ((int)lx.size() < 15 || (int)ly.size() < 15) return false;
  const double sp = lx[1] - lx[0];
  if (sp < 8) return false;
  int bx = -1, by = -1;
  double bdx = sp * 0.5, bdy = sp * 0.5;
  for (int i = 0; i < 15; ++i) {
    double dx = sx - lx[i]; if (dx < 0) dx = -dx;
    if (dx < bdx) { bdx = dx; bx = i; }
    double dy = sy - ly[i]; if (dy < 0) dy = -dy;
    if (dy < bdy) { bdy = dy; by = i; }
  }
  if (bx < 0 || by < 0) return false;
  gx = bx; gy = by;
  return true;
}

#define LSEL_TITLE_H 44
#define LSEL_BTN_W   78
#define LSEL_BTN2_W  78
#define LSEL_BTN_GAP 8
#define LSEL_EDGE    8

#define LSEL_PUR_R 95     
#define LSEL_PUR_G 138
#define LSEL_PUR_B 244
#define LSEL_TEAL_R 70
#define LSEL_TEAL_G 190
#define LSEL_TEAL_B 175   

static const wchar_t* LselT(const wchar_t* zh, const wchar_t* en) { return g_uiLangEn ? en : zh; }

static void LselOpenAt(int x, int y, int w, int h);   
static void StartLinkFrame();                            
static void StartLinkScanLoop();                      

static HBITMAP g_lselBmp = nullptr;
static int g_lselBmpW = 0, g_lselBmpH = 0;
static BYTE* g_lselBits = nullptr;

static void LselClose() {
  
  if (g_scanRegion.right > g_scanRegion.left || g_scanRegion.bottom > g_scanRegion.top) {
    g_scanRegion = {0, 0, 0, 0};
    LogMsg("[link] frame closed -> region cleared");
  }
  g_linkScanRun.store(false);
  
  if (g_lselWnd && IsWindow(g_lselWnd)) PostMessageW(g_lselWnd, WM_CLOSE, 0, 0);
  g_lselWnd = nullptr;
  g_lselOk = false; g_lselPinned = false; g_lselGrown = false;
  g_geoX.clear(); g_geoY.clear(); g_geoSpacing = 0;
  g_lastBoardKey = GB_LINK_NO_BOARD_KEY;
  g_linkBestX.store(-1); g_linkBestY.store(-1);            
  g_tightPendStreak = 0; g_tightPendRect = {0, 0, 0, 0};
}

static void RestoreLinkScan() {
  if (!g_linkSession.load()) return;
  
  if (g_scanRegion.right <= g_scanRegion.left || g_scanRegion.bottom <= g_scanRegion.top) {
    LogMsg("[link] scan idle (no framed region - waiting for user frame)");
    return;
  }
  g_lselGrown = false;
  g_lastBoardKey = GB_LINK_NO_BOARD_KEY;
  StartLinkScanLoop();
  LogMsg("[link] scan restored (framed region, stone-only)");
}

static void FsPaintLayer(HWND h) {
  int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int W = GetSystemMetrics(SM_CXVIRTUALSCREEN), H = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
  bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
  BYTE* bits = nullptr;
  HDC sdc = GetDC(nullptr);
  HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
  if (bmp && bits) {
    Gdiplus::Bitmap gb(W, H, W * 4, PixelFormat32bppPARGB, bits);
    Gdiplus::Graphics g(&gb);
    g.Clear(Gdiplus::Color(28, LSEL_PUR_R, LSEL_PUR_G, LSEL_PUR_B));   
    if (g_fsRect.right > g_fsRect.left && g_fsRect.bottom > g_fsRect.top) {
      
      g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
      Gdiplus::SolidBrush hollow(Gdiplus::Color(0, 0, 0, 0));
      g.FillRectangle(&hollow, (Gdiplus::REAL)(g_fsRect.left - vx), (Gdiplus::REAL)(g_fsRect.top - vy),
                      (Gdiplus::REAL)(g_fsRect.right - g_fsRect.left),
                      (Gdiplus::REAL)(g_fsRect.bottom - g_fsRect.top));
      g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
      float X0 = (float)(g_fsRect.left - vx), Y0 = (float)(g_fsRect.top - vy);
      float X1 = (float)(g_fsRect.right - vx), Y1 = (float)(g_fsRect.bottom - vy);
      float a = 22.0f;
      if (a > (X1 - X0) / 2.4f) a = (X1 - X0) / 2.4f;
      if (a > (Y1 - Y0) / 2.4f) a = (Y1 - Y0) / 2.4f;
      {   
        Gdiplus::Pen lpen(Gdiplus::Color(225, 47, 216, 255), 2.5f);
        g.DrawLine(&lpen, X0 + a, Y0, X1 - a, Y0);
        g.DrawLine(&lpen, X0 + a, Y1, X1 - a, Y1);
        g.DrawLine(&lpen, X0, Y0 + a, X0, Y1 - a);
        g.DrawLine(&lpen, X1, Y0 + a, X1, Y1 - a);
      }
      {   
        float p[8][4] = {
            {X0, Y0 + a, X0, Y0}, {X0, Y0, X0 + a, Y0},
            {X1 - a, Y0, X1, Y0}, {X1, Y0, X1, Y0 + a},
            {X1, Y1 - a, X1, Y1}, {X1, Y1, X1 - a, Y1},
            {X0 + a, Y1, X0, Y1}, {X0, Y1, X0, Y1 - a},
        };
        for (int pass = 0; pass < 2; ++pass) {
          Gdiplus::Color lc = (pass == 0) ? Gdiplus::Color(92, LSEL_PUR_R, LSEL_PUR_G, LSEL_PUR_B)
                                          : Gdiplus::Color(255, LSEL_PUR_R, LSEL_PUR_G, LSEL_PUR_B);
          float lt = (pass == 0) ? 2.5f * 2.6f : 2.5f;
          Gdiplus::Pen pen(lc, lt);
          pen.SetLineJoin(Gdiplus::LineJoinMiter);
          for (int i = 0; i < 8; ++i) g.DrawLine(&pen, p[i][0], p[i][1], p[i][2], p[i][3]);
        }
      }
    }
    HDC mem = CreateCompatibleDC(sdc);
    HGDIOBJ old = SelectObject(mem, bmp);
    POINT src = {0, 0};
    SIZE sz = {W, H};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(h, sdc, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteDC(mem);
  }
  ReleaseDC(nullptr, sdc);
  if (bmp) DeleteObject(bmp);
}

static LRESULT CALLBACK FsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    
    case WM_WINDOWPOSCHANGING:
      return MarkerOnWndPosChanging(l);
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(h, &ps);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_MOUSEMOVE:
      if (g_fsDragging) {
        POINT pt; GetCursorPos(&pt);
        g_fsRect.left   = (g_fsAnchor.x < pt.x) ? g_fsAnchor.x : pt.x;
        g_fsRect.right  = (g_fsAnchor.x < pt.x) ? pt.x : g_fsAnchor.x;
        g_fsRect.top    = (g_fsAnchor.y < pt.y) ? g_fsAnchor.y : pt.y;
        g_fsRect.bottom = (g_fsAnchor.y < pt.y) ? pt.y : g_fsAnchor.y;
        FsPaintLayer(h);                      
      }
      return 0;
    case WM_LBUTTONDOWN:
      g_fsDragging = true;
      GetCursorPos(&g_fsAnchor);
      g_fsRect = {g_fsAnchor.x, g_fsAnchor.y, g_fsAnchor.x, g_fsAnchor.y};
      SetCapture(h);
      return 0;
    case WM_LBUTTONUP: {
      g_fsDragging = false;
      ReleaseCapture();
      RECT r = g_fsRect;
      HWND me = h;
      DestroyWindow(me);                       
      g_fsWnd = nullptr;
      if (r.right - r.left >= 120 && r.bottom - r.top >= 120) {
        
        LselOpenAt(r.left, r.top - LSEL_TITLE_H, r.right - r.left,
                   (r.bottom - r.top) + LSEL_TITLE_H);
      } else {
        RestoreLinkScan();            
      }
      return 0;
    }
    case WM_KEYDOWN:
      if (w == VK_ESCAPE) { DestroyWindow(h); g_fsWnd = nullptr; RestoreLinkScan(); }
      return 0;
    case WM_DESTROY:
      if (g_fsWnd == h) g_fsWnd = nullptr;
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

static void StartFrameSelectFlow() {
  
  LselClose();
  if (g_fsWnd && IsWindow(g_fsWnd)) return;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = FsProc;
  wc.hInstance = g_hInst;
  wc.lpszClassName = L"GbLinkFsSelect";
  wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
  RegisterClassExW(&wc);
  int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  g_fsWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
                            L"GbLinkFsSelect", nullptr, WS_POPUP,
                            vx, vy, vw, vh, nullptr, nullptr, g_hInst, nullptr);
  if (!g_fsWnd) { LogMsg("[link] fullscreen select window failed"); return; }
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x11
#endif
  MarkerExcludeFromCapture(g_fsWnd, "link", false);   
  g_fsRect = {0, 0, 0, 0};
  FsPaintLayer(g_fsWnd);                        
  ShowWindow(g_fsWnd, SW_SHOWNORMAL);
  LogMsg("[link] fullscreen rubber-band select started (drag a rect over the board)");
}

enum { LSEL_NONE = 0, LSEL_MOVE = 1, LSEL_L, LSEL_R, LSEL_T, LSEL_B, LSEL_TL, LSEL_TR, LSEL_BL, LSEL_BR,
       LSEL_BTN_REC = 100, LSEL_BTN_AUTO = 101, LSEL_BTN_CLOSE = 102 };
static int  g_lselDragMode = LSEL_NONE;
static POINT g_lselDragPt = {0, 0};
static RECT  g_lselDragRect = {0, 0, 0, 0};
static int   g_lselHoverBtn = 0, g_lselPressBtn = 0;
static RECT  g_lselBtnRec = {0}, g_lselBtnAuto = {0}, g_lselBtnClose = {0};
static bool  g_lselAuto = false;         

static void LselRoundRect(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
  if (r > w / 2.0f) r = w / 2.0f;
  if (r > h / 2.0f) r = h / 2.0f;
  p.AddArc(x, y, r * 2, r * 2, 180, 90);
  p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
  p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
  p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
  p.CloseFigure();
}

static int LselHit(HWND h, int mx, int my) {
  if (g_lselMarkOnly.load()) return LSEL_NONE;   
  RECT rc;
  GetClientRect(h, &rc);
  int W = rc.right - rc.left, H = rc.bottom - rc.top;
  if (my < LSEL_TITLE_H) {
    if (mx >= g_lselBtnRec.left && mx < g_lselBtnRec.right &&
        my >= g_lselBtnRec.top && my < g_lselBtnRec.bottom) return LSEL_BTN_REC;
    if (mx >= g_lselBtnAuto.left && mx < g_lselBtnAuto.right &&
        my >= g_lselBtnAuto.top && my < g_lselBtnAuto.bottom) return LSEL_BTN_AUTO;
    if (mx >= g_lselBtnClose.left && mx < g_lselBtnClose.right &&
        my >= g_lselBtnClose.top && my < g_lselBtnClose.bottom) return LSEL_BTN_CLOSE;
    return LSEL_MOVE;
  }
  const int G = 10;                     
  bool l = mx < G, r = mx >= W - G, t = my < LSEL_TITLE_H + G, b = my >= H - G;
  if (l && t) return LSEL_TL;  if (r && t) return LSEL_TR;
  if (l && b) return LSEL_BL;  if (r && b) return LSEL_BR;
  if (l) return LSEL_L;  if (r) return LSEL_R;
  if (t) return LSEL_T;  if (b) return LSEL_B;
  return LSEL_NONE;
}
static void LselCursor(int hit) {
  static LPWSTR cur = IDC_ARROW;
  LPWSTR want = IDC_ARROW;
  if (hit == LSEL_L || hit == LSEL_R) want = IDC_SIZEWE;
  else if (hit == LSEL_T || hit == LSEL_B) want = IDC_SIZENS;
  else if (hit == LSEL_TL || hit == LSEL_BR) want = IDC_SIZENWSE;
  else if (hit == LSEL_TR || hit == LSEL_BL) want = IDC_SIZENESW;
  else if (hit == LSEL_MOVE) want = IDC_SIZEALL;
  if (cur != want) { SetCursor(LoadCursor(nullptr, want)); cur = want; }
}

static void LselPaint(HWND h) {
  RECT rc;
  GetClientRect(h, &rc);
  int W = rc.right - rc.left, H = rc.bottom - rc.top;
  if (W <= 0 || H <= 0) return;
  if (!g_lselBmp || g_lselBmpW != W || g_lselBmpH != H) {
    if (g_lselBmp) DeleteObject(g_lselBmp);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    g_lselBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void**)&g_lselBits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    g_lselBmpW = W; g_lselBmpH = H;
  }
  if (!g_lselBmp || !g_lselBits) return;
  Gdiplus::Bitmap gb(W, H, W * 4, PixelFormat32bppPARGB, g_lselBits);
  Gdiplus::Graphics g(&gb);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
  g.Clear(Gdiplus::Color(0, 0, 0, 0));                     
  
  const Gdiplus::Color main_((BYTE)LSEL_PUR_R, (BYTE)LSEL_PUR_G, (BYTE)LSEL_PUR_B);
  if (!g_lselMarkOnly.load()) {   
  {   
    Gdiplus::SolidBrush tb(Gdiplus::Color(236, main_.GetRed(), main_.GetGreen(), main_.GetBlue()));
    g.FillRectangle(&tb, 0.0f, 0.0f, (Gdiplus::REAL)W, (Gdiplus::REAL)LSEL_TITLE_H);
    Gdiplus::Pen ul(Gdiplus::Color(120, 255, 255, 255), 1.0f);
    g.DrawLine(&ul, 0.0f, (Gdiplus::REAL)LSEL_TITLE_H - 0.5f, (Gdiplus::REAL)W, (Gdiplus::REAL)LSEL_TITLE_H - 0.5f);
  }
  {   
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font f(&ff, 15.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush b(Gdiplus::Color(255, 255, 255, 255));
    std::wstring t = LselT(L"框选棋盘", L"Frame Board");
    if (g_lselOk) t += LselT(L" · 已锁定（识别成功）", L" · Locked");
    else if (g_scanRegion.right > g_scanRegion.left) t += LselT(L" · 待识别", L" · Pending");
    Gdiplus::StringFormat sf(Gdiplus::StringFormatFlagsNoWrap);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    g.DrawString(t.c_str(), -1, &f, Gdiplus::PointF(12.0f, (Gdiplus::REAL)(LSEL_TITLE_H / 2)), &sf, &b);
  }
  }   
  if (!g_lselMarkOnly.load()) {   
  {   
    
    int bh = LSEL_TITLE_H - 8, by = 4;
    
    int bx3 = W - LSEL_EDGE - LSEL_BTN2_W;
    int bx2 = bx3 - LSEL_BTN_GAP - LSEL_BTN2_W;
    int bx1 = bx2 - LSEL_BTN_GAP - LSEL_BTN2_W;
    g_lselBtnRec   = {bx1, by, bx1 + LSEL_BTN2_W, by + bh};
    g_lselBtnAuto  = {bx2, by, bx2 + LSEL_BTN2_W, by + bh};
    g_lselBtnClose = {bx3, by, bx3 + LSEL_BTN2_W, by + bh};
    std::wstring autoTxt = g_lselAuto ? LselT(L"自动 开", L"Auto") : LselT(L"自动 关", L"Off");
    struct BtnDef { RECT r; int id; int rr, gg, bb; std::wstring t; };
    BtnDef defs[3] = {
      { g_lselBtnRec,   LSEL_BTN_REC,   30, 150, 255, LselT(L"识别", L"Scan") },
      { g_lselBtnAuto,  LSEL_BTN_AUTO,  g_lselAuto ? 72 : 96, g_lselAuto ? 201 : 118,
        g_lselAuto ? 186 : 136, autoTxt },
      { g_lselBtnClose, LSEL_BTN_CLOSE, 214,  96,  96, LselT(L"关闭", L"Close") },
    };
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font f(&ff, 14.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);   
    Gdiplus::StringFormat sf(Gdiplus::StringFormatFlagsNoWrap);
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    for (int i = 0; i < 3; ++i) {
      const BtnDef& d = defs[i];
      bool hov = (g_lselHoverBtn == d.id), prs = (g_lselPressBtn == d.id);
      int r = d.rr, gg = d.gg, bb = d.bb;
      if (d.id == LSEL_BTN_REC) {
        
      } else if (d.id == LSEL_BTN_CLOSE) {
        r = 214; gg = 96; bb = 96;
      }
      if (hov && !prs) { r += 24; gg += 24; bb += 12; }
      if (prs)         { r -= 26; gg -= 26; bb -= 20; }
      if (r < 0) r = 0; if (gg < 0) gg = 0; if (bb < 0) bb = 0;
      if (r > 255) r = 255; if (gg > 255) gg = 255; if (bb > 255) bb = 255;
      
      Gdiplus::GraphicsPath pth;
      LselRoundRect(pth, (float)d.r.left, (float)d.r.top,
                    (float)(d.r.right - d.r.left), (float)(d.r.bottom - d.r.top), 4.0f);
      Gdiplus::SolidBrush fb(Gdiplus::Color(255, (BYTE)r, (BYTE)gg, (BYTE)bb));
      Gdiplus::SolidBrush tb(Gdiplus::Color(255, 255, 255, 255));
      g.FillPath(&fb, &pth);
      g.DrawString(d.t.c_str(), -1, &f,
                   Gdiplus::RectF((Gdiplus::REAL)d.r.left, (Gdiplus::REAL)d.r.top,
                                  (Gdiplus::REAL)(d.r.right - d.r.left),
                                  (Gdiplus::REAL)(d.r.bottom - d.r.top)), &sf, &tb);
    }
  }
  }   
  {   
    
    float top = g_lselMarkOnly.load() ? 2.0f : (float)LSEL_TITLE_H + 2.0f, bot = (float)H - 2.0f;
    float x0 = 3.0f, x1 = (float)W - 3.0f;
    float a = 26.0f;                              
    {
      Gdiplus::Pen lpen(Gdiplus::Color(225, 47, 216, 255), 3.0f);   
      g.DrawLine(&lpen, x0 + a, top, x1 - a, top);
      g.DrawLine(&lpen, x0 + a, bot, x1 - a, bot);
      g.DrawLine(&lpen, x0, top + a, x0, bot - a);
      g.DrawLine(&lpen, x1, top + a, x1, bot - a);
    }
    {
      
      float cr = (float)LSEL_PUR_R;
      float cg = (float)LSEL_PUR_G;
      float cb = (float)LSEL_PUR_B;
      float p[8][4] = {
          {x0, top + a, x0, top}, {x0, top, x0 + a, top},
          {x1 - a, top, x1, top}, {x1, top, x1, top + a},
          {x1, bot - a, x1, bot}, {x1, bot, x1 - a, bot},
          {x0 + a, bot, x0, bot}, {x0, bot, x0, bot - a},
      };
      for (int pass = 0; pass < 2; ++pass) {
        Gdiplus::Color c = (pass == 0) ? Gdiplus::Color(92, (BYTE)cr, (BYTE)cg, (BYTE)cb)
                                       : Gdiplus::Color(255, (BYTE)cr, (BYTE)cg, (BYTE)cb);
        float t = (pass == 0) ? 3.0f * 2.6f : 3.0f;      
        Gdiplus::Pen pen(c, t);
        pen.SetLineJoin(Gdiplus::LineJoinMiter);
        for (int i = 0; i < 8; ++i) g.DrawLine(&pen, p[i][0], p[i][1], p[i][2], p[i][3]);
      }
    }
  }
  {   
    
    if (g_lselOk) {
      RECT wr2;
      GetWindowRect(h, &wr2);
      std::vector<double> lx2, ly2;
      {
        std::lock_guard<std::mutex> lk(g_linkGeoMtx);
        lx2 = g_geoX; ly2 = g_geoY;
      }
      if ((int)lx2.size() == 15 && (int)ly2.size() == 15 && lx2.size() >= 2) {
        const double sp2 = lx2[1] - lx2[0];
        if (sp2 > 8) {
          auto inFrame = [](float x, float y) {
            
            return x >= 2 && y >= (g_lselMarkOnly.load() ? 2.0f : (float)LSEL_TITLE_H + 2);
          };
          
          const int idxs[5][2] = {{3,3},{11,3},{3,11},{11,11},{7,7}};
          const float rr = (float)(sp2 * 0.22);
          for (int i = 0; i < 5; ++i) {
            float x = (float)(lx2[idxs[i][0]] - wr2.left), y = (float)(ly2[idxs[i][1]] - wr2.top);
            if (!inFrame(x, y)) continue;
            Gdiplus::Pen cross(Gdiplus::Color(235, 70, 190, 175), 2.6f);
            g.DrawLine(&cross, x - rr, y, x + rr, y);
            g.DrawLine(&cross, x, y - rr, x, y + rr);
          }
          
          int bx = g_linkBestX.load(), by = g_linkBestY.load();
          if (bx >= 0 && bx < 15 && by >= 0 && by < 15) {
            float x = (float)(lx2[bx] - wr2.left), y = (float)(ly2[by] - wr2.top);
            if (inFrame(x, y)) {
              float br = (float)(sp2 * 0.46);
              Gdiplus::Pen bp(Gdiplus::Color(240, 47, 216, 255), 3.4f);   
              g.DrawEllipse(&bp, x - br, y - br, br * 2, br * 2);
              Gdiplus::Pen bp2(Gdiplus::Color(120, 255, 255, 255), 1.2f); 
              g.DrawEllipse(&bp2, x - br * 0.8f, y - br * 0.8f, br * 1.6f, br * 1.6f);
            }
          }
          
          auto drawBalMk = [&](const std::vector<std::pair<int,int>>& v,
                               const std::vector<int>& seqv,
                               int ringR, int ringG, int ringB, int dotR, int dotG, int dotB) {
            for (size_t mi = 0; mi < v.size(); ++mi) {
              const auto& mk = v[mi];
              if (mk.first < 0 || mk.first > 14 || mk.second < 0 || mk.second > 14) continue;
              float x = (float)(lx2[mk.first] - wr2.left), y = (float)(ly2[mk.second] - wr2.top);
              if (!inFrame(x, y)) continue;
              float br = (float)(sp2 * 0.4);
              Gdiplus::Pen bp(Gdiplus::Color(235, (BYTE)ringR, (BYTE)ringG, (BYTE)ringB), 3.0f);
              g.DrawEllipse(&bp, x - br, y - br, br * 2, br * 2);
              int seq = (mi < seqv.size()) ? seqv[mi] : 0;
              if (seq > 0 && br >= 9.0f) {
                float fsz = sp2 * 0.42f; if (fsz < 10.0f) fsz = 10.0f; if (fsz > 20.0f) fsz = 20.0f;
                Gdiplus::FontFamily ff3(L"Segoe UI");
                Gdiplus::Font f3(&ff3, fsz, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                if (f3.GetLastStatus() == Gdiplus::Ok) {
                  wchar_t num[8]; swprintf(num, 8, L"%d", seq);
                  Gdiplus::StringFormat sf3(Gdiplus::StringFormatFlagsNoWrap);
                  sf3.SetAlignment(Gdiplus::StringAlignmentCenter);
                  sf3.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                  Gdiplus::SolidBrush halo3(Gdiplus::Color(215, 0, 0, 0));
                  Gdiplus::SolidBrush fg3(Gdiplus::Color(255, 255, 255, 255));
                  Gdiplus::RectF rc3(x - br, y - br, br * 2, br * 2);
                  g.DrawString(num, -1, &f3, Gdiplus::RectF(rc3.X - 1, rc3.Y, rc3.Width, rc3.Height), &sf3, &halo3);
                  g.DrawString(num, -1, &f3, rc3, &sf3, &fg3);
                }
              }
            }
          };
          drawBalMk(g_lkBalB, g_lkBalBSeq, 148, 108, 238, 70, 30, 90);   
          drawBalMk(g_lkBalW, g_lkBalWSeq, 70, 190, 175, 20, 60, 130);   
        }
      }
    }
  }
  if (!g_lselMarkOnly.load()) {   
  {   
    Gdiplus::SolidBrush hb(main_);
    const int hs = 8;
    float pts[4][2] = {
      {1.0f, (float)LSEL_TITLE_H + 1.0f},
      {(float)(W - 1 - hs), (float)LSEL_TITLE_H + 1.0f},
      {1.0f, (float)(H - 1 - hs)},
      {(float)(W - 1 - hs), (float)(H - 1 - hs)},
    };
    for (int i = 0; i < 4; ++i) g.FillRectangle(&hb, pts[i][0], pts[i][1], (float)hs, (float)hs);
  }
  }   
  HDC sdc = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(sdc);
  HGDIOBJ old = SelectObject(mem, g_lselBmp);
  POINT src = {0, 0};
  SIZE sz = {W, H};
  BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(h, sdc, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);
  SelectObject(mem, old);
  DeleteDC(mem);
  ReleaseDC(nullptr, sdc);
}

static void LselFollowRect(const RECT& tight) {
  if (!(g_lselWnd && IsWindow(g_lselWnd))) return;
  const bool mark = g_lselMarkOnly.load();
  const int wx = tight.left;
  const int wy = mark ? tight.top : (tight.top - LSEL_TITLE_H);
  const int ww = tight.right - tight.left;
  const int wh = (tight.bottom - tight.top) + (mark ? 0 : LSEL_TITLE_H);
  RECT cur;
  if (!GetWindowRect(g_lselWnd, &cur)) return;
  if (labs(cur.left - wx) <= 1 && labs(cur.top - wy) <= 1 &&
      labs((cur.right - cur.left) - ww) <= 1 && labs((cur.bottom - cur.top) - wh) <= 1)
    return;                                       
  SetWindowPos(g_lselWnd, nullptr, wx, wy, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
  PostMessageW(g_lselWnd, kMsgLinkFramePaint, 0, 0);   
}

static void LselSetAutoScanRegion() {
  
  g_scanRegion.left = 0; g_scanRegion.top = 0;
  g_scanRegion.right = 0; g_scanRegion.bottom = 0;
  g_lselGrown = false;
  g_lselOk = false;
  g_lastBoardKey = GB_LINK_NO_BOARD_KEY;
  g_tightPendStreak = 0; g_tightPendRect = {0, 0, 0, 0};   
  {
    std::lock_guard<std::mutex> lk(g_linkGeoMtx);
    g_geoX.clear(); g_geoY.clear(); g_geoSpacing = 0;
  }
}

static void LselApplyRegion(HWND h) {
  RECT wr;
  GetWindowRect(h, &wr);
  int top = wr.top + LSEL_TITLE_H;
  if (wr.right - wr.left < 120 || wr.bottom - top < 120) return;
  // ★ 2026-10-13 回退（用户：「寻棋盘算法，自动功能跟随昨天 23 点左右的那一版」）：
  //   小框（< 屏幕 1/3）几乎肯定没框住整个棋盘 —— 切到「自动寻盘」，让扫描区扩到整屏
  //   再自动收紧到 ±0.6 格（自动保持开启，以后再开框也走同一条路）。这段是 10-12 22:09
  //   加的，属于 23:00 那一版的行为；本轮的改动把它删掉了，是退步，现在照原样补回来。
  {
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if ((wr.right - wr.left) < sw / 3 || (wr.bottom - top) < sh / 3) {
      g_lselAuto = true;
      LselSetAutoScanRegion();
      StartLinkScanLoop();
      PostToLinkPageSoon("{\"type\":\"linkState\",\"frame\":\"pending\"}");
      LogMsg("[link] small frame -> auto board-search (grow to full board)");
      LselPaint(h);
      return;
    }
  }
  g_scanRegion.left = wr.left + 2; g_scanRegion.top = top + 2;      
  g_scanRegion.right = wr.right - 2; g_scanRegion.bottom = wr.bottom - 2;
  g_lselOk = false; g_lselGrown = false;
  g_linkScanRun.store(false);
  
  {
    std::lock_guard<std::mutex> lk(g_linkGeoMtx);
    g_geoX.clear(); g_geoY.clear(); g_geoSpacing = 0;
  }
  g_lastBoardKey = GB_LINK_NO_BOARD_KEY;
  g_tightPendStreak = 0; g_tightPendRect = {0, 0, 0, 0};   
  StartLinkScanLoop();                       
  LogMsg("[link] region set: screen (" + std::to_string(g_scanRegion.left) + "," +
         std::to_string(g_scanRegion.top) + ") " +
         std::to_string(g_scanRegion.right - g_scanRegion.left) + "x" +
         std::to_string(g_scanRegion.bottom - g_scanRegion.top));
  PostToLinkPageSoon("{\"type\":\"linkState\",\"frame\":\"pending\"}");
  LselPaint(h);
}

static LRESULT CALLBACK LselProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    
    case WM_WINDOWPOSCHANGING:
      return MarkerOnWndPosChanging(lp);
    
    case WM_TIMER:
      MarkerRaiseTopmost(h);
      return 0;
    case WM_NCHITTEST: {
      
      POINT pt; pt.x = GET_X_LPARAM(lp); pt.y = GET_Y_LPARAM(lp);
      POINT cl = pt; ScreenToClient(h, &cl);
      if (LselHit(h, cl.x, cl.y) == LSEL_NONE) return HTTRANSPARENT;
      return HTCLIENT;
    }
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        LselCursor(LselHit(h, pt.x, pt.y));
        return TRUE;
      }
      break;
    case WM_MOUSEMOVE: {
      int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
      if (g_lselDragMode != LSEL_NONE && g_lselDragMode < LSEL_BTN_REC) {
        POINT pt; GetCursorPos(&pt);
        int dx = pt.x - g_lselDragPt.x, dy = pt.y - g_lselDragPt.y;
        RECT r = g_lselDragRect;
        switch (g_lselDragMode) {
          case LSEL_MOVE: r.left += dx; r.right += dx; r.top += dy; r.bottom += dy; break;
          case LSEL_L: r.left  += dx; break;
          case LSEL_R: r.right += dx; break;
          case LSEL_T: r.top   += dy; break;
          case LSEL_B: r.bottom += dy; break;
          case LSEL_TL: r.left += dx; r.top += dy; break;
          case LSEL_TR: r.right += dx; r.top += dy; break;
          case LSEL_BL: r.left += dx; r.bottom += dy; break;
          case LSEL_BR: r.right += dx; r.bottom += dy; break;
          default: break;
        }
        int minW = 140, minH = LSEL_TITLE_H + 100;
        if (r.right - r.left < minW) {
          if (g_lselDragMode == LSEL_L || g_lselDragMode == LSEL_TL || g_lselDragMode == LSEL_BL)
            r.left = r.right - minW;
          else r.right = r.left + minW;
        }
        if (r.bottom - r.top < minH) {
          if (g_lselDragMode == LSEL_T || g_lselDragMode == LSEL_TL || g_lselDragMode == LSEL_TR)
            r.top = r.bottom - minH;
          else r.bottom = r.top + minH;
        }
        SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        
        g_lselOk = false;
        LselPaint(h);
        return 0;
      }
      int hit = LselHit(h, mx, my);
      int hov = (hit >= LSEL_BTN_REC) ? hit : 0;
      if (hov != g_lselHoverBtn) { g_lselHoverBtn = hov; LselPaint(h); }
      LselCursor(hit);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      int hit = LselHit(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      if (hit >= LSEL_BTN_REC) {
        g_lselPressBtn = hit; LselPaint(h);
        SetCapture(h);
        return 0;
      }
      if (g_lselPinned) return 0;              
      if (hit == LSEL_NONE) return 0;
      g_lselDragMode = hit;
      GetCursorPos(&g_lselDragPt);
      GetWindowRect(h, &g_lselDragRect);
      SetCapture(h);
      return 0;
    }
    case WM_LBUTTONUP: {
      if (!GetCapture()) { ReleaseCapture(); return 0; }
      ReleaseCapture();
      if (g_lselPressBtn != LSEL_NONE) {
        int hit = LselHit(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        int pressed = g_lselPressBtn;
        g_lselPressBtn = LSEL_NONE;
        if (hit == pressed) {
          if (pressed == LSEL_BTN_REC) LselApplyRegion(h);
          else if (pressed == LSEL_BTN_AUTO) {
            g_lselAuto = !g_lselAuto;      
            LselPaint(h);
            if (g_lselAuto) {
              LselSetAutoScanRegion();
              StartLinkScanLoop();
              LogMsg("[link] auto board-search on (fullscreen)");
            } else {
              
              if (g_scanRegion.right <= g_scanRegion.left ||
                  g_scanRegion.bottom <= g_scanRegion.top) {
                g_linkScanRun.store(false);
                LogMsg("[link] auto board-search off (no region - scan idle)");
              } else {
                StartLinkScanLoop();     
                                         
                LogMsg("[link] auto board-search off (region locked - keep manual scan)");
              }
            }
          }
          else {
            
            LselClose();
            LogMsg("[link] frame closed (fully stopped recognition)");
          }
          return 0;
        }
        LselPaint(h);
        return 0;
      }
      g_lselDragMode = LSEL_NONE;
      
      return 0;
    }
    case WM_KEYDOWN:
      if (wp == VK_ESCAPE) {
        
        LselClose();
        LogMsg("[link] ESC closed frame (fully stopped recognition)");
      }
      return 0;
    case WM_SIZE:
      LselPaint(h);
      return 0;
    case kMsgLinkFramePaint:                
      LselPaint(h);
      return 0;
    case WM_DESTROY:
      KillTimer(h, 9);                          
      if (g_lselWnd == h) g_lselWnd = nullptr;
      if (g_lselBmp) { DeleteObject(g_lselBmp); g_lselBmp = nullptr; }
      g_lselBits = nullptr; g_lselBmpW = 0; g_lselBmpH = 0;
      PostQuitMessage(0);               
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

static int g_lselX = 0, g_lselY = 0, g_lselW = 470, g_lselH = 544;

static DWORD WINAPI LselThreadMain(LPVOID) {
  if (g_lselWnd && IsWindow(g_lselWnd)) { LogMsg("[link] frame thread: already open"); return 0; }
  LogMsg("[link] frame thread: registering class...");
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = LselProc;
  wc.hInstance = g_hInst;
  wc.lpszClassName = L"GbLinkSelHost";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassExW(&wc);
  LogMsg("[link] frame thread: creating window...");
  
  HWND hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            L"STATIC", nullptr, WS_POPUP,
                            g_lselX, g_lselY, g_lselW, g_lselH, nullptr, nullptr, g_hInst, nullptr);
  if (!hw) { LogMsg("[link] frame thread: FAILED to create frame window"); return 1; }
  SetWindowLongPtrW(hw, GWLP_WNDPROC, (LONG_PTR)LselProc);   
  g_lselWnd = hw;
  LogMsg("[link] frame thread: window created");
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x11
#endif
  char sca[4] = {0};
  bool allowSelf = (GetEnvironmentVariableA("GB_ALLOW_SELF_CAPTURE", sca, sizeof(sca)) > 0 && sca[0] == '1');
  MarkerExcludeFromCapture(hw, "link", allowSelf);
  LogMsg("[link] frame thread: affinity set");
  ShowWindow(hw, SW_SHOWNORMAL);
  LogMsg("[link] frame thread: shown");
  GdiplusEnsure();              
  LselPaint(hw);
  
  SetTimer(hw, 9, 1500, nullptr);
  LogMsg("[link] frame thread: painted");
  LogMsg("[link] hollow frame window opened (drag title to move, edges to fine-tune)");
  
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (g_lselWnd == hw) g_lselWnd = nullptr;
  LogMsg("[link] frame thread exited");
  return 0;
}

static void LselOpenAt(int x, int y, int w, int h) {
  g_lselX = x; g_lselY = y; g_lselW = w; g_lselH = h;
  HANDLE t = CreateThread(nullptr, 0, LselThreadMain, nullptr, 0, nullptr);
  if (t) CloseHandle(t);
}

static void StartLinkFrame() {
  if (g_lselWnd && IsWindow(g_lselWnd)) {
    
    if (g_lselMarkOnly.exchange(false)) {
      RECT mr; GetWindowRect(g_lselWnd, &mr);
      SetWindowPos(g_lselWnd, nullptr, mr.left, mr.top - LSEL_TITLE_H,
                   mr.right - mr.left, (mr.bottom - mr.top) + LSEL_TITLE_H,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      PostMessageW(g_lselWnd, kMsgLinkFramePaint, 0, 0);
      LogMsg("[link] frame restored from mark-only");
    } else {
      LogMsg("[link] frame already open, keep");
    }
    return;
  }
  LogMsg("[link] StartLinkFrame: opening hollow frame window...");
  
  int w = 420, h = 420 + LSEL_TITLE_H;           
  int x = 0, y = 0;
  if (g_scanRegion.right > g_scanRegion.left && g_scanRegion.bottom > g_scanRegion.top) {
    x = g_scanRegion.left;
    y = g_scanRegion.top - LSEL_TITLE_H;
    w = g_scanRegion.right - g_scanRegion.left;
    h = (g_scanRegion.bottom - g_scanRegion.top) + LSEL_TITLE_H;
    g_lselOk = true;
  } else {
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(g_hwnd ? g_hwnd : GetDesktopWindow(),
                                     MONITOR_DEFAULTTOPRIMARY);
    RECT wa = { 0, 0, 1920, 1080 };
    if (GetMonitorInfoW(mon, &mi)) wa = mi.rcWork;
    x = wa.left + (wa.right - wa.left - w) / 2;
    y = wa.top + (wa.bottom - wa.top - h) / 2;
  }
  LselOpenAt(x, y, w, h);
  // ★ 2026-10-13 回退：开着「自动」重新开框时，先跑一遍全屏寻盘，让框自己收紧到棋盘上，
  //   而不是拿上一次的旧区域去扫。同属 23:00 那一版的行为（10-12 22:09 加的）。
  if (g_lselAuto) {
    LselSetAutoScanRegion();
    StartLinkScanLoop();
    LogMsg("[link] frame reopened with auto on -> fullscreen board-search");
  }
  LogMsg("[link] StartLinkFrame done");
}

static bool LnkNum(const std::string& s, size_t from, size_t limit, double& out) {
  while (from < limit && (s[from] == ' ' || s[from] == ':')) ++from;
  const char* b = s.c_str() + from;
  char* e = nullptr;
  double v = strtod(b, &e);
  if (e == b) return false;
  out = v;
  return true;
}

static std::vector<double> LnkNumArr(const std::string& s, const char* key) {
  std::vector<double> out;
  size_t p = s.find(key);
  if (p == std::string::npos) return out;
  p = s.find('[', p);
  if (p == std::string::npos) return out;
  size_t e = s.find(']', p);
  if (e == std::string::npos) return out;
  size_t i = p + 1;
  while (i < e) {
    double v = 0;
    size_t save = i;
    if (LnkNum(s, i, e, v)) out.push_back(v);
    while (i < e && s[i] != ',') ++i;
    if (i == save) break;
    ++i;
  }
  return out;
}

static int LnkParseStones(const std::string& s, const char* key, int board[15][15], int color) {
  int n = 0;
  size_t p = s.find(key);
  if (p == std::string::npos) return 0;
  p = s.find('[', p);
  if (p == std::string::npos) return 0;
  size_t e = s.find(']', p);
  if (e == std::string::npos) return 0;
  size_t i = p + 1;
  while (i < e) {
    size_t bo = s.find('{', i);
    if (bo == std::string::npos || bo >= e) break;
    size_t bc = s.find('}', bo);
    if (bc == std::string::npos || bc > e) break;
    double x = -1, y = -1;
    size_t xe = s.find("\"x\"", bo);
    size_t ye = s.find("\"y\"", bo);
    if (xe != std::string::npos && xe < bc && LnkNum(s, xe + 3, bc, x) &&
        ye != std::string::npos && ye < bc && LnkNum(s, ye + 3, bc, y)) {
      int ix = (int)llround(x), iy = (int)llround(y);
      if (ix >= 0 && ix < 15 && iy >= 0 && iy < 15) { board[iy][ix] = color; ++n; }
    }
    i = bc + 1;
  }
  return n;
}

static void SaveLinkSnap() {
  static DWORD lastSnapSave = 0;
  DWORD nowMs = GetTickCount();
  if (nowMs - lastSnapSave < 15000) return;
  lastSnapSave = nowMs;
  int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  RECT rc = { vx, vy, vx + vw, vy + vh };
  std::vector<unsigned char> png = CaptureRectPng(rc);
  if (png.empty()) return;
  wchar_t tmp[MAX_PATH] = {0};
  GetTempPathW(MAX_PATH, tmp);
  std::wstring fp = Join(tmp, L"gomoku_link_miss_latest.png");
  FILE* f = _wfopen(fp.c_str(), L"wb");
  if (f) { fwrite(png.data(), 1, png.size(), f); fclose(f);
           LogMsg("[link] miss snapshot saved: link_miss_latest.png"); }
}

static void LinkScanTick() {
  
  if (!g_lselAuto && (g_scanRegion.right <= g_scanRegion.left ||
                      g_scanRegion.bottom <= g_scanRegion.top))
    return;
  
  bool fullscreen = (g_scanRegion.right <= g_scanRegion.left ||
                     g_scanRegion.bottom <= g_scanRegion.top);
  
  std::string excl;

  char rb[256];
  std::string body = "{\"size\":15";
  if (!fullscreen) {
    snprintf(rb, sizeof(rb), ",\"region\":[%ld,%ld,%ld,%ld]",
             (long)g_scanRegion.left, (long)g_scanRegion.top,
             (long)(g_scanRegion.right - g_scanRegion.left),
             (long)(g_scanRegion.bottom - g_scanRegion.top));
    body += rb;
    
    std::vector<double> gx, gy; double gsp = 0;
    {
      std::lock_guard<std::mutex> lk(g_linkGeoMtx);
      gx = g_geoX; gy = g_geoY; gsp = g_geoSpacing;
    }
    if (g_lselGrown && (int)gx.size() == 15 && (int)gy.size() == 15 && gsp > 8) {
      std::string gs = ",\"geometry\":{\"x_lines\":[";
      char nb[32];
      for (size_t i = 0; i < gx.size(); ++i) {
        snprintf(nb, sizeof(nb), "%.2f", gx[i]); gs += nb;
        if (i + 1 < gx.size()) gs += ",";
      }
      gs += "],\"y_lines\":[";
      for (size_t i = 0; i < gy.size(); ++i) {
        snprintf(nb, sizeof(nb), "%.2f", gy[i]); gs += nb;
        if (i + 1 < gy.size()) gs += ",";
      }
      snprintf(nb, sizeof(nb), "%.3f", gsp);
      gs += "],\"spacing\":" + std::string(nb) + "}";
      body += gs;
    }
  }
  if (!excl.empty()) body += ",\"exclude\":[" + excl + "]";
  // ★★ 2026-10-18 最终口径（用户：「这是找棋盘的逻辑，用我们昨天的，然后识别棋子的逻辑，用今天新的，
  //   识别参数恢复那 6 个参数」）：
  //   **定位 = 自制**（findBoardRects + refineGrid，vision 里 sync 不接管 Hough 找盘，见 gbscreen.cpp
  //   的「定位只走自制」注释）；**读子 = 同步算法**（readStonesSync / syncCellInfo，用户从大佬那拿的）。
  //   所以这里必须下发 algo=sync + 6 项参数（含 interval 之外的 5 个阈值），
  //   vision 侧 ScanDirect() 据此 useSync != nullptr ⇒ 只用 sync 的棋子黑白判定。
  {
    char sb[256];
    snprintf(sb, sizeof(sb),
             ",\"algo\":\"sync\",\"recog\":{\"gray_offset\":%d,\"black_offset\":%d,"
             "\"white_offset\":%d,\"black_percent\":%d,\"white_percent\":%d}",
             g_syncGray.load(), g_syncBlackOff.load(), g_syncWhiteOff.load(),
             g_syncBlackPct.load(), g_syncWhitePct.load());
    body += sb;
  }
  body += "}";

  int code = 0;
  std::string resp;
  
  if (VisScanCall(body, resp)) code = 200;
  
  if (code == 200 && resp.size() > 2 && resp[0] == '{')
    PostToLinkPageSoon("{\"type\":\"scan\",\"data\":" + resp + "}");
  else {
    
    static DWORD lastOfflinePost = 0;
    DWORD nowOff = GetTickCount();
    if (nowOff - lastOfflinePost >= 5000) {
      lastOfflinePost = nowOff;
      PostToLinkPageSoon("{\"type\":\"offline\"}");
    }
  }
  
  static DWORD lastScanLog = 0;
  DWORD nowS = GetTickCount();
  if (nowS - lastScanLog >= 2000) {
    lastScanLog = nowS;
    bool foundG = (resp.find("\"found\": true") != std::string::npos) ||
                  (resp.find("\"found\":true") != std::string::npos);
    std::string blkS, whtS;
    size_t pB = resp.find("\"black\"");
    if (pB != std::string::npos) blkS = resp.substr(pB, 80);
    size_t pW = resp.find("\"white\"");
    if (pW != std::string::npos) whtS = resp.substr(pW, 80);
    // ★ 2026-10-18：`reader=` 直接告诉我们是哪套读子算法在跑 ——
    //   vision 的 sync 分支会写 diag.sync=true（gbsync.cpp），自制读子不写这个键。
    //   `read/cells/hit` 是变化识别（帧指纹 + 逐格签名缓存）的实测指标：
    //     hit=1 → 整帧没变，一格都没重算；cells=读了几格/复用了几格；read=读子耗时 ms。
    const char* reader = (resp.find("\"sync\": true") != std::string::npos ||
                          resp.find("\"sync\":true") != std::string::npos) ? "sync" : "adaptive";
    int rMs = JsonIntAfter(resp, "sync_read_ms");
    int cRead = JsonIntAfter(resp, "sync_cells_read");
    int cReuse = JsonIntAfter(resp, "sync_cells_reused");
    const char* hit = (resp.find("\"sync_frame_hit\": true") != std::string::npos ||
                       resp.find("\"sync_frame_hit\":true") != std::string::npos) ? "1" : "0";
    LogMsg(std::string("[link-scan] code=") + std::to_string(code) + " found=" +
           (foundG ? "true" : "false") + " reader=" + reader +
           " hit=" + hit + " read=" + std::to_string(rMs) + "ms" +
           " cells=" + std::to_string(cRead) + "/" + std::to_string(cRead + (cReuse > 0 ? cReuse : 0)) +
           " resp=" + resp.substr(0, 60) +
           " |B:" + blkS + " |W:" + whtS);
  }
  bool ok = (code == 200 && resp.size() > 2 && resp[0] == '{' &&
             resp.find("\"black\"") != std::string::npos);
  if (!ok) {
    
    EnsureVisionServer();
    static DWORD lastMissLog = 0;
    DWORD nowMs = GetTickCount();
    if (nowMs - lastMissLog > 3000) {       
      lastMissLog = nowMs;
      std::string bodyPrev = resp.substr(0, 160);
      for (auto& ch : bodyPrev) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
      LogMsg("[link] scan miss: code=" + std::to_string(code) +
             " resp=" + std::to_string((int)resp.size()) +
             " region=" + std::to_string(g_scanRegion.right - g_scanRegion.left) + "x" +
             std::to_string(g_scanRegion.bottom - g_scanRegion.top) +
             " body=" + bodyPrev);
      
      static DWORD lastSnapSave = 0;
      if (nowMs - lastSnapSave > 15000) {
        lastSnapSave = nowMs;
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        RECT rc = { vx, vy, vx + vw, vy + vh };
        std::vector<unsigned char> png = CaptureRectPng(rc);
        if (!png.empty()) {
          wchar_t tmp[MAX_PATH] = {0};
          GetTempPathW(MAX_PATH, tmp);
          std::wstring fp = Join(tmp, L"gomoku_link_miss_latest.png");
          FILE* f = _wfopen(fp.c_str(), L"wb");
          if (f) { fwrite(png.data(), 1, png.size(), f); fclose(f);
                   LogMsg("[link] miss snapshot saved: link_miss_latest.png"); }
        }
      }
    }
  }
  int board[15][15] = {{0}};
  int nb = 0, nw = 0;
  if (ok) {
    nb = LnkParseStones(resp, "\"black\"", board, 1);
    nw = LnkParseStones(resp, "\"white\"", board, 2);
  }
  
  if (ok && nb + nw > 0) {
    int ring = 0, inner = 0;
    for (int yy = 0; yy < 15; ++yy)
      for (int xx = 0; xx < 15; ++xx) {
        if (!board[yy][xx]) continue;
        if (xx == 0 || yy == 0 || xx == 14 || yy == 14) ++ring; else ++inner;
      }
    if (ring >= 12 && inner == 0) {
      static DWORD lastRingLog = 0;
      DWORD nowR = GetTickCount();
      if (nowR - lastRingLog > 8000) {
        lastRingLog = nowR;
        LogMsg("[link] frame discarded (ring ghosts: " + std::to_string(ring) +
               " edge stones, empty inside)");
      }
      ok = false; nb = 0; nw = 0;
      memset(board, 0, sizeof(board));
    }
  }
  
  static int lkSwampStreak = 0;          
  static DWORD lastSwampLog = 0, lastBurstLog = 0;
  if (ok) {
    bool swamp = false;
    size_t swp = resp.find("not_swamped");
    if (swp != std::string::npos) {
      
      size_t tv = resp.find("true", swp);
      size_t fv = resp.find("false", swp);
      swamp = (fv != std::string::npos && (tv == std::string::npos || fv < tv));
    }
    if (swamp) {
      
      bool hasGrid = (LnkNumArr(resp, "\"x_lines\"").size() == 15 &&
                      LnkNumArr(resp, "\"y_lines\"").size() == 15);
      if (hasGrid) {
        
        static int lkSwampCntr = 0;
        static DWORD lastCntrLog = 0;
        if (nb + nw < 9 && lkSwampCntr < 2) {
          ++lkSwampCntr;
          DWORD nowC = GetTickCount();
          if (nowC - lastCntrLog > 5000) {
            lastCntrLog = nowC;
            LogMsg("[link] swamp/parse contradiction (" + std::to_string(nb) + "B " +
                   std::to_string(nw) + "W but not_swamped) -> drop frame, clear geo, full re-read");
          }
          { std::lock_guard<std::mutex> lk(g_linkGeoMtx);
            g_geoX.clear(); g_geoY.clear(); g_geoSpacing = 0; }
          g_linkRescanSoon.store(true);      
          ok = false; nb = 0; nw = 0;
          memset(board, 0, sizeof(board));
        } else {
          if (nb + nw < 9)
            LogMsg("[link] dense board adopted after contradiction streak (grid present, " +
                   std::to_string(nb) + "B " + std::to_string(nw) + "W)");
          lkSwampCntr = 0;
          lkSwampStreak = 0;
          LogMsg("[link] dense board kept (grid present, " + std::to_string(nb) +
                 "B " + std::to_string(nw) + "W)");
        }
      } else {
        DWORD nowS = GetTickCount();
        if (nowS - lastSwampLog > 8000) {
          lastSwampLog = nowS;
          LogMsg("[link] frame discarded (not_swamped no-grid: " + std::to_string(nb) +
                 "B " + std::to_string(nw) + "W)");
        }
        ok = false; nb = 0; nw = 0;
        memset(board, 0, sizeof(board));
        SaveLinkSnap();
      }
    } else if (std::max(nb, nw) > 16 && std::max(nb, nw) > 2 * std::min(nb, nw) + 6) {
      
      if (++lkSwampStreak >= 3) {
        lkSwampStreak = 0;
        if (g_lselAuto && g_lselGrown) {
          LselSetAutoScanRegion();
          PostToLinkPageSoon("{\"type\":\"linkState\",\"frame\":\"pending\"}");
          LogMsg("[link] one-side burst x3 -> unlock & re-search fullscreen (frame manually if persists)");
        }
      }
      DWORD nowB = GetTickCount();
      if (nowB - lastBurstLog > 8000) {
        lastBurstLog = nowB;
        LogMsg("[link] frame discarded (one-side burst B" + std::to_string(nb) +
               " W" + std::to_string(nw) + " - mis-framed region)");
      }
      ok = false; nb = 0; nw = 0;
      memset(board, 0, sizeof(board));
    } else {
      lkSwampStreak = 0;
    }
  }
  
  bool found = ok && ((nb + nw) > 0 ||
                      (LnkNumArr(resp, "\"x_lines\"").size() == 15 &&
                       LnkNumArr(resp, "\"y_lines\"").size() == 15));
  
  if (found) {
    std::vector<double> xs = LnkNumArr(resp, "\"x_lines\"");
    if (xs.empty()) xs = LnkNumArr(resp, "\"xs\"");
    std::vector<double> ys = LnkNumArr(resp, "\"y_lines\"");
    if (ys.empty()) ys = LnkNumArr(resp, "\"ys\"");
    double spacing = 0;
    {
      std::lock_guard<std::mutex> lk(g_linkGeoMtx);
      if ((int)xs.size() == 15 && (int)ys.size() == 15) {
        g_geoX = xs; g_geoY = ys;
        if (xs.size() >= 2) spacing = xs[1] - xs[0];
        g_geoSpacing = spacing;
      }
    }
    if (spacing > 8 && (int)xs.size() == 15 && (int)ys.size() == 15) {
      RECT cand = { (LONG)xs.front(), (LONG)ys.front(),
                    (LONG)xs.back(), (LONG)ys.back() };
      
      const int e = (int)llround(spacing * 0.8);
      RECT tight = { cand.left - e, cand.top - e, cand.right + e, cand.bottom + e };
      if (!g_lselGrown) {
        
        const LONG TOL = 4;
        bool same = (labs(cand.left - g_tightPendRect.left) <= TOL &&
                     labs(cand.top - g_tightPendRect.top) <= TOL &&
                     labs(cand.right - g_tightPendRect.right) <= TOL &&
                     labs(cand.bottom - g_tightPendRect.bottom) <= TOL);
        if (same) ++g_tightPendStreak;
        else { g_tightPendRect = cand; g_tightPendStreak = 1; }
        if (g_tightPendStreak >= 2) {
          
          g_scanRegion = tight;
          g_lselGrown = true;
          LselFollowRect(tight);
          LogMsg("[link] region tightened to grid +/-0.6 cell and locked (" +
                 std::to_string(tight.right - tight.left) + "x" +
                 std::to_string(tight.bottom - tight.top) + ")");
        }
        
      } else if (g_lselAuto) {
        
        bool same = (labs(cand.left   - g_tightPendRect.left)   <= 4 &&
                     labs(cand.top    - g_tightPendRect.top)    <= 4 &&
                     labs(cand.right  - g_tightPendRect.right)  <= 4 &&
                     labs(cand.bottom - g_tightPendRect.bottom) <= 4);
        if (same) ++g_tightPendStreak;
        else { g_tightPendRect = cand; g_tightPendStreak = 1; }
        g_scanRegion = tight;                 
        if (g_tightPendStreak >= 2) LselFollowRect(tight);   
      }
    }
  }
  
  bool vis = found || (g_lselGrown && g_lselOk);
  
  if (found) g_linkMissStreak.store(0); else g_linkMissStreak.fetch_add(1);
  if (g_lselWnd && IsWindow(g_lselWnd) && g_lselOk != vis) {
    g_lselOk = vis;
    PostMessageW(g_lselWnd, kMsgLinkFramePaint, 0, 0);
  } else {
    g_lselOk = vis;
  }
  if (!found) {
    if (g_lselAuto) {                 
      
      static int autoMiss = 0;
      if (++autoMiss >= 4) {
        autoMiss = 0;
        if (g_lselGrown) {
          LselSetAutoScanRegion();
          LogMsg("[link] auto: no board, re-search fullscreen");
        }
      }
    }
    
    return;
  }
  if (g_lselAuto) { static int autoMiss = 0; autoMiss = 0; (void)autoMiss; }
  
  char key[225 + 2 * 225 + 1];        
  int w = 0;
  for (int y = 0; y < 15; ++y)
    for (int x = 0; x < 15; ++x)
      if (board[y][x] && w < 225) key[w++] = (char)('0' + board[y][x] * 3 + x % 3);
  key[w] = 0;
  for (int y = 0; y < 15; ++y)
    for (int x = 0; x < 15; ++x)
      if (board[y][x] && w < 675) { key[w++] = (char)('a' + y); key[w++] = (char)('A' + x); }
  key[w] = 0;
  if (key == g_lastBoardKey) return;
  
  if (!g_lselGrown) { g_linkRescanSoon.store(true); return; }
  
  g_linkFastLeft.store(3);          
  g_lastBoardKey = key;
  memcpy(g_linkBoard, board, sizeof(board));
  
  {
    std::string bl, wh;
    for (int y = 0; y < 15; ++y)
      for (int x = 0; x < 15; ++x) {
        if (g_linkBoard[y][x] == 1) { char b[32]; snprintf(b, 32, "[%d,%d],", x, y); bl += b; }
        else if (g_linkBoard[y][x] == 2) { char b[32]; snprintf(b, 32, "[%d,%d],", x, y); wh += b; }
      }
    if (!bl.empty()) bl.pop_back();
    if (!wh.empty()) wh.pop_back();
    PostToLinkPageSoon("{\"type\":\"linkBoard\",\"black\":[" + bl + "],\"white\":[" + wh +
                       "],\"b\":" + std::to_string(nb) + ",\"w\":" + std::to_string(nw) + "}");
  }
  
  {
    std::string bl, wh;
    for (int y = 0; y < 15; ++y)
      for (int x = 0; x < 15; ++x) {
        if (g_linkBoard[y][x] == 1) { char b[32]; snprintf(b, 32, "{\"x\":%d,\"y\":%d},", x, y); bl += b; }
        else if (g_linkBoard[y][x] == 2) { char b[32]; snprintf(b, 32, "{\"x\":%d,\"y\":%d},", x, y); wh += b; }
      }
    if (!bl.empty()) bl.pop_back();
    if (!wh.empty()) wh.pop_back();
    std::string payload = "{\"black\":[" + bl + "],\"white\":[" + wh + "]}";
    
    PostToPageSoon("{\"type\":\"linkPos\",\"payload\":" + JsonQuote(payload) + "}");
    LogMsg("[link] projected to trainer: B" + std::to_string(nb) + " W" + std::to_string(nw));
  }
}

static void StartLinkScanLoop() {
  if (g_linkScanRun.exchange(true)) return;         
  StartLinkFrameWatch();                            
  std::thread([] {
    LogMsg("[link] scan loop started (locked 50ms rhythm + adaptive 100/150/200/1000ms)");
    while (g_linkScanRun.load() && g_linkSession.load()) {
      ULONGLONG t0 = GetTickCount64();
      LinkScanTick();
      ULONGLONG el = GetTickCount64() - t0;
      
      if (g_lselGrown && g_lselOk) {
        
        int fl = g_linkFastLeft.load();
        if (g_linkRescanSoon.exchange(false) || fl > 0) {
          if (fl > 0) g_linkFastLeft.store(fl - 1);
          
          Sleep(100);
        } else {
          // Fixed 50ms locked rhythm instead of the frame watch event: a
          // steady poll keeps per-tick latency uniform and engine load flat
          // (stone-change capture target ~50ms; the 10-04 vision DLL still
          // needs its ~65ms full recognition per tick, which is the floor).
          int remain = 50 - (int)el;
          if (remain < 10) remain = 10;
          Sleep(remain);
        }
      } else {
        int period = 1000;
        int fl = g_linkFastLeft.load();
        if (fl > 0) { period = 100; g_linkFastLeft.store(fl - 1); }
        
        else if (!g_lselAuto &&
                 (g_scanRegion.right > g_scanRegion.left &&
                  g_scanRegion.bottom > g_scanRegion.top))
          period = g_syncInterval.load();   // ★ 2026-10-18：非锁定扫描周期由「识别参数」的「同步时间间隔」接管（锁定分支仍 50ms）
        
        else if (g_linkMissStreak.load() >= 3) period = 1000;
        
        else if (g_lselAuto) period = 150;
        else period = 1000;
        int slp = period - (int)el;
        if (slp < 40) slp = 40;
        Sleep(slp);
      }
    }
    g_linkScanRun.store(false);      
                                     
    LogMsg("[link] scan loop stopped");
  }).detach();
}

static std::atomic<bool> g_adBusyLink(false);

static std::atomic<int> g_adPipePref(0);

static std::string LnkWinDesc(HWND h) {
  if (!h) return "none";
  wchar_t cls[96] = {0};
  GetClassNameW(h, cls, 96);
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  char buf[192];
  snprintf(buf, sizeof(buf), "class=%s pid=%lu", WideToUtf8(cls).c_str(),
           (unsigned long)pid);
  return std::string(buf);
}

static HWND LnkTargetWindowAt(POINT pt) {
  const DWORD me = GetCurrentProcessId();
  for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
    if (!IsWindowVisible(h) || IsIconic(h)) continue;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TRANSPARENT) continue;              
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == me) continue;                           
    RECT wr;
    if (!GetWindowRect(h, &wr)) continue;
    if (pt.x < wr.left || pt.x >= wr.right || pt.y < wr.top || pt.y >= wr.bottom) continue;
    return h;
  }
  return nullptr;
}

static void LnkSetOverlaysClickThrough(bool on) {
  HWND wins[3] = { g_lselWnd, g_fsWnd, g_visSelHwnd };
  for (int i = 0; i < 3; ++i) {
    HWND h = wins[i];
    if (!h || !IsWindow(h)) continue;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    LONG nx = on ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
    if (nx != ex) {
      SetWindowLongW(h, GWL_EXSTYLE, nx);
      if (!on) PostMessageW(h, kMsgLinkFramePaint, 0, 0);   
    }
  }
}

struct LnkMouseShield {
  bool armed;
  explicit LnkMouseShield(bool enable) : armed(enable) {
    
    if (armed) { LnkSetOverlaysClickThrough(true); Sleep(20); }   
  }
  ~LnkMouseShield() { if (armed) LnkSetOverlaysClickThrough(false); }
};

static void MarkerRaiseAll() {
  MarkerRaiseTopmost(g_lselWnd);
  MarkerRaiseTopmost(g_fsWnd);
  MarkerRaiseTopmost(g_visSelHwnd);
  MarkerRaiseTopmost(g_linkHwnd);      
}

static void LnkActivateTarget(HWND target) {
  if (!target || !IsWindow(target)) return;
  HWND fg = GetForegroundWindow();
  if (fg == target) return;
  if (IsIconic(target)) ShowWindow(target, SW_RESTORE);   
  
  DWORD curTid = GetCurrentThreadId();
  DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
  bool attached = false;
  if (fgTid && fgTid != curTid) {
    attached = (AttachThreadInput(curTid, fgTid, TRUE) != FALSE);
  }
  SetForegroundWindow(target);
  if (attached) AttachThreadInput(curTid, fgTid, FALSE);
  
  Sleep(45);                                              
  
  MarkerRaiseAll();
  LogMsg("[link] activate target " + LnkWinDesc(target) +
         " fg_now=" + LnkWinDesc(GetForegroundWindow()));
}

static int LnkGlideSteps(int dist) {
  int s = dist / 12;
  if (s < 8) s = 8;        
  if (s > 40) s = 40;      
  return s;
}

static void LnkGlideCursor(int x, int y) {
  POINT cur = { 0, 0 };
  if (!GetCursorPos(&cur)) { SetCursorPos(x, y); return; }
  int dx = x - cur.x, dy = y - cur.y;
  int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  if (adx <= 1 && ady <= 1) { SetCursorPos(x, y); return; }
  int steps = LnkGlideSteps(adx + ady);
  
  timeBeginPeriod(1);
  for (int i = 1; i <= steps; ++i) {
    double t = (double)i / (double)steps;
    double e = t * t * (3.0 - 2.0 * t);                 
    SetCursorPos(cur.x + (int)llround(dx * e), cur.y + (int)llround(dy * e));
    Sleep(4);                                           
  }
  timeEndPeriod(1);
  SetCursorPos(x, y);                                   
}

static void LnkDropPost(HWND target, int sx, int sy, int count) {
  POINT cp = { sx, sy };
  if (!ScreenToClient(target, &cp)) { cp.x = sx; cp.y = sy; }
  LPARAM lp = MAKELPARAM((SHORT)cp.x, (SHORT)cp.y);
  PostMessageW(target, WM_MOUSEMOVE, 0, lp);
  for (int i = 0; i < count; ++i) {
    PostMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    PostMessageW(target, WM_LBUTTONUP, 0, lp);
    if (i < count - 1) Sleep(120);
  }
}
static void LnkDropSendInput(int x, int y, int count) {
  POINT old = { 0, 0 };
  GetCursorPos(&old);
  
  int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  int w1 = (vw > 1) ? vw - 1 : 1, h1 = (vh > 1) ? vh - 1 : 1;
  auto nx = [vx, w1](int v) -> DWORD { return (DWORD)llround((double)(v - vx) * 65535.0 / (double)w1); };
  auto ny = [vy, h1](int v) -> DWORD { return (DWORD)llround((double)(v - vy) * 65535.0 / (double)h1); };
  
  for (int i = 0; i < count; ++i) {
    INPUT mv = {}; mv.type = INPUT_MOUSE;
    mv.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    mv.mi.dx = nx(x); mv.mi.dy = ny(y);
    SendInput(1, &mv, sizeof(mv));
    Sleep(12);                          
    INPUT dn = {}; dn.type = INPUT_MOUSE; dn.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &dn, sizeof(dn));
    Sleep(16);                          
    INPUT up = {}; up.type = INPUT_MOUSE; up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &up, sizeof(up));
    if (i < count - 1) Sleep(120);
  }
  INPUT back = {}; back.type = INPUT_MOUSE;
  back.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
  back.mi.dx = nx(old.x); back.mi.dy = ny(old.y);
  SendInput(1, &back, sizeof(back));
}
static void LnkDropReal(int x, int y, int count) {
  
  LnkGlideCursor(x, y);
  for (int i = 0; i < count; ++i) {
    SetCursorPos(x, y);                 
    Sleep(20);                          
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(26);                          
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    if (i < count - 1) Sleep(140);
  }
}

static void LnkRunDropPipelines(int x, int y, int count, bool force, bool real,
                                const char* tag) {
  POINT pt = { x, y };
  HWND target = LnkTargetWindowAt(pt);          
  HWND hit0 = WindowFromPoint(pt);              
  const int pref = g_adPipePref.load();
  
  const bool useReal = real;
  const int  prefEff = (pref == 3) ? 2 : pref;
  const bool useSi   = !useReal && (force || prefEff == 2);
  const bool usePost = !useReal && !useSi && target != nullptr;
  const char* pipe = useReal ? "real" : (useSi ? "sendinput" : (usePost ? "post" : "sendinput"));
  std::string tgt = "-";
  
  const bool coveredByUs = (target != nullptr && hit0 != nullptr && hit0 != target);
  const bool needShield = !usePost && coveredByUs;
  LnkMouseShield shield(needShield);
  HWND hit1 = needShield ? WindowFromPoint(pt) : hit0;   
  
  if (!usePost && GetForegroundWindow() != target) LnkActivateTarget(target);
  LogMsg(std::string("[") + tag + "] drop probe (" + std::to_string(x) + "," +
         std::to_string(y) + ") target=" + LnkWinDesc(target) +
         " hit_plain=" + LnkWinDesc(hit0) + " hit_shielded=" + LnkWinDesc(hit1));
  if (useReal) {
    LnkDropReal(x, y, count);
  } else if (useSi) {
    LnkDropSendInput(x, y, count);
  } else if (usePost) {
    
    LnkDropPost(target, x, y, count);
    tgt = LnkWinDesc(target);
  } else {
    LnkDropSendInput(x, y, count);              
  }
  LogMsg(std::string("[") + tag + "] autodrop done pipeline=" + pipe + " (" +
         std::to_string(x) + "," + std::to_string(y) + ") x" + std::to_string(count) +
         " target=" + tgt);
  PostToLinkPageSoon("{\"type\":\"autodropDone\",\"pipeline\":\"" + std::string(pipe) +
                     "\",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) +
                     ",\"count\":" + std::to_string(count) + "}");
}

static void LinkAutoDropAtXY(int x, int y, int count, bool force, bool real) {
  if (g_adBusyLink.exchange(true)) return;      
  std::thread([x, y, count, force, real] {
    
    LnkRunDropPipelines(x, y, count, force, real, "link-bridge");
    g_adBusyLink = false;
  }).detach();
}

static void LinkAutoDropAt(int cx, int cy, bool force, bool real) {
  if (!g_linkAuto.load() && !force) { LogMsg("[link] auto-drop off -> dropped"); return; }
  
  if (g_linkDropOnce) { g_linkDropOnce = false; g_linkAuto.store(false); }
  double sx = -1, sy = -1;
  {
    std::lock_guard<std::mutex> lk(g_linkGeoMtx);
    if ((int)g_geoX.size() == 15 && (int)g_geoY.size() == 15 &&
        cx >= 0 && cx < 15 && cy >= 0 && cy < 15) {
      sx = g_geoX[cx]; sy = g_geoY[cy];
    }
  }
  if (sx < 0) { LogMsg("[link] no locked geometry -> drop lost"); return; }
  int x = (int)llround(sx), y = (int)llround(sy), count = g_linkClicks.load();
  if (count < 1) count = 1;
  if (g_adBusyLink.exchange(true)) return;
  std::thread([x, y, count, force, real] {
    
    LnkRunDropPipelines(x, y, count, force, real, "link");
    g_adBusyLink = false;
  }).detach();
}

static LRESULT CALLBACK LinkWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      PaintSplash(h, g_linkPageReady, kBrandNameLink);
      return 0;
    
    case WM_WINDOWPOSCHANGING:
      return MarkerOnWndPosChanging(l);
    case WM_SIZE:
      LayoutLinkWebView();
      return 0;
    
    case WM_CLOSE:
      SaveLinkWinPos();
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      SaveLinkWinPos();
      LselClose();                              
      if (g_linkSession.exchange(false)) {
        ApplyMainLinkAspect(false);             
        PostToPageSoon("{\"type\":\"linkOn\",\"on\":false}");
        
        PostToPageSoon("{\"type\":\"linkClosed\"}");
      }
      
      if (g_linkController) { g_linkController->Release(); g_linkController = nullptr; }
      if (g_linkWebview) { g_linkWebview->Release(); g_linkWebview = nullptr; }
      g_linkHwnd = nullptr;
      g_linkCreating = false;
      g_linkPending.clear();
      g_linkShown = false;
      g_linkPageReady = false;
      LogMsg("[link] link window destroyed");
      return 0;
    default:
      break;
  }
  return DefWindowProcW(h, m, w, l);
}

class LinkMsgHandler : public Cb<ICoreWebView2WebMessageReceivedEventHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*,
                                   ICoreWebView2WebMessageReceivedEventArgs* e) override {
    LPWSTR raw = nullptr;
    if (FAILED(e->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
    std::string s = WideToUtf8(raw);
    CoTaskMemFree(raw);
    
    {
      LPWSTR raw2 = nullptr;
      if (SUCCEEDED(e->TryGetWebMessageAsString(&raw2)) && raw2) {
        std::string t = WideToUtf8(raw2);
        CoTaskMemFree(raw2);
        // ★ 2026-10-17：WebView2 的 TryGetWebMessageAsString 只对「以**字符串**形式发出」的
        //   消息成功，即页面必须 postMessage(JSON.stringify(o))。页面若直接
        //   postMessage({...})（对象消息），这里取到的是空串 ⇒ 下面整条 { } 分支被跳过
        //   ⇒ 消息**静默丢弃**。实测事故：bridge.js 的 bridgeDiag 就是这么被丢的，
        //   LINK_ACK_TIMEOUT / LINK_ENG_STATUS / LINK_BOOT_GUARD / LINK_TT_RESET /
        //   LINK_EVAL_FAIL 等 15 个诊断点一个都到不了日志。
        //   兜底：t 为空时改用 AsJson 形式（s）—— 此时它正好是干净的 JSON 对象文本。
        if (t.empty() && !s.empty() && s[0] == '{') t = s;
        if (!t.empty() && t[0] == '{') {
          
          if (t.find("\"type\":\"autodrop\"") != std::string::npos) {
            if (t.find("\"on\":true") != std::string::npos) {
              int ax = JsonIntAfter(t, "\"x\":"), ay = JsonIntAfter(t, "\"y\":");
              int ac = JsonIntAfter(t, "\"count\":"); if (ac < 1) ac = 1;
              bool af = t.find("\"force\":true") != std::string::npos;
              
              bool ar = t.find("\"real\":true") != std::string::npos;
              LogMsg("[link-bridge] autodrop req (" + std::to_string(ax) + "," +
                     std::to_string(ay) + ") x" + std::to_string(ac) +
                     (ar ? " [real]" : (af ? " [force]" : "")));
              
              LinkAutoDropAtXY(ax, ay, ac, af, ar);
            }
            return S_OK;
          }
          
          // 引擎通道：ask/stop/caps/config 原样交宿主引擎层（ack/tick 由它推回连线页）
          if (t.find("\"engine\"") != std::string::npos && t.find("\"op\":") != std::string::npos) {
            std::string op = JsonStrAfter(t, "\"op\":");
            int id = JsonIntAfter(t, "\"id\":");
            eng::HandleOp(op, id, t, true);
            return S_OK;
          }

          if (t.find("\"type\":\"adPipeOk\"") != std::string::npos) {
            int id = 0;
            if (t.find("\"pipe\":\"sendinput\"") != std::string::npos) id = 2;
            else if (t.find("\"pipe\":\"real\"") != std::string::npos) id = 3;
            else if (t.find("\"pipe\":\"post\"") != std::string::npos) id = 1;
            if (id) {
              int was = g_adPipePref.exchange(id);
              if (was != id)
                LogMsg("[link-bridge] autodrop pipeline pinned -> " +
                       std::string(id == 1 ? "post" : (id == 2 ? "sendinput" : "real")));
            }
            return S_OK;
          }
          
          if (t.find("\"type\":\"ensureDeps\"") != std::string::npos) {
            LogMsg("[link-bridge] ensureDeps ping (engine self-heals via /engine/ensure)");
            return S_OK;
          }
          
          if (t.find("\"type\":\"log\"") != std::string::npos) {
            LogMsg("[bridge] " + JsonStrAfter(t, "\"text\":"));
            return S_OK;
          }
          
          if (t.find("\"type\":\"status\"") != std::string::npos) {
            LogMsg("[bridge-ui] " + JsonStrAfter(t, "\"text\":"));
            return S_OK;
          }
          
          if (t.find("\"type\":\"guide\"") != std::string::npos) {
            int bx = -1, by = -1;
            if (t.find("\"on\":true") != std::string::npos) {
              double sx = JsonIntAfter(t, "\"x\":"), sy = JsonIntAfter(t, "\"y\":");
              if (sx > 0 && sy > 0) LinkScreenToGrid(sx, sy, bx, by);
            }
            if (bx >= 0) {
              static int guideN = 0;
              if (++guideN <= 6)
                LogMsg("[link-bridge] guide circle -> grid (" + std::to_string(bx) + "," +
                       std::to_string(by) + ") on immune layer");
            }
            g_linkBestX.store(bx); g_linkBestY.store(by);
            if (g_lselWnd && IsWindow(g_lselWnd)) PostMessageW(g_lselWnd, kMsgLinkFramePaint, 0, 0);
            return S_OK;
          }
          
          if (t.find("\"type\":\"balance\"") != std::string::npos) {
            std::vector<std::pair<int,int>> nb, nw;
            std::vector<int> nsb, nsw;
            if (t.find("\"on\":true") != std::string::npos) {
              size_t cp = t.find("\"cells\":\"");
              if (cp != std::string::npos) {
                size_t i = cp + 9;
                while (i < t.size() && t[i] != '"') {
                  int v[3] = {0, 0, 0}; int got = 0;
                  while (got < 3 && i < t.size() && t[i] != ';' && t[i] != '"') {
                    while (i < t.size() && (t[i] < '0' || t[i] > '9') && t[i] != '-') ++i;
                    if (i >= t.size() || t[i] == ';' || t[i] == '"') break;
                    int sign = 1; if (t[i] == '-') { sign = -1; ++i; }
                    int val = 0;
                    while (i < t.size() && t[i] >= '0' && t[i] <= '9') { val = val * 10 + (t[i] - '0'); ++i; }
                    v[got++] = val * sign;
                    if (i < t.size() && t[i] == ',') ++i;
                  }
                  char color = 0;
                  while (i < t.size() && t[i] != ';' && t[i] != '"') {
                    if (t[i] == 'b' || t[i] == 'w') color = t[i];
                    ++i;
                  }
                  if (got == 3 && v[2] >= 0 && color) {
                    int gx = -1, gy = -1;
                    if (LinkScreenToGrid(v[0], v[1], gx, gy)) {
                      if (color == 'b') { nb.push_back({ gx, gy }); nsb.push_back(v[2]); }
                      else              { nw.push_back({ gx, gy }); nsw.push_back(v[2]); }
                    }
                  }
                  if (i < t.size() && t[i] == ';') ++i;
                  else break;
                }
              }
            }
            g_lkBalB = nb; g_lkBalW = nw;
            g_lkBalBSeq = nsb; g_lkBalWSeq = nsw;
            LogMsg("[link-bridge] balance marks: B" + std::to_string((int)nb.size()) +
                   " W" + std::to_string((int)nw.size()) + " (immune layer, seq labels)");
            if (g_lselWnd && IsWindow(g_lselWnd)) PostMessageW(g_lselWnd, kMsgLinkFramePaint, 0, 0);
            return S_OK;
          }
          
          return S_OK;
        }
      }
    }
    if (s.find("\"ready\"") != std::string::npos) {
      g_linkPageReady = true;
      ApplyDarkTitleBar(g_linkHwnd, g_uiDark);
      RevealLinkPage();
      SetLinkBackground(JsonStrAfter(s, "\"bg\":"));
      LogMsg("[link] link page ready");
      if (!g_linkPending.empty()) {
        std::string p = g_linkPending;
        g_linkPending.clear();
        PostToLinkPage(p);
      }
      // ★ 2026-10-18：把宿主落盘的「识别参数」回填给连线页浮层（页面自身 localStorage 不可靠）
      PostToLinkPage("{\"type\":\"linkRecog\",\"recog\":" + LinkRecogJson() + "}");
      // ★★ 2026-10-19（用户要的「读档」抽屉）：把宿主落盘的**存档列表**整包回填。
      //   窗口标题栏等都由宿主来；这里只搬 JSON，页面自己 JSON.parse。
      PostToLinkPage("{\"type\":\"linkRecogSlots\",\"json\":" + JsonQuote(ReadLinkRecogSlots()) + "}");
      LogMsg("[link] recog slots backfilled: " + std::to_string(ReadLinkRecogSlots().size()) + " bytes");
      ShowLinkWindow();
      return S_OK;
    }
    
    if (s.find("\"linkSelect\"") != std::string::npos) {
      PostMessageW(g_hwnd, kMsgLinkSelect, 0, 0);
      return S_OK;
    }
    
    // ★★ 2026-10-18 修订（用户：「点击识别参数，棋盘上面的按键更换为各种参数」「调节后真正能
    //   识别的效果实时更新」）：识别参数从浮层小窗改为**就地编辑 + 实时联动**。
    //   · linkRecogPreview：滑动条/数值框一动就发一条 —— 宿主把值写进「有效参数」**并落盘**，
    //     再唤醒扫描循环。下一拍扫描就用新阈值读子 → 连线窗下方棋盘实时反映识别效果。
    //     ★★ 2026-10-19 修正（用户：「连线调整参数的持久化没有做好，关闭软件再打开，参数又恢复
    //     默认了」）：以前预览**不落盘**，用户拖完滑条就直接关软件 → 下次启动读的是上一次「应用」
    //     的那份（或出厂默认），观感就是「参数白调了」。现在预览也落盘：磁盘上那一份恒等于屏幕上
    //     正在生效的那一份，退出/断电都不会回退。（文件只有 ~17 字节，页面侧本身还有 120ms 防抖。）
    //   · linkRecogCancel：页面把「打开时那份原值」再发一次 —— 宿主回写有效参数并同样落盘
    //     （取消 = 回到上次已应用的那一份，那一份本来就该是磁盘上那一份）。
    //   · linkRecogCfg（「应用」）：写盘（单一权威）+ 回填页面（沿用旧逻辑）。
    auto lkApplyRecog = [](const std::string& js, bool persist) {
      int iv = JsonIntAfter(js, "\"interval\":");        if (iv < 0) iv = g_syncInterval.load();
      int gr = JsonIntAfter(js, "\"gray_offset\":");     if (gr < 0) gr = g_syncGray.load();
      int bo = JsonIntAfter(js, "\"black_offset\":");    if (bo < 0) bo = g_syncBlackOff.load();
      int wo = JsonIntAfter(js, "\"white_offset\":");    if (wo < 0) wo = g_syncWhiteOff.load();
      int bp = JsonIntAfter(js, "\"black_percent\":");   if (bp < 0) bp = g_syncBlackPct.load();
      int wp = JsonIntAfter(js, "\"white_percent\":");   if (wp < 0) wp = g_syncWhitePct.load();
      ClampLinkRecog(iv, gr, bo, wo, bp, wp);
      g_syncInterval.store(iv);
      g_syncGray.store(gr);
      g_syncBlackOff.store(bo);
      g_syncWhiteOff.store(wo);
      g_syncBlackPct.store(bp);
      g_syncWhitePct.store(wp);
      if (persist) SaveLinkRecogPrefs();
      g_linkRescanSoon.store(true);      // 立刻用新阈值重扫一次（锁定态 50ms 节拍）
      g_linkFastLeft.store(3);           // 后续几拍也走快速节拍，便于观察实时效果
    };

    // ★★ 2026-10-19（用户要的「存档 / 读档」）：页面把**整包**存档 JSON 发过来 → 原样写盘。
    //   载荷形如 {"type":"linkRecogSlots","json":"[{\"name\":…},…]"}；JsonStrAfter 的
    //   去转义规则与 JSON.stringify 的转义规则互为逆运算，所以这包内容能原样往返
    //   （名称里的引号/反斜杠也在内）。宿主不解析内容，只校验「是数组 + 体积合理」。
    //   ★ 这一条必须排在下面几个 linkRecog* 之前：存档里如果有个名字恰好叫「linkRecogCfg」，
    //     那包 JSON 里就会出现那个子串，被后面的分支先命中就会把存档当成「应用参数」处理。
    //     反过来，本页另外三种消息（Preview / Cancel / Cfg）都不会含 "linkRecogSlots"，无副作用。
    if (s.find("\"linkRecogSlots\"") != std::string::npos) {
      std::string blob = JsonStrAfter(s, "\"json\":");
      size_t bp = blob.find_first_not_of(" \r\n\t");
      if (bp != std::string::npos && blob[bp] == '[' && blob.size() <= 64 * 1024) {
        WriteLinkRecogSlots(blob);
        LogMsg("[link] recog slots saved: " + std::to_string(blob.size()) + " bytes");
      } else if (!blob.empty()) {
        LogMsg("[link] recog slots rejected (payload size " + std::to_string(blob.size()) + ")");
      }
      return S_OK;
    }

    if (s.find("\"linkRecogPreview\"") != std::string::npos) {
      lkApplyRecog(s, true);            // ★ 2026-10-19：预览也落盘（见上方注释）
      LogMsg("[link] recog live: " + LinkRecogJson());
      return S_OK;
    }

    if (s.find("\"linkRecogCancel\"") != std::string::npos) {
      lkApplyRecog(s, true);            // ★ 2026-10-19：还原后的那份也落盘
      LogMsg("[link] recog reverted to: " + LinkRecogJson());
      return S_OK;
    }

    // 连线「识别参数」编辑区「应用」→ 宿主写盘（单一权威）+ 回填页面
    if (s.find("\"linkRecogCfg\"") != std::string::npos) {
      lkApplyRecog(s, true);
      PostToLinkPage("{\"type\":\"linkRecog\",\"recog\":" + LinkRecogJson() + "}");
      LogMsg("[link] recog cfg applied: " + LinkRecogJson());
      return S_OK;
    }
    
    if (s.find("\"linkCfg\"") != std::string::npos) {
      g_linkAuto.store(JsonStrAfter(s, "\"auto\":").find("true") != std::string::npos);
      int clicks = JsonIntAfter(s, "\"clicks\":");
      if (clicks >= 1 && clicks <= 9) g_linkClicks.store(clicks);
      int rule = JsonIntAfter(s, "\"rule\":");
      if (rule < 0 || rule > 2) rule = 0;
      PostToPageSoon("{\"type\":\"linkCfg\",\"auto\":" +
                     std::string(g_linkAuto.load() ? "true" : "false") +
                     ",\"aiB\":" + std::string(JsonStrAfter(s, "\"aiB\":").find("true") != std::string::npos ? "true" : "false") +
                     ",\"aiW\":" + std::string(JsonStrAfter(s, "\"aiW\":").find("true") != std::string::npos ? "true" : "false") +
                     ",\"turnMs\":" + std::to_string(JsonIntAfter(s, "\"turnMs\":")) +
                     ",\"rule\":" + std::to_string(rule) + "}");
      LogMsg("[link] cfg: auto=" + std::string(g_linkAuto.load() ? "on" : "off") +
             " clicks=" + std::to_string(g_linkClicks.load()) + " rule=" + std::to_string(rule));
      return S_OK;
    }
    
    if (s.find("\"linkEval\"") != std::string::npos) {
      std::string payload = s;
      size_t p = payload.find('{');
      if (p != std::string::npos) payload = payload.substr(p);
      PostToLinkPageSoon(payload);
      return S_OK;
    }
    
    if (s.find("\"linkCfgSync\"") != std::string::npos) {
      std::string payload = s;
      size_t p = payload.find('{');
      if (p != std::string::npos) payload = payload.substr(p);
      PostToLinkPageSoon(payload);
      return S_OK;
    }
    
    if (s.find("\"linkBalMarks\"") != std::string::npos) {
      ParseBalPairs(s, "\"black\"", g_lkBalB);
      ParseBalPairs(s, "\"white\"", g_lkBalW);
      g_lkBalBSeq.assign(g_lkBalB.size(), 0);   
      g_lkBalWSeq.assign(g_lkBalW.size(), 0);
      LogMsg("[link] balance marks: B" + std::to_string((int)g_lkBalB.size()) +
             " W" + std::to_string((int)g_lkBalW.size()) + " (immune layer)");
      if (g_lselWnd && IsWindow(g_lselWnd)) {
        PostMessageW(g_lselWnd, kMsgLinkFramePaint, 0, 0);
      } else {
        
        PostToLinkPageSoon("{\"type\":\"linkTip\",\"txt\":\"请先框选棋盘并识别，再点交换平衡点 / Select board first\"}");
      }
      return S_OK;
    }
    
    if (s.find("\"linkBest\"") != std::string::npos) {
      LinkParseBest(s);
      return S_OK;
    }
    if (s.find("\"linkClose\"") != std::string::npos) {
      if (g_linkHwnd) PostMessageW(g_linkHwnd, WM_CLOSE, 0, 0);
      return S_OK;
    }
    if (s.find("uiTheme") != std::string::npos) {
      std::string lg = JsonStrAfterDecoded(s, JKey("lang").c_str());
      if (!lg.empty()) g_uiLangEn = (lg.rfind("en", 0) == 0);
      std::string th = JsonStrAfterDecoded(s, JKey("theme").c_str());
      if (th != "light" && th != "dark") th = "dark";
      SaveUiPrefs(s);
      g_uiDark = (th != "light");
      ApplyDarkTitleBar(g_linkHwnd, g_uiDark);
      return S_OK;
    }
    return S_OK;
  }
};

class LinkCtlHandler : public Cb<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* ctl) override {
    g_linkCreating = false;
    if (FAILED(hr) || !ctl || !g_linkHwnd) {
      LogMsg("[link] link controller creation failed");
      return S_OK;
    }
    g_linkController = ctl;
    ctl->AddRef();
    ctl->get_CoreWebView2(&g_linkWebview);
    if (!g_linkWebview) return S_OK;
    g_linkWebview->AddRef();
    HardenWebviewSettings(g_linkWebview, "link");
    SetupUiServing(g_linkWebview, "link");
    EventRegistrationToken tok = {};
    g_linkWebview->add_WebMessageReceived(new LinkMsgHandler(), &tok);
    g_linkWebview->Navigate(UiBaseUrl("link").c_str());
    SetWebviewBg(ctl, g_uiBg);
    ctl->put_IsVisible(FALSE);
    LayoutLinkWebView();
    LogMsg("[link] navigated to calc.html?link=1");
    return S_OK;
  }
};

static void CreateLinkWindow() {
  if (g_linkHwnd || g_linkCreating || !g_env) return;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = LinkWndProc;
  wc.hInstance = g_hInst;
  wc.lpszClassName = kLinkCls;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
  wc.hIconSm = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
  RegisterClassExW(&wc);
  
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 0, h = 0;
  {
    FILE* f = _wfopen(LinkWinPosFile().c_str(), L"rb");
    if (f) {
      WINDOWPLACEMENT wp = {};
      if (fread(&wp, 1, sizeof(wp), f) == sizeof(wp) && wp.length == sizeof(wp)) {
        RECT r = wp.rcNormalPosition;
        x = r.left; y = r.top;
        w = r.right - r.left; h = r.bottom - r.top;
      }
      fclose(f);
    }
  }
  int cw = 0, ch = 0;
  RECT work = { 0, 0, 0, 0 };
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  
  // ★ 2026-10-18 二次修订（用户：「这个连线，这个界面恢复之前的尺寸」）：恢复为加高之前的
  //   原始口径 304×712。「识别参数」进入后只留「参数区 + 下方棋盘」（已收起按键区/统计条/
  //   状态行），因此原高度够用。
  const int kLinkCssW = 304, kLinkCssH = 712;
  if (g_hwnd) {
    
    cw = GbPx(g_hwnd, kLinkCssW); ch = GbPx(g_hwnd, kLinkCssH);   
  } else {
    HDC dc = GetDC(nullptr);
    int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    cw = MulDiv(kLinkCssW, dpi, 96); ch = MulDiv(kLinkCssH, dpi, 96);
  }
  
  GbClampToWorkArea(g_hwnd, cw, ch);
  w = cw; h = ch;                                     
  if (x == CW_USEDEFAULT) {
    RECT mw = { 0, 0, 0, 0 };
    if (g_hwnd) GetWindowRect(g_hwnd, &mw);
    x = mw.right + 12;
    if (x + w > work.right) x = mw.left - w - 12;
    y = mw.top;
  }
  if (x < work.left) x = work.left;
  if (y < work.top) y = work.top;
  if (x + w > work.right) x = work.right - w;
  if (y + h > work.bottom) y = work.bottom - h;
  
  g_linkHwnd = CreateWindowExW(WS_EX_TOPMOST, kLinkCls, kLinkTitle,
                               (WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME)) | WS_CLIPCHILDREN,
                               x, y, w, h, nullptr, nullptr, g_hInst, nullptr);
  if (!g_linkHwnd) { LogMsg("[link] CreateWindowEx failed"); return; }
  
  {
    int dpiNow = GbDpiFor(g_linkHwnd), dpiRef = GbDpiFor(g_hwnd);
    if (dpiRef > 0 && dpiNow != dpiRef) {
      int nw = MulDiv(kLinkCssW, dpiNow, 96), nh = MulDiv(kLinkCssH, dpiNow, 96);
      GbClampToWorkArea(g_linkHwnd, nw, nh);
      SetWindowPos(g_linkHwnd, nullptr, 0, 0, nw, nh,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      LogMsg("[link] dpi remap " + std::to_string(dpiRef) + " -> " + std::to_string(dpiNow) +
             " (css " + std::to_string(kLinkCssW) + "x" + std::to_string(kLinkCssH) + ")");
    }
  }
  g_linkCreating = true;
  g_linkShown = false;
  g_linkPageReady = false;
  LogMsg("[link] link window created (no maximize, bounded resize)");
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x11
#endif
  MarkerExcludeFromCapture(g_linkHwnd, "link", false);   
  g_env->CreateCoreWebView2Controller(g_linkHwnd, new LinkCtlHandler());
}

static void OpenLinkWindow() {
  LogMsg("[link] openLink requested");
  PrewarmVisionServiceAsync();                 
  eng::Init();
  bool firstOpen = !g_linkSession.exchange(true);
  if (firstOpen) {
    ApplyMainLinkAspect(true);                 
    PostToPageSoon("{\"type\":\"linkOn\",\"on\":true}");
  }
  
  
  g_scanRegion.left = g_scanRegion.top = g_scanRegion.right = g_scanRegion.bottom = 0;
  g_lselGrown = false;
  g_lastBoardKey = GB_LINK_NO_BOARD_KEY;
  if (g_linkHwnd) {
    if (IsIconic(g_linkHwnd)) ShowWindow(g_linkHwnd, SW_RESTORE);
    ShowLinkWindow();
    SetForegroundWindow(g_linkHwnd);
    return;
  }
  if (g_linkCreating || !g_env) {
    LogMsg(g_linkCreating ? "[link] queued (window is being created)"
                          : "[link] queued (webview2 env not ready)");
    return;
  }
  CreateLinkWindow();
  if (g_linkHwnd) ShowLinkWindow();
  
}

static const wchar_t* kCls = L"GbCalcHost";
static const wchar_t* kTitle = L"Meter Gomoku";   

static std::wstring WinPosFile() {
  wchar_t tmp[MAX_PATH] = {0};
  GetTempPathW(MAX_PATH, tmp);
  return Join(std::wstring(tmp), L"GomokuTrainer.pos");
}
static void SaveWinPos() {
  if (!g_hwnd || IsIconic(g_hwnd)) return;
  WINDOWPLACEMENT wp = {};
  wp.length = sizeof(wp);
  if (!GetWindowPlacement(g_hwnd, &wp)) return;
  FILE* f = _wfopen(WinPosFile().c_str(), L"wb");
  if (!f) return;
  fwrite(&wp, 1, sizeof(wp), f);
  fclose(f);
}

// ---- settings window ------------------------------------------------------------------------
// A second top-level WebView2 window next to the main one: always 40% of the main window width
// and exactly its height, fixed ratio (no resize, no maximise). It hosts calc.html?set=1, which
// is the preferences panel (language / colours / which cards are shown). All state travels
// through localStorage, so the host only has to create, follow and destroy the window.
static const wchar_t* kSetCls = L"GbSetWin";
static const wchar_t* kSetTitle = L"Settings";
static const wchar_t* kBrandNameSet = L"Gomoku Settings";

static void LayoutSetWebView() {
  if (!g_setController || !g_setHwnd) return;
  RECT r;
  GetClientRect(g_setHwnd, &r);
  // never hand a zero rect to the webview: early startup paths can read an empty client
  if (!r.right || !r.bottom) {
    static DWORD last00 = 0;
    DWORD now00 = GetTickCount();
    if (now00 - last00 > 500) {
      last00 = now00;
      RECT w = { 0, 0, 0, 0 };
      GetWindowRect(g_setHwnd, &w);
      char b[160];
      snprintf(b, sizeof(b), "[set] empty client (win %ldx%ld vis=%d iconic=%d)",
               (long)(w.right - w.left), (long)(w.bottom - w.top),
               (int)IsWindowVisible(g_setHwnd), (int)IsIconic(g_setHwnd));
      LogMsg(b);
    }
    return;
  }
  g_setController->put_Bounds(r);
  static long lastW = -1, lastH = -1;
  if (r.right != lastW || r.bottom != lastH) {
    lastW = r.right; lastH = r.bottom;
    char b[96];
    snprintf(b, sizeof(b), "[set] bounds %ldx%ld", (long)r.right, (long)r.bottom);
    LogMsg(b);
  }
}
static void RevealSetPage() {
  if (g_setController) g_setController->put_IsVisible(TRUE);
  if (g_setHwnd && IsWindow(g_setHwnd)) InvalidateRect(g_setHwnd, nullptr, TRUE);
}
// Fixed-size settings window: 410 x 656 CSS px (= 0.8 x the previous 0.4-of-main-window width
// x main-window height). Screen adaptation uses the SAME algorithm as the main window
// (GbPx DIP->physical + GbClampToWorkArea), so it fits any resolution / DPI exactly like the
// main UI does. Created once, never resized, never follows the main window.
static void SetWinDesiredRect(RECT* out) {
  RECT mr = { 0, 0, 0, 0 };
  if (g_hwnd) GetWindowRect(g_hwnd, &mr);

  // width : height = 1 : 1.618 (golden ratio), 410 CSS px wide
  int cw = GbPx(g_hwnd, 410), ch = GbPx(g_hwnd, 663);   // per-monitor DPI of the main window
  GbClampToWorkArea(g_hwnd, cw, ch);

  RECT work = { 0, 0, 0, 0 };
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

  int x = mr.right + 10, y = mr.top;
  if (x + cw > work.right) x = mr.left - cw - 10;
  if (x < work.left) x = work.left;
  if (x + cw > work.right) x = work.right - cw;
  y = mr.top;
  if (y + ch > work.bottom) y = work.bottom - ch;
  if (y < work.top) y = work.top;

  out->left = x; out->top = y;
  out->right = x + cw; out->bottom = y + ch;
}
static LRESULT CALLBACK SetWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    // NOTE: no WM_GETMINMAXINFO here - it fires during WM_NCCREATE, when GetWindowRect still
    // returns the 24x24 default, and clamps the window to that forever. The window has no
    // WS_THICKFRAME, so it cannot be resized by the user anyway.
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      PaintSplash(h, g_setPageReady, kBrandNameSet);
      return 0;
    case WM_SIZE:
      if (w == SIZE_RESTORED || w == SIZE_MAXIMIZED) {
        // back from the taskbar: claim the privileged (topmost) level again
        if (!(GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
          SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
          LogMsg("[set] restored -> topmost again");
        }
      }
      LayoutSetWebView();
      return 0;
    case WM_SYSCOMMAND:
      if ((static_cast<UINT>(w) & 0xFFF0) == SC_MINIMIZE) {
        // a TOPMOST window minimizes into a floating caption ABOVE the taskbar; drop the
        // privileged level first so it lands in the taskbar like any normal window
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        LogMsg("[set] minimize -> taskbar (topmost dropped)");
      }
      break;                     // DefWindowProc performs the actual minimize
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      if (g_setController) { g_setController->Release(); g_setController = nullptr; }
      if (g_setWebview) { g_setWebview->Release(); g_setWebview = nullptr; }
      g_setHwnd = nullptr;
      g_setCreating = false;
      g_setPageReady = false;
      LogMsg("[set] settings window destroyed");
      return 0;
    default:
      break;
  }
  return DefWindowProcW(h, m, w, l);
}

class SetMsgHandler : public Cb<ICoreWebView2WebMessageReceivedEventHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*,
                                   ICoreWebView2WebMessageReceivedEventArgs* e) override {
    LPWSTR raw = nullptr;
    if (FAILED(e->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
    std::string s = WideToUtf8(raw);
    CoTaskMemFree(raw);
    if (s.find("\"ready\"") != std::string::npos) {
      g_setPageReady = true;
      RevealSetPage();
      // engine handshake for the settings page too: the threads/hash selects need the
      // real option lists (threadsMax / RAM-aware hash cap)
      if (g_setWebview) {
        g_setWebview->PostWebMessageAsJson(Utf8ToWide(HostHandshakeJson()).c_str());
        g_setWebview->PostWebMessageAsJson(Utf8ToWide(SysInfoJson()).c_str());
      }
      return S_OK;
    }
    if (s.find("\"settingsClose\"") != std::string::npos) {
      if (g_setHwnd) PostMessageW(g_setHwnd, WM_CLOSE, 0, 0);
      return S_OK;
    }
    if (s.find("\"settingsPatch\"") != std::string::npos) {
      // the settings page pushes its patch; relay it to EVERY window (settings are master:
      // main, link, review and vision all follow immediately)
      std::string patch = JsonStrAfterDecoded(s, "\"patch\":");
      if (!patch.empty()) {
        std::string msg = "{\"type\":\"setSync\",\"patch\":" + patch + "}";
        PostToPageSoon(msg);
        PostToLinkPageSoon(msg);
        PostToRvPageSoon(msg);
        PostToVisPageSoon(msg);
      }
      return S_OK;
    }
    if (s.find("\"setTitle\"") != std::string::npos) {
      std::string tx = JsonStrAfterDecoded(s, "\"txt\":");
      if (!tx.empty() && g_setHwnd) SetWindowTextW(g_setHwnd, Utf8ToWide(tx).c_str());
      return S_OK;
    }
    if (s.find("uiTheme") != std::string::npos) {
      std::string th = JsonStrAfterDecoded(s, JKey("theme").c_str());
      if (!th.empty() && g_setHwnd) ApplyDarkTitleBar(g_setHwnd, th != "light");
      return S_OK;
    }
    return S_OK;
  }
};

class SetCtlHandler : public Cb<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* ctl) override {
    g_setCreating = false;
    if (FAILED(hr) || !ctl || !g_setHwnd) {
      LogMsg("[set] settings controller creation failed");
      return S_OK;
    }
    g_setController = ctl;
    ctl->AddRef();
    ctl->get_CoreWebView2(&g_setWebview);
    if (!g_setWebview) return S_OK;
    g_setWebview->AddRef();
    HardenWebviewSettings(g_setWebview, "set");
    SetupUiServing(g_setWebview, "set");
    EventRegistrationToken tok = {};
    g_setWebview->add_WebMessageReceived(new SetMsgHandler(), &tok);
    g_setWebview->Navigate(UiBaseUrl("set").c_str());
    SetWebviewBg(ctl, g_uiBg);
    ctl->put_IsVisible(TRUE);
    LayoutSetWebView();
    // the client rect can still be empty this early: force one deferred layout pass
    PostMessageW(g_setHwnd, WM_SIZE, 0, 0);
    LogMsg("[set] navigated to calc.html?set=1");
    return S_OK;
  }
};

static void CreateSetWindow() {
  if (g_setHwnd || g_setCreating || !g_env) return;
  static HBRUSH setBgBrush = nullptr;                 // theme-coloured splash/backdrop (no white flash)
  if (!setBgBrush) setBgBrush = CreateSolidBrush(g_uiBg);
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = SetWndProc;
  wc.hInstance = g_hInst;
  wc.lpszClassName = kSetCls;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = setBgBrush;
  wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
  wc.hIconSm = LoadIconW(g_hInst, MAKEINTRESOURCEW(1));
  RegisterClassExW(&wc);

  RECT d;
  SetWinDesiredRect(&d);
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
  // TOPMOST: the settings window is the master panel - it stays above every other window
  g_setHwnd = CreateWindowExW(WS_EX_TOPMOST, kSetCls, kSetTitle, style,
                              d.left, d.top, d.right - d.left, d.bottom - d.top,
                              g_hwnd, nullptr, g_hInst, nullptr);
  if (!g_setHwnd) { LogMsg("[set] CreateWindowEx failed"); return; }
  g_setCreating = true;
  g_setPageReady = false;
  ApplyDarkTitleBar(g_setHwnd, g_uiDark);
  LogMsg("[set] window created " + std::to_string(d.right - d.left) + "x" +
         std::to_string(d.bottom - d.top));
  g_env->CreateCoreWebView2Controller(g_setHwnd, new SetCtlHandler());
}

static void OpenSetWindow() {
  if (g_setHwnd && !IsWindow(g_setHwnd)) {   // stale handle: prewarmed window died, rebuild
    g_setHwnd = nullptr; g_setController = nullptr; g_setWebview = nullptr; g_setCreating = false;
  }
  if (g_setHwnd) {
    if (IsIconic(g_setHwnd)) ShowWindow(g_setHwnd, SW_RESTORE);
    ShowWindow(g_setHwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_setHwnd);
    LayoutSetWebView();
    return;
  }
  if (g_setCreating || !g_env) {
    LogMsg("[set] open queued (window or webview2 env not ready)");
    return;
  }
  CreateSetWindow();
  if (g_setHwnd) {
    ShowWindow(g_setHwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_setHwnd);
  }
}
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case kMsgPrewarmRv:                        
      PrewarmReviewWindow();
      return 0;
    case kMsgVisShot:                          
      StartVisShotFlow();
      return 0;
    case kMsgLinkSelect:                       
      
      PostMessageW(h, kMsgLinkFrameOpen, 0, 0);
      return 0;
    case kMsgLinkFrameOpen:                       
      StartLinkFrame();
      return 0;
    case WM_ERASEBKGND:
      
      return 1;
    case WM_PAINT:
      
      PaintSplash(h, g_mainPageShown, kBrandName);
      return 0;
    case WM_SIZE:
      LayoutWebView();
      return 0;
    case WM_MOVE:
      // the settings window is independent: it never follows the main window
      return 0;
    case kMsgFlushOutbox:
      
      {
        std::vector<std::string> pg, rv, vs, lk;
        OutboxDrain(g_outboxPage, pg);
        OutboxDrain(g_outboxRv, rv);
        OutboxDrain(g_outboxVis, vs);
        OutboxDrain(g_outboxLink, lk);
        for (size_t i = 0; i < pg.size(); ++i) PostToPage(pg[i]);
        for (size_t i = 0; i < rv.size(); ++i) PostToRvPage(rv[i]);
        for (size_t i = 0; i < vs.size(); ++i) PostToVisPage(vs[i]);
        for (size_t i = 0; i < lk.size(); ++i) PostToLinkPage(lk[i]);
      }
      return 0;
    case WM_TIMER:
      
      if (w == kMainTimerReload) {
        KillTimer(h, kMainTimerReload);
        if (g_mainPageShown) return 0;
        // ★ 2026-10-16：自愈之前先取证 —— 每次看到沉默都记一条「渲染进程到底活没活」。
        //   这是把「约五成随机沉默」变成可定位问题的关键一步（以前只有一句 still silent）。
        if (g_webview) g_webview->ExecuteScript(Utf8ToWide(kSilentProbeJs).c_str(), new MainProbeHandler());
        if (g_mainReloads < 2 && g_webview) {
          g_mainReloads++;
          LogMsg("[ui] main page still silent - re-navigating (self heal #" +
                 std::to_string(g_mainReloads) + "/2)");
          g_webview->Navigate(MainNavUrl(UiBaseUrl(nullptr).c_str()).c_str());
          SetTimer(h, kMainTimerReload, 8000, nullptr);
          return 0;
        }
        // ★ 2026-10-16：重导航两次仍沉默 ⇒ 换一个渲染进程（重建 WebView2 控制器）。
        //   实测「页面沉默」时渲染进程是可以完全不工作的（探针无回话），此时重导航
        //   救不回来，只有重建控制器能拿到新的渲染进程。以前走到这里直接
        //   RevealMainPage() ⇒ 用户看到永久白屏（丢进待办的那个产品级风险）。
        if (g_mainCtrlRebuilds < 2 && g_env && g_hwnd) {
          g_mainCtrlRebuilds++;
          LogMsg("[ui] 页面始终无 ready ⇒ 重建 WebView2 控制器（换渲染进程）#" +
                 std::to_string(g_mainCtrlRebuilds) + "/2");
          if (g_webview) { g_webview->Release(); g_webview = nullptr; }
          if (g_controller) { g_controller->Release(); g_controller = nullptr; }
          g_mainReloads = 0;
          g_env->CreateCoreWebView2Controller(g_hwnd, new CtlHandler());
          SetTimer(h, kMainTimerReload, 10000, nullptr);
          return 0;
        }
        LogMsg("[ui] main page never reported ready - revealing the webview anyway");
        RevealMainPage();
        return 0;
      }
      
      if (w == kMainTimerOutbox) {
        KillTimer(h, kMainTimerOutbox);
        std::vector<std::string> pg, rv, lk;
        OutboxDrain(g_outboxPage, pg);
        OutboxDrain(g_outboxRv, rv);
        OutboxDrain(g_outboxLink, lk);
        for (size_t i = 0; i < pg.size(); ++i) PostToPage(pg[i]);
        for (size_t i = 0; i < rv.size(); ++i) PostToRvPage(rv[i]);
        for (size_t i = 0; i < lk.size(); ++i) PostToLinkPage(lk[i]);
        return 0;
      }
      
      if (w == kMainTimerHook) {
        KillTimer(h, kMainTimerHook);
        FireTestHooks();
        return 0;
      }

      if (w == kMainTimerTestSetMin) {
        KillTimer(h, kMainTimerTestSetMin);
        if (g_setHwnd && IsWindow(g_setHwnd)) {
          PostMessageW(g_setHwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
          LogMsg("[set] test hook: SC_MINIMIZE posted");
        } else {
          LogMsg("[set] test hook: settings window is not there yet");
        }
        return 0;
      }
      
      if (w == kMainTimerEval) {
        KillTimer(h, kMainTimerEval);
        if (g_webview) {
          g_webview->ExecuteScript(Utf8ToWide(
            "(function(){"
            "var b=document.getElementById('boot');"
            "try{return JSON.stringify({"
            "url:location.href,"
            "rs:document.readyState,"
            "title:document.title.slice(0,20),"
            "bootInfo:(typeof window.__bootInfo!=='undefined')?JSON.stringify(window.__bootInfo):null,"
            "gbTop:!!window.__gbTop,"
            "bootElGone:!b,"
            "hasLocalAI:typeof LocalAI,"
            "moves:(typeof G!=='undefined'&&G.moves)?G.moves.length:-1,"
            "readySent:!!window.__gbReadySent"
            "});}catch(err){return 'EVALERR:'+err.message;}"
            "})()").c_str(), new EvalHandler());
        }
        return 0;
      }
      
      if (w == kRvTimerPrewarm) {
        KillTimer(h, kRvTimerPrewarm);
        PrewarmReviewWindow();
        return 0;
      }

      if (w == kMainTimerPrewarmSet) {
        KillTimer(h, kMainTimerPrewarmSet);
        // pre-create the settings window (hidden) so the first open is instant
        if (!g_setHwnd && !g_setCreating) { CreateSetWindow(); LogMsg("[set] prewarmed at boot"); }
        return 0;
      }
      
      if (w == kRvTimerTestOpen) {
        KillTimer(h, kRvTimerTestOpen);
        PostToPage("{\"type\":\"testOpenReview\"}");
        LogMsg("[rv] test hook (delayed): asked the page to open a review window");
        return 0;
      }
      
      if (w == kMainTimerSysMem) {
        PostToPage(SysMemJson());
        return 0;
      }

      // ★ 2026-10-16：连线链路 E2E 测试钩子的两段定时器。
      if (w == kMainTimerTestLink) {
        KillTimer(h, kMainTimerTestLink);
        LogMsg("[test] GB_TEST_OPEN_LINK: opening link window");
        OpenLinkWindow();
        SetTimer(h, kMainTimerTestLinkScan, 4000, nullptr);
        return 0;
      }
      if (w == kMainTimerTestLinkScan) {
        KillTimer(h, kMainTimerTestLinkScan);
        if (!g_linkPageReady) {
          SetTimer(h, kMainTimerTestLinkScan, 2000, nullptr);
          return 0;
        }
        // 我执黑 + 合成 4B4W 扫描帧（黑先、无五连）→ 触发真实 analyze 链路。
        // setSide 是切换语义：若上次会话已持久化 side=0，发一次反而取消 —— 连发两次保证终态=黑。
        PostToLinkPageSoon("{\"type\":\"setSide\",\"v\":0}");
        PostToLinkPageSoon("{\"type\":\"setSide\",\"v\":0}");
        std::string fake =
          "{\"type\":\"scan\",\"data\":{\"ok\":true,\"found\":true,\"size\":15,"
          "\"geometry\":{\"x_lines\":[100,140,180,220,260,300,340,380,420,460,500,540,580,620,660],"
          "\"y_lines\":[120,160,200,240,280,320,360,400,440,480,520,560,600,640,680],\"spacing\":40,\"size\":15},"
          "\"black\":[{\"x\":7,\"y\":7},{\"x\":8,\"y\":8},{\"x\":9,\"y\":9},{\"x\":7,\"y\":9}],"
          "\"white\":[{\"x\":7,\"y\":8},{\"x\":8,\"y\":9},{\"x\":9,\"y\":7},{\"x\":10,\"y\":10}]}}";
        PostToLinkPageSoon(fake);
        LogMsg("[test] link fake scan posted (4B4W, side=black)");
        // ★ 2026-10-17：连线窗持久化回归钩子 —— 连线窗的卡片固定/布局只能在这里验证。
        {
          char lev[2048] = {0};
          if (GetEnvironmentVariableA("GB_TEST_EVAL_LINK", lev, sizeof(lev)) && lev[0] && g_linkWebview) {
            g_linkWebview->ExecuteScript(Utf8ToWide(lev).c_str(), nullptr);
            LogMsg("[test] GB_TEST_EVAL_LINK injected (" + std::to_string(strlen(lev)) + " chars)");
          }
        }
        // ★ 2026-10-16 回归探针（GB_TEST_LINK_PROBE=1）：
        //   ① 6s 后投空盘帧（连投数帧以过 STABLE_FRAMES/emptyStreak 门槛）
        //      → 期望日志 LINK_TT_RESET reason=empty-board
        //   ② 16s 后向**主页面**投 linkCfgSync（模拟主界面改规则）
        //      → 期望宿主转发 + 连线页 LINK_TT_RESET reason=rule-change ...
        //   这两条正是用户报的「空盘要清置换表」「变规则不能顺利切换并评估」。
        char pv2[8] = {0};
        if (GetEnvironmentVariableA("GB_TEST_LINK_PROBE", pv2, sizeof(pv2)) && pv2[0] == '1') {
          // ★ 2026-10-17 次序修正：**先**在盘面仍非空（4B4W）时切规则（→ 必须立即按新规则
          //   重算一次，这才是用户说的「变规则不能顺利切换并评估」），**后**再投空盘帧
          //   （→ 清置换表）。原先把空盘放在前面，等到切规则时盘面已空，
          //   kickAnalyze 在空盘下什么都不做 ⇒ 压根证明不了「换规则会重算」。
          SetTimer(h, kMainTimerTestLinkProbe2, 6000, nullptr);
          SetTimer(h, kMainTimerTestLinkProbe, 16000, nullptr);
          LogMsg("[test] link probe armed (rule-sync @6s on non-empty board, empty-board @16s)");
        }
        SetTimer(h, kMainTimerTestLinkCheck, 4000, nullptr);
        return 0;
      }
      if (w == kMainTimerTestLinkProbe) {
        // 空盘帧连投 5 次（每次 400ms）：handleScan 的 busy 守卫会丢帧，
        // emptyStreak 也要 ≥2 才进空盘分支。
        static int s_emptyPost = 0;
        std::string empt =
          "{\"type\":\"scan\",\"data\":{\"ok\":true,\"found\":true,\"size\":15,"
          "\"geometry\":{\"x_lines\":[100,140,180,220,260,300,340,380,420,460,500,540,580,620,660],"
          "\"y_lines\":[120,160,200,240,280,320,360,400,440,480,520,560,600,640,680],\"spacing\":40,\"size\":15},"
          "\"black\":[],\"white\":[]}}";
        PostToLinkPageSoon(empt);
        if (++s_emptyPost < 5) {
          SetTimer(h, kMainTimerTestLinkProbe, 400, nullptr);
        } else {
          KillTimer(h, kMainTimerTestLinkProbe);
          s_emptyPost = 0;
          LogMsg("[test] link empty-board frames done (probe 1)");
        }
        return 0;
      }
      if (w == kMainTimerTestLinkProbe2) {
        KillTimer(h, kMainTimerTestLinkProbe2);
        // ★ 2026-10-17 修正：主界面改规则的**真实**路径是「页面主动发给宿主」
        //   {type:'linkCfgSync',rule:N}（calc.js sel_rule.onchange → tellHost），
        //   再由宿主 MsgHandler 转发给连线页。原来这里用 PostToPageSoon 把消息**投给
        //   主页面**，可主页面根本没注册 onLinkHostMsg（那在 bootLink 里）⇒ 等于空投，
        //   宿主的转发分支从没被走到，回归探针形同虚设。
        //   现在改为往主页面**注入脚本**，复现页面真实的发送动作（真下拉 + 直发消息）。
        if (g_webview) {
          // ★ 2026-10-17：**取反**当前规则再切 —— 上一轮探针会把规则持久化进全局键
          //   （gbcalc.rule.v1），下一轮连线页开局就已经是新规则 ⇒ 写死 rule=1 会变成
          //   「没有变化」的空操作，探针静默失效（实测踩过）。
          const char* inj =
            "(function(){try{"
            "var e=document.getElementById('sel_rule');"
            "var cur=e?parseInt(e.value,10):0;if(!(cur===0||cur===1||cur===2))cur=0;"
            "var nxt=(cur===1)?0:1;"
            "var msg='probe2 rule '+cur+'->'+nxt;"
            "if(e){e.value=String(nxt);e.dispatchEvent(new Event('change'));}"
            "window.chrome.webview.postMessage({type:'linkCfgSync',rule:nxt});"
            "try{window.chrome.webview.postMessage({type:'log',text:msg})}catch(e3){msg=msg+' (log post failed)'}"
            "}catch(err){try{window.chrome.webview.postMessage({type:'log',text:'probe2 inject err '+err.message})}catch(e2){}}"
            "})()";
          g_webview->ExecuteScript(Utf8ToWide(inj).c_str(), nullptr);
          LogMsg("[test] linkCfgSync (rule toggle) sent FROM main page (probe 2)");
        } else {
          LogMsg("[test] probe 2 skipped: g_webview null");
        }
        return 0;
      }
      if (w == kMainTimerTestLinkCheck) {
        // ★ 2026-10-16：周期探查（4s x6）——覆盖候选快速交付（~1-2s）与各兜底路径。
        static int s_linkCheckCount = 0;
        if (g_linkWebview) {
          g_linkWebview->ExecuteScript(Utf8ToWide(
            "(function(){"
            "function t(id){var e=document.getElementById(id);return e?e.textContent:'';}"
            "var out={status:t('lkStatus').slice(0,120),"
            "eval:t('lk_s_eval').slice(0,40),"
            "depth:t('lk_s_depth').slice(0,20),"
            "speed:t('lk_s_speed').slice(0,40),"
            "line:t('lk_s_nodes').slice(0,60)};"
            "return JSON.stringify(out);"
            "})()").c_str(), new EvalHandler());
          LogMsg("[test] link state check #" + std::to_string(s_linkCheckCount + 1) + " requested");
        }
        if (++s_linkCheckCount >= 6) { KillTimer(h, kMainTimerTestLinkCheck); s_linkCheckCount = 0; }
        return 0;
      }
      break;
    case WM_GETMINMAXINFO: {
      MINMAXINFO* mm = (MINMAXINFO*)l;
      
      int mw = GbPx(h, kMainMinCssW), mh = GbPx(h, kMainMinCssH);
      GbClampToWorkArea(h, mw, mh);
      mm->ptMinTrackSize.x = mw;
      mm->ptMinTrackSize.y = mh;
      return 0;
    }
    case WM_COPYDATA: {
      
      COPYDATASTRUCT* cd = (COPYDATASTRUCT*)l;
      if (cd && cd->lpData && cd->cbData > 0) {
        std::string body((const char*)cd->lpData, (size_t)cd->cbData);
        std::string msg = "{\"type\":\"external\",\"payload\":" + JsonQuote(body) + "}";
        PostToPage(msg);
        LogMsg("[ipc] WM_COPYDATA " + std::to_string(body.size()) + " bytes -> page");
      }
      SetForegroundWindow(h);
      return TRUE;
    }
    case WM_CLOSE:
      SaveWinPos();
      DestroyWindow(h);
      return 0;
    
    case WM_QUERYENDSESSION:
      return TRUE;
    case WM_ENDSESSION:
      if (w) { SaveWinPos(); DestroyWindow(h); }
      return 0;
    case WM_DESTROY:
      SaveWinPos();
      g_running.store(false);
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(h, m, w, l);
}

static std::wstring WebView2UserDataDir() {
  wchar_t exePath[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  std::wstring oldDir = std::wstring(exePath) + L".WebView2";   
  std::wstring nu = Join(ExeDir(), L"GomokuTrainer resources");
  DWORD attr = GetFileAttributesW(oldDir.c_str());
  if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
    if (GetFileAttributesW(nu.c_str()) == INVALID_FILE_ATTRIBUTES) {
      if (MoveFileW(oldDir.c_str(), nu.c_str()))
        LogMsg("[boot] migrated WebView2 user data dir -> GomokuTrainer resources");
      else
        LogMsg("[boot] old WebView2 dir in use, migration skipped (will retry next start)");
    }
  }
  return nu;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
  g_bootT0 = GetTickCount();                 
  g_hInst = hInst;
  
  GbSetDpiAwareness();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  
  LogCpuSplitOnce();
  EnsureJob();   // Job（KILL_ON_JOB_CLOSE）：宿主退出即回收引擎子进程
  eng::Init();   // 探测变体 + 读回用户线程/哈希偏好（不 spawn，首个 ask 惰性启动）
                            
  std::string argPayload;
  {
    int nArgs = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    if (argv) {
      for (int i = 1; i < nArgs; ++i) {
        std::wstring a(argv[i]);
        if (a.rfind(L"--board=", 0) == 0) argPayload = WideToUtf8(a.substr(8));
      }
      LocalFree(argv);
    }
  }
  HANDLE mx = CreateMutexW(nullptr, FALSE, L"Global\\GomokuTrainer_v1");
  bool already = (mx && GetLastError() == ERROR_ALREADY_EXISTS);
  if (already) {
    HWND prev = FindWindowW(kCls, nullptr);
    if (prev) {
      if (!argPayload.empty()) {
        COPYDATASTRUCT cd = {};
        cd.cbData = (DWORD)argPayload.size();
        cd.lpData = (void*)argPayload.data();
        SendMessageW(prev, WM_COPYDATA, (WPARAM)0, (LPARAM)&cd);
      }
      if (IsIconic(prev)) ShowWindow(prev, SW_RESTORE);
      SetForegroundWindow(prev);
    }
    if (mx) CloseHandle(mx);
    return 0;
  }

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.lpszClassName = kCls;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));         
  wc.hIconSm = LoadIconW(hInst, MAKEINTRESOURCEW(1));
  RegisterClassExW(&wc);

  int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
  int w = GbPx(nullptr, kMainDefCssW), h = GbPx(nullptr, kMainDefCssH);
  GbClampToWorkArea(nullptr, w, h);
  bool maximized = false;
  {
    FILE* f = _wfopen(WinPosFile().c_str(), L"rb");
    if (f) {
      WINDOWPLACEMENT wp = {};
      if (fread(&wp, 1, sizeof(wp), f) == sizeof(wp) && wp.length == sizeof(wp)) {
        RECT r = wp.rcNormalPosition;
        x = r.left; y = r.top;
        w = r.right - r.left; h = r.bottom - r.top;
        maximized = (wp.showCmd == SW_SHOWMAXIMIZED);
      }
      fclose(f);
    }
  }
  
  {
    int mw = GbPx(nullptr, kMainMinCssW), mh = GbPx(nullptr, kMainMinCssH);
    GbClampToWorkArea(nullptr, mw, mh);
    if (w < mw) w = mw;
    if (h < mh) h = mh;
  }
  
  LoadUiPrefs();
  // Load persisted link "识别参数" before the link page can be opened, so the overlay and the
  // scan loop start from the user's real choice.
  LoadLinkRecogPrefs();
  LoadVisRecogPrefs();      // 识图「识别参数」：宿主落盘那份启动读回（单一权威）
  g_hwnd = CreateWindowExW(0, kCls, kTitle,
                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           x, y, w, h, nullptr, nullptr, hInst, nullptr);
  if (!g_hwnd) { LogMsg("[boot] CreateWindowEx failed"); return 1; }
  ApplyDarkTitleBar(g_hwnd, g_uiDark);
  ShowWindow(g_hwnd, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
  
  PrewarmVisionServiceAsync();
  UpdateWindow(g_hwnd);          
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x11
#endif
  
  LogBoot("window shown");
  
  SetTimer(g_hwnd, kMainTimerReload, 10000, nullptr);  
  {
    char dv[8] = {0};
    if (GetEnvironmentVariableA("GB_TEST_DIAG", dv, sizeof(dv)) && dv[0] == '1')
      SetTimer(g_hwnd, kMainTimerEval, 25000, nullptr);  
  }

  LogMsg("[boot] GomokuTrainer starting (exe dir " + WideToUtf8(ExeDir()) + ")");
  
  std::wstring wv2udf = WebView2UserDataDir();
  
  ClearWebViewCache(wv2udf, "启动");
  // ★ 2026-10-16：不再传 nullptr（= Chromium 桌面默认，节流最狠的一组）。
  //   直接带上「不节流 + GPU 光栅」的 browser args；万一这套 args 让环境创建失败
  //   （驱动/企业策略等），自动退回默认口径，保证 UI 一定能起来。
  HRESULT hr = E_FAIL;
  {
    EnvOptions* opt = new EnvOptions(GbWv2BrowserArgs());
    LogMsg("[wv2] env create with args: " + WideToUtf8(opt->args()));
    hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, wv2udf.c_str(), opt, new EnvHandler());
    if (FAILED(hr)) {
      char b[24] = {0};
      sprintf_s(b, "0x%08X", (unsigned)hr);
      LogMsg(std::string("[wv2] env create with args FAILED hr=") + b + " -> retry with defaults");
      hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, wv2udf.c_str(), nullptr, new EnvHandler());
    }
  }
  LogBoot("wv2 env requested");

  std::thread([] {
    int delays[] = { 2500, 7000, 15000 };
    for (int i = 0; i < 3; i++) {
      Sleep(delays[i]);
      if (!g_running.load()) return;
      BoostWebViewPriority();
    }
  }).detach();
  std::thread(HttpHistoryServer).detach();
  std::thread(InboxFileWatcher).detach();
#ifdef GB_SUITE_ENGINE
  
  std::thread(EngineWatchdog).detach();
#endif
  if (!argPayload.empty()) {
    std::string msg = "{\"type\":\"external\",\"payload\":" + JsonQuote(argPayload) + "}";
    g_pending = msg;
  }

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  g_running.store(false);
  eng::Shutdown();
  CloseJobHandle();
  
  KillOurChildProcesses("正常关闭");
  
  ClearWebViewCache(WebView2UserDataDir(), "退出");
  
  std::thread([] {
    Sleep(1500);
    if (UpdDirSize(WebView2UserDataDir()) >= kUpdCleanLimit) UpdSweepCacheDirs();
  }).detach();
  if (mx) CloseHandle(mx);
  CoUninitialize();
  return 0;
}
