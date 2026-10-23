// Meter Gomoku Link — v4.43
/* Vision - boardpipe. */


#include "gbpipe.h"
#include "gb.h"
#include "gbloader.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gb {

namespace {

constexpr uint32_t PROTO_VER = 1;
constexpr uint32_t MAX_PAYLOAD = 4u * 1024 * 1024;    
constexpr uint32_t MAX_SHM_IMG  = 128u * 1024 * 1024; 

enum Opcode : uint32_t {
    OP_HELLO = 1,
    OP_SCAN = 2,
    OP_SCAN_IMAGE = 3,
    OP_RECOGNIZE = 4,
    OP_PING = 5,
};

constexpr uint32_t ST_OK = 0;            
constexpr uint32_t ST_MODEL = 1;         
constexpr uint32_t ST_BADREQ = 2;        

#pragma pack(push, 1)
struct FrameHdr {                        
    char     magic[4];                   
    uint32_t opcode;
    uint32_t payloadLen;
};
#pragma pack(pop)

bool readExact(HANDLE h, void* buf, uint32_t n) {
    char* p = (char*)buf;
    while (n > 0) {
        DWORD got = 0;
        if (!ReadFile(h, p, n, &got, nullptr) || got == 0) return false;
        p += got; n -= got;
    }
    return true;
}

bool writeExact(HANDLE h, const void* buf, uint32_t n) {
    const char* p = (const char*)buf;
    while (n > 0) {
        DWORD put = 0;
        if (!WriteFile(h, p, n, &put, nullptr) || put == 0) return false;
        p += put; n -= put;
    }
    return true;
}

bool writeFrame(HANDLE h, uint32_t opcode, const void* payload, uint32_t len) {
    FrameHdr hdr;
    memcpy(hdr.magic, "GBV1", 4);
    hdr.opcode = opcode;
    hdr.payloadLen = len;
    if (!writeExact(h, &hdr, sizeof(hdr))) return false;
    if (len) return writeExact(h, payload, len);
    return true;
}

bool writeStatus(HANDLE h, uint32_t opcode, uint32_t status, const std::string& resp) {
    std::vector<unsigned char> out(8 + resp.size());
    memcpy(out.data(), &status, 4);
    uint32_t rl = (uint32_t)resp.size();
    memcpy(out.data() + 4, &rl, 4);
    if (!resp.empty()) memcpy(out.data() + 8, resp.data(), resp.size());
    return writeFrame(h, opcode, out.data(), (uint32_t)out.size());
}

uint32_t rd32(const unsigned char* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static int safeCopy(const unsigned char* base, size_t off, size_t len, unsigned char* dst) {
    __try {
        memcpy(dst, base + off, len);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

struct ShmCache {
    std::vector<std::pair<std::wstring, HANDLE>>      sections;
    std::vector<std::pair<std::wstring, const void*>> views;

    ~ShmCache() {
        for (auto& v : views) if (v.second) UnmapViewOfFile(v.second);
        for (auto& s : sections) if (s.second) CloseHandle(s.second);
    }

    bool read(const std::wstring& name, uint32_t off, uint32_t len, std::vector<unsigned char>& out) {
        if (name.empty() || len == 0 || len > MAX_SHM_IMG) return false;
        const void* view = nullptr;
        for (size_t i = 0; i < sections.size(); ++i) {
            if (sections[i].first == name) { view = views[i].second; break; }
        }
        if (!view) {
            HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
            if (!h) return false;
            void* v = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
            if (!v) { CloseHandle(h); return false; }
            sections.emplace_back(name, h);
            views.emplace_back(name, v);
            view = v;
        }
        out.resize(len);
        if (!safeCopy((const unsigned char*)view, off, len, out.data())) return false;
        return true;
    }
};

bool assembleRecognizeBody(const std::vector<unsigned char>& img, const std::string& extra,
                           std::string& body) {
    if (img.empty()) { body = extra; return !body.empty(); }
    if (extra.size() < 2 || extra[0] != '{' || extra[extra.size() - 1] != '}') return false;
    body = "{\"image\":\"" + base64Encode(img) + "\"," + extra.substr(1);
    return true;
}

struct ShmRef {
    std::wstring name;
    uint32_t     off = 0;
    uint32_t     len = 0;
    std::string  body;    
    bool         ok = false;
};

ShmRef parseRef(uint32_t opcode, const std::vector<unsigned char>& pl) {
    ShmRef r;
    if (opcode == OP_SCAN) {                       
        if (pl.size() < 4) return r;
        uint32_t bodyLen = rd32(pl.data());
        if (pl.size() < 4 + bodyLen) return r;
        r.body.assign((const char*)pl.data() + 4, bodyLen);
        r.ok = true;
        return r;
    }
    
    if (pl.size() < 12) return r;                  
    uint32_t nameLen = rd32(pl.data());
    if (nameLen > 260) return r;
    size_t need = 4 + nameLen + 8;
    if (opcode == OP_RECOGNIZE) need += 4;
    if (pl.size() < need) return r;
    r.name = utf8ToWide(std::string((const char*)pl.data() + 4, nameLen));
    r.off = rd32(pl.data() + 4 + nameLen);
    r.len = rd32(pl.data() + 4 + nameLen + 4);
    size_t cur = 4 + nameLen + 8;
    if (opcode == OP_RECOGNIZE) {
        uint32_t extraLen = rd32(pl.data() + cur);
        cur += 4;
        if (pl.size() < cur + extraLen) return r;
        r.body.assign((const char*)pl.data() + cur, extraLen);
    }
    r.ok = true;
    return r;
}

void handleConn(HANDLE h) {
    ShmCache shm;                          
    std::vector<unsigned char> img;        
    std::string extra, resp, body;

    for (;;) {
        FrameHdr hdr;
        if (!readExact(h, &hdr, sizeof(hdr))) break;
        if (memcmp(hdr.magic, "GBV1", 4) != 0) break;           
        if (hdr.opcode == 0 || hdr.opcode > OP_PING) break;
        if (hdr.payloadLen > MAX_PAYLOAD) break;

        if (hdr.opcode == OP_HELLO) {
            unsigned char out[8];
            uint32_t v = PROTO_VER;
            uint32_t rd = ModelDllReady() ? 1 : 0;
            memcpy(out, &v, 4);
            memcpy(out + 4, &rd, 4);
            if (!writeFrame(h, OP_HELLO, out, 8)) break;
            continue;
        }
        if (hdr.opcode == OP_PING) {
            if (!writeFrame(h, OP_PING, nullptr, 0)) break;
            continue;
        }

        std::vector<unsigned char> payload;
        if (hdr.payloadLen) {
            payload.resize(hdr.payloadLen);
            if (!readExact(h, payload.data(), hdr.payloadLen)) break;
        }
        ShmRef ref = parseRef(hdr.opcode, payload);
        if (!ref.ok) { writeStatus(h, hdr.opcode, ST_BADREQ, std::string()); continue; }

        img.clear();
        if (ref.len > 0 && !shm.read(ref.name, ref.off, ref.len, img)) {
            writeStatus(h, hdr.opcode, ST_BADREQ, std::string());
            continue;
        }

        uint32_t status = ST_OK;
        resp.clear();
        if (hdr.opcode == OP_SCAN) {
            if (!ModelScan(ref.body, resp)) status = ST_MODEL;
        } else if (hdr.opcode == OP_SCAN_IMAGE) {
            if (!ModelScanImage(img.empty() ? nullptr : img.data(), (int)img.size(), false, resp))
                status = ST_MODEL;
        } else {  
            body.clear();
            if (!assembleRecognizeBody(img, ref.body, body) || !ModelRecognizeJson(body, resp))
                status = ST_MODEL;
        }
        if (!writeStatus(h, hdr.opcode, status, resp)) break;
    }

    FlushFileBuffers(h);
    DisconnectNamedPipe(h);
    CloseHandle(h);
}

void listenLoop(const std::wstring& pipeName) {
    for (;;) {
        HANDLE h = CreateNamedPipeW(
            pipeName.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            64 * 1024, 64 * 1024,
            0, nullptr);
        if (h == INVALID_HANDLE_VALUE) { Sleep(500); continue; }
        BOOL ok = ConnectNamedPipe(h, nullptr) ? TRUE
                : (GetLastError() == ERROR_PIPE_CONNECTED ? TRUE : FALSE);
        if (ok) {
            gb::TouchTraffic();                    
            std::thread(handleConn, h).detach();   
        } else {
            CloseHandle(h);
        }
    }
}

}  

bool startPipeServer(const std::string& role, int port, std::string& err) {
    if (role != "recognize" && role != "scan") { err = "bad role"; return false; }
    
    std::wstring name = L"\\\\.\\pipe\\GomokuVision." + utf8ToWide(role) +
                        L"." + std::to_wstring(port);
    std::thread(listenLoop, name).detach();
    return true;
}

}  
