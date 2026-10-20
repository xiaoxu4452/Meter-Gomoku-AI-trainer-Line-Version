/* Vision - main. */



#include "gb.h"
#include "gbhttp.h"
#include "gbloader.h"
#include "gbpipe.h"

#include <windows.h>
#include <intrin.h>   
#include <immintrin.h> 

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

using namespace gb;

static ULONGLONG g_lastTrafficAt = GetTickCount64();
static DWORD g_parentPid = 0;

void gb::TouchTraffic() { g_lastTrafficAt = GetTickCount64(); }

namespace {

constexpr int DEFAULT_RECOGNIZE_PORT = 8970;
constexpr int DEFAULT_SCAN_PORT = 8971;

HANDLE gMutex = nullptr;
std::atomic<bool> gStopping{false};

bool HttpEnabledByEnv(bool explicitSingleRole) {
    char v[16] = {0};
    if (GetEnvironmentVariableA("GB_VISION_HTTP", v, sizeof(v)) && v[0]) {
        if (v[0] == '0') return false;                   // "0" / "0xxx"
        if (v[0] == 'f' || v[0] == 'F') return false;    // false / FALSE
        if (v[0] == 'n' || v[0] == 'N') return false;    // no / none
        return true;                                     // "1" / "true" / "yes" ...
    }
    return explicitSingleRole;
}

std::vector<std::pair<std::string, std::string>> corsHeaders(bool privateNetwork) {
    std::vector<std::pair<std::string, std::string>> h;
    h.emplace_back("Access-Control-Allow-Origin", "*");
    h.emplace_back("Access-Control-Allow-Headers", "Content-Type");
    h.emplace_back("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    if (privateNetwork) h.emplace_back("Access-Control-Allow-Private-Network", "true");
    return h;
}

HttpResponse jsonResponse(const Json& j) {
    HttpResponse r;
    r.status = 200;
    r.contentType = "application/json";
    r.body = j.dump();
    return r;
}

HttpResponse noContent(bool privateNetwork) {
    HttpResponse r;
    r.status = 204;
    r.body.clear();
    r.extraHeaders = corsHeaders(privateNetwork);
    return r;
}

HttpResponse notFound(const std::string& err) {
    Json j = Json::makeObj();
    j.set("ok", Json::makeBool(false));
    j.set("err", Json::makeStr(err));
    HttpResponse r = jsonResponse(j);
    r.status = 404;
    return r;
}

HttpResponse modelUnavailable() {
    Json j = Json::makeObj();
    j.set("ok", Json::makeBool(false));
    j.set("err", Json::makeStr("model dll not loaded (GomokuVision.dll missing or stale pair)"));
    return jsonResponse(j);
}

int recognizePort() {
    const char* e = getenv("GB_RECOG_PORT");
    if (e && *e) return atoi(e);
    return DEFAULT_RECOGNIZE_PORT;
}

HttpResponse handleRecognize(const HttpRequest& req) {
    gb::TouchTraffic();

    if (req.method == "OPTIONS") return noContent(true);
    if (req.method == "GET" && req.path == "/health") {
        bool ready = ModelDllReady();
        Json j = Json::makeObj();
        j.set("ok", Json::makeBool(ready));
        HttpResponse r = jsonResponse(j);
        if (!ready) r.status = 503;
        return r;
    }
    if (req.method == "POST" && req.path == "/recognize") {
        std::string resp;
        if (!ModelRecognizeJson(req.body, resp)) return modelUnavailable();
        HttpResponse r;
        r.status = 200;
        r.contentType = "application/json";
        r.body = resp;
        r.extraHeaders = corsHeaders(true);
        return r;
    }
    return notFound("not found");
}

int scanPort() {
    const char* e = getenv("GB_SCAN_PORT");
    if (e && *e) return atoi(e);
    return DEFAULT_SCAN_PORT;
}

HttpResponse handleScan(const HttpRequest& req) {
    gb::TouchTraffic();
    if (req.method == "OPTIONS") return noContent(true);
    if (req.method == "GET" && req.path == "/health") {
        bool ready = ModelDllReady();
        Json j = Json::makeObj();
        j.set("ok", Json::makeBool(ready));
        j.set("port", Json::makeInt(scanPort()));
        HttpResponse r = jsonResponse(j);
        if (!ready) r.status = 503;
        return r;
    }
    if (req.method == "POST" && req.path == "/quit") {
        HttpResponse r = jsonResponse([]{ Json j = Json::makeObj();
                                          j.set("ok", Json::makeBool(true));
                                          return j; }());
        r.extraHeaders = corsHeaders(true);
        gStopping.store(true);
        std::thread([] {
            Sleep(200);
            if (gMutex) ReleaseMutex(gMutex);
            ExitProcess(0);
        }).detach();
        return r;
    }
    if (req.method == "POST" && req.path == "/scan-image") {
        std::string resp;
        if (!ModelScanImage((const unsigned char*)req.body.data(),
                            (int)req.body.size(), false, resp)) return modelUnavailable();
        HttpResponse r;
        r.status = 200;
        r.contentType = "application/json";
        r.body = resp;
        r.extraHeaders = corsHeaders(true);
        return r;
    }
    if (req.method != "POST" || req.path != "/scan") return notFound("not found");

    std::string resp;
    if (!ModelScan(req.body, resp)) return modelUnavailable();
    HttpResponse r;
    r.status = 200;
    r.contentType = "application/json";
    r.body = resp;
    r.extraHeaders = corsHeaders(true);
    return r;
}

std::vector<unsigned char> fileBytes(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
}

}  

int main(int argc, char** argv) {
    setDpiAware();

    std::string role = "both";
    std::string scanImage;
    std::string recognizeImage;
    bool probeImage = false;
    bool nosnap = false;      
    bool cpuidDump = false;   
    std::string algoSync;     
    std::string recogCsv;     
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--scan") role = "scan";
        else if (a == "--recognize") role = "recognize";
        else if (a == "--both") role = "both";
        else if (a == "--probe") probeImage = true;
        else if (a == "--nosnap") nosnap = true;
        else if (a == "--cpuid") cpuidDump = true;
        else if (a == "--scan-image" && i + 1 < argc) scanImage = argv[++i];
        else if (a == "--recognize-image" && i + 1 < argc) recognizeImage = argv[++i];
        else if (a == "--algo" && i + 1 < argc) algoSync = argv[++i];
        else if (a == "--recog" && i + 1 < argc) recogCsv = argv[++i];
        else if (a == "--help" || a == "-h") {
            printf("GomokuVision —— 五子棋棋盘识别服务（识图服务壳 + 识别模型库 DLL）\n");
            printf("  通道：**命名管道 + 共享内存**（默认；本进程一个 TCP 端口都不开）\n");
            printf("  --recognize            只起识别管道 \\\\.\\pipe\\GomokuVision.recognize.<port>\n");
            printf("  --scan                 只起扫描管道 \\\\.\\pipe\\GomokuVision.scan.<port>\n");
            printf("  （不带参数 = 两条管道都起）\n");
            printf("  env GB_VISION_HTTP=1   ★ 仅老产品需要：额外在 :8970/:8971 上开 TCP\n");
            printf("                        （我们这一版无参数启动 ⇒ 零 TCP，纯管道）\n");
            printf("  --recognize-image 文件  离线跑一次 /recognize 并打印 JSON（开发期对拍用）\n");
            printf("  --scan-image 文件       离线跑一次 /scan-image 并打印 JSON（可配 --probe）\n");
            printf("  --nosnap                跳过自动吸附棋盘的重试（HTTP 请求体 nosnap:true 同效）\n");
            printf("  --algo sync --recog 5项  识别走同步读子（灰度偏差,黑偏差,白偏差,黑占比,白占比）\n");
            printf("  --cpuid                 打印 CPU 指令集能力 JSON 后退出（avx2 / avx512vnni）\n");
            return 0;
        }
    }

    if (cpuidDump) {
        
        int r[4] = {0, 0, 0, 0};
        __cpuidex(r, 0, 0);
        int maxLeaf = r[0];
        bool avx2 = false, avx512f = false, avx512vnni = false, avxvnni = false;
        if (maxLeaf >= 1) {
            __cpuidex(r, 1, 0);
            bool osxsave = (r[2] & (1 << 27)) != 0;
            bool avx0    = (r[2] & (1 << 28)) != 0;
            unsigned long long xcr0 = osxsave ? _xgetbv(0) : 0;
            bool xmmYmm = osxsave && (xcr0 & 0x06) == 0x06;   
            bool zmm    = osxsave && (xcr0 & 0xE6) == 0xE6;   
            if (maxLeaf >= 7) {
                __cpuidex(r, 7, 0);
                bool avx2bit = (r[1] >> 5) & 1;
                bool f512 = (r[1] >> 16) & 1, dq = (r[1] >> 17) & 1;
                bool bw   = (r[1] >> 30) & 1, vl = (r[1] >> 31) & 1;
                bool v512 = (r[2] >> 11) & 1;                  
                avx2      = avx0 && xmmYmm && avx2bit;
                avx512f   = avx2 && zmm && f512 && dq && bw && vl;
                avx512vnni = avx512f && v512;
                __cpuidex(r, 7, 1);                            
                avxvnni   = avx2 && (((unsigned)r[0] >> 4) & 1) != 0;
            }
        }
        Json j = Json::makeObj();
        j.set("ok", Json::makeBool(true));
        j.set("sse", Json::makeBool(true));            
        j.set("avx2", Json::makeBool(avx2));
        j.set("avx512f", Json::makeBool(avx512f));
        j.set("avx512vnni", Json::makeBool(avx512vnni));
        j.set("avxvnni", Json::makeBool(avxvnni));
        printf("%s\n", j.dump().c_str());
        return 0;
    }

    if (!scanImage.empty() || !recognizeImage.empty()) {
        if (!LoadModelDll(true)) {
            fprintf(stderr, "[vision] model dll not available, cannot run offline entry\n");
            return 2;
        }
        if (!scanImage.empty()) {
            std::vector<unsigned char> raw = fileBytes(scanImage.c_str());
            if (raw.empty()) {
                fprintf(stderr, "[vision] cannot read image: %s\n", scanImage.c_str());
                return 2;
            }
            
            std::string resp;
            if (!ModelScanImage(raw.data(), (int)raw.size(), probeImage, resp)) {
                fprintf(stderr, "[vision] model scan-image failed\n");
                return 2;
            }
            printf("%s\n", resp.c_str());
        } else {
            std::vector<unsigned char> raw = fileBytes(recognizeImage.c_str());
            if (raw.empty()) {
                fprintf(stderr, "[vision] cannot read image: %s\n", recognizeImage.c_str());
                return 2;
            }
            std::string body = "{\"image\":\"" + base64Encode(raw) + "\",\"size\":15";
            if (nosnap) body += ",\"nosnap\":true";
            if (algoSync == "sync") {
                int g = 50, bo = 96, wo = 96, bp = 33, wp = 33;
                if (!recogCsv.empty())
                    sscanf(recogCsv.c_str(), "%d,%d,%d,%d,%d", &g, &bo, &wo, &bp, &wp);
                char rb[192];
                snprintf(rb, sizeof(rb),
                         ",\"algo\":\"sync\",\"recog\":{\"gray_offset\":%d,\"black_offset\":%d,"
                         "\"white_offset\":%d,\"black_percent\":%d,\"white_percent\":%d}",
                         g, bo, wo, bp, wp);
                body += rb;
            }
            body += "}";
            std::string resp;
            if (!ModelRecognizeJson(body, resp)) {
                fprintf(stderr, "[vision] model recognize failed\n");
                return 2;
            }
            printf("%s\n", resp.c_str());
        }
        return 0;
    }

    LoadModelDll(false);

    std::vector<int> ports;
    if (role == "recognize" || role == "both") ports.push_back(recognizePort());
    if (role == "scan" || role == "both") ports.push_back(scanPort());

    std::string mtxName = "Global\\GomokuVision_" + role;
    for (int p : ports) mtxName += "_" + std::to_string(p);
    mtxName += "_v2";
    gMutex = CreateMutexW(nullptr, TRUE, std::wstring(mtxName.begin(), mtxName.end()).c_str());
    if (gMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        printf("[vision] another %s instance (%s) is already running - exit.\n",
               role.c_str(), mtxName.c_str());
        return 0;
    }

    {
        char v[32] = {0};
        if (GetEnvironmentVariableA("GB_PARENT_PID", v, sizeof(v)) && v[0])
            g_parentPid = (DWORD)atoi(v);
    }

    const bool explicitSingleRole = (role == "scan" || role == "recognize");
    const bool wantHttp = HttpEnabledByEnv(explicitSingleRole);
    bool anyOk = false;

    if (wantHttp) {
        for (int port : ports) {
            std::string err;
            bool isScan = (port == scanPort());
            HttpHandler handler = isScan ? HttpHandler(handleScan) : HttpHandler(handleRecognize);
            if (startHttpServer(port, handler, err)) {
                printf("[vision] %s listening on 127.0.0.1:%d\n",
                       isScan ? "scan service" : "recognize service", port);
                anyOk = true;
            } else {
                printf("[vision] %s failed to start on :%d (%s)\n",
                       isScan ? "scan" : "recognize", port, err.c_str());
            }
        }
    }

    for (int port : ports) {
        std::string perr;
        const char* prole = (port == scanPort()) ? "scan" : "recognize";
        if (startPipeServer(prole, port, perr)) {
            printf("[vision] pipe listening on \\\\.\\pipe\\GomokuVision.%s.%d\n", prole, port);
            anyOk = true;
        } else {
            printf("[vision] pipe %s failed to start (%s)\n", prole, perr.c_str());
        }
    }
    printf("[vision] transport = named pipe only%s\n",
           wantHttp ? " + opt-in TCP (GB_VISION_HTTP=1)" : " (no TCP listener in this process)");
    fflush(stdout);
    if (!anyOk) {
        if (gMutex) ReleaseMutex(gMutex);
        return 1;
    }

    ULONGLONG lastBeltCheck = GetTickCount64();
    while (!gStopping.load()) {
        Sleep(500);
        if (g_parentPid == 0) continue;                       
        ULONGLONG now = GetTickCount64();
        if (now - lastBeltCheck < 5000) continue;
        lastBeltCheck = now;
        bool parentAlive = false;
        HANDLE hp = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                FALSE, g_parentPid);
        if (hp) {
            parentAlive = (WaitForSingleObject(hp, 0) == WAIT_TIMEOUT);
            CloseHandle(hp);
        }
        if (parentAlive) continue;                            
        if (now - g_lastTrafficAt <= 30000) continue;         
        printf("[vision] parent (pid %lu) gone + ports quiet 30s -> orphan self-exit\n",
               (unsigned long)g_parentPid);
        fflush(stdout);
        break;
    }
    if (gMutex) ReleaseMutex(gMutex);
    return 0;
}
