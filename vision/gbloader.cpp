// Meter Gomoku Link — v4.43
/* Vision - boardloader. */


#include "gbloader.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

using namespace gb;

namespace {

typedef int (*GbvScanFn)(const char* req, int reqLen, char* out, int cap);
typedef int (*GbvScanImageFn)(const char* img, int imgLen, int probe, char* out, int cap);
typedef int (*GbvRecognizeJsonFn)(const char* req, int reqLen, char* out, int cap);

HMODULE g_dll = nullptr;
GbvScanFn g_scan = nullptr;
GbvScanImageFn g_scanImage = nullptr;
GbvRecognizeJsonFn g_recognizeJson = nullptr;
std::mutex g_mu;

bool deliver(int n, std::vector<char>& buf, std::string& resp) {
    if (n < 0) return false;
    resp.assign(buf.data(), buf.data() + n);
    return true;
}

}  

namespace gb {

bool LoadModelDll(bool quietLog) {
    static bool done = false;
    static bool ok = false;
    if (done) return ok;
    done = true;

    wchar_t exe[MAX_PATH * 2] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    std::wstring dir(exe);
    size_t p = dir.find_last_of(L"\\/");
    dir = (p == std::wstring::npos) ? L"." : dir.substr(0, p);
    std::wstring path = dir + L"\\GomokuVision.dll";

    HMODULE h = LoadLibraryW(path.c_str());
    if (!h) {
        if (!quietLog)
            fprintf(stderr, "[vision] GomokuVision.dll load FAILED (err %lu): %ls\n",
                    GetLastError(), path.c_str());
        return false;
    }
    g_scan = (GbvScanFn)GetProcAddress(h, "GbvScan");
    g_scanImage = (GbvScanImageFn)GetProcAddress(h, "GbvScanImage");
    g_recognizeJson = (GbvRecognizeJsonFn)GetProcAddress(h, "GbvRecognizeJson");
    if (!g_scan || !g_scanImage || !g_recognizeJson) {
        if (!quietLog)
            fprintf(stderr,
                    "[vision] GomokuVision.dll missing exports (stale pair?) - rebuild both\n");
        g_scan = nullptr;
        g_scanImage = nullptr;
        g_recognizeJson = nullptr;
        FreeLibrary(h);
        return false;
    }
    g_dll = h;
    ok = true;
    if (!quietLog) fprintf(stderr, "[vision] model dll ready: %ls\n", path.c_str());
    return true;
}

bool ModelDllReady() { return g_scan != nullptr; }

bool ModelScan(const std::string& reqBody, std::string& resp) {
    if (!g_scan) return false;
    static std::vector<char> buf(1 << 22);   
    std::lock_guard<std::mutex> lk(g_mu);
    int n = g_scan(reqBody.data(), (int)reqBody.size(), buf.data(), (int)buf.size());
    return deliver(n, buf, resp);
}

bool ModelScanImage(const unsigned char* img, int len, bool probe, std::string& resp) {
    if (!g_scanImage) return false;
    static std::vector<char> buf(1 << 22);
    std::lock_guard<std::mutex> lk(g_mu);
    int n = g_scanImage((const char*)img, len, probe ? 1 : 0, buf.data(), (int)buf.size());
    return deliver(n, buf, resp);
}

bool ModelRecognizeJson(const std::string& reqBody, std::string& resp) {
    if (!g_recognizeJson) return false;
    static std::vector<char> buf(1 << 22);
    std::lock_guard<std::mutex> lk(g_mu);
    int n = g_recognizeJson(reqBody.data(), (int)reqBody.size(), buf.data(), (int)buf.size());
    return deliver(n, buf, resp);
}

}  
