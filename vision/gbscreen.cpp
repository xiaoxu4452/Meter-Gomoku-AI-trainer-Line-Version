// Meter Gomoku Link — v4.43
/* Vision - boardscreen. */


#include "gbvision.h"

#include <windows.h>
#include <shellscalingapi.h>
#include <cmath>
#include <limits>

#include <d3d11.h>
#include <dxgi1_2.h>

namespace gb {

namespace {

inline int iRound(double v) { return (int)std::nearbyint(v); }
inline double npRound(double v) { return std::nearbyint(v); }

int screenOx = 0, screenOy = 0;

BOOL CALLBACK enumMonitorProc(HMONITOR, HDC, LPRECT, LPARAM) { return TRUE; }

inline long long nowMs() {
    return (long long)(cv::getTickCount() * 1000 / cv::getTickFrequency());
}

inline bool gbNoDxgi() {
    static const bool off = [] {
        char v[4] = {0};
        return GetEnvironmentVariableA("GB_NO_DXGI", v, sizeof(v)) > 0 && v[0] == '1';
    }();
    return off;
}

struct DxgiCapturer {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGIOutputDuplication* dup = nullptr;
    ID3D11Texture2D* staging = nullptr;
    int w = 0, h = 0, ox = 0, oy = 0;
    bool tried = false;        
    bool usable = false;       
    bool lost = false;         
    bool everWorked = false;   
    cv::Mat last;              

    void ReleaseAll() {
        if (staging) { staging->Release(); staging = nullptr; }
        if (dup) { dup->Release(); dup = nullptr; }
        if (ctx) { ctx->Release(); ctx = nullptr; }
        if (dev) { dev->Release(); dev = nullptr; }
        last.release();
        usable = false;
    }

    bool Init() {
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (vw <= 0 || vh <= 0) return false;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                       D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        if (FAILED(hr) || !dev || !ctx) { ReleaseAll(); return false; }
        IDXGIDevice* dxgiDev = nullptr;
        if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) || !dxgiDev) {
            ReleaseAll();
            return false;
        }
        IDXGIAdapter* ad = nullptr;
        hr = dxgiDev->GetAdapter(&ad);
        dxgiDev->Release();
        if (FAILED(hr) || !ad) { ReleaseAll(); return false; }
        bool matched = false;
        for (UINT i = 0; i < 16 && !matched; ++i) {
            IDXGIOutput* out = nullptr;
            if (ad->EnumOutputs(i, &out) != S_OK || !out) break;
            DXGI_OUTPUT_DESC od;
            ZeroMemory(&od, sizeof(od));
            if (SUCCEEDED(out->GetDesc(&od)) && od.AttachedToDesktop) {
                const RECT& r = od.DesktopCoordinates;
                if ((int)r.left == vx && (int)r.top == vy &&
                    (int)(r.right - r.left) == vw && (int)(r.bottom - r.top) == vh) {
                    IDXGIOutput1* o1 = nullptr;
                    if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput1), (void**)&o1)) && o1) {
                        IDXGIOutputDuplication* dup = nullptr;
                        if (SUCCEEDED(o1->DuplicateOutput(dev, &dup)) && dup) {
                            this->dup = dup;
                            matched = true;
                        }
                        o1->Release();
                    }
                }
            }
            out->Release();
        }
        ad->Release();
        if (!matched || !dup) { ReleaseAll(); return false; }
        D3D11_TEXTURE2D_DESC td;
        ZeroMemory(&td, sizeof(td));
        td.Width = (UINT)vw;
        td.Height = (UINT)vh;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging)) || !staging) {
            ReleaseAll();
            return false;
        }
        w = vw; h = vh; ox = vx; oy = vy;
        usable = true;
        return true;
    }

    bool Grab(cv::Mat& rgb, bool& reused) {
        reused = false;
        if (!usable) return false;
        if (gbNoDxgi()) { usable = false; return false; }
        DXGI_OUTDUPL_FRAME_INFO info;
        ZeroMemory(&info, sizeof(info));
        IDXGIResource* res = nullptr;
        
        HRESULT hr = dup->AcquireNextFrame(0, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            if (!last.empty()) { rgb = last; reused = true; return true; }
            return false;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
            lost = true;
            ReleaseAll();
            return false;
        }
        if (FAILED(hr) || !res) return false;
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex) {
            ctx->CopyResource(staging, tex);       
            tex->Release();
        }
        res->Release();
        bool ok = false;
        D3D11_MAPPED_SUBRESOURCE m;
        ZeroMemory(&m, sizeof(m));
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            
            cv::Mat view(h, w, CV_8UC4, m.pData, (size_t)m.RowPitch);
            cv::cvtColor(view, rgb, cv::COLOR_BGRA2RGB);
            ctx->Unmap(staging, 0);
            ok = true;
        }
        dup->ReleaseFrame();
        if (ok) {
            last = rgb;                            
            everWorked = true;
        }
        return ok;
    }
};

DxgiCapturer& DxgiCap() {
    static DxgiCapturer c;
    
    if (!c.tried || c.lost) {
        c.tried = true;
        c.lost = false;
        c.ReleaseAll();
        if (!gbNoDxgi()) {
            bool ok = c.Init();
            static bool logged = false;
            if (!logged) {
                logged = true;
                if (ok) fprintf(stderr, "[vision] capture: DXGI 桌面复制可用（显卡抓屏）\n");
                else fprintf(stderr, "[vision] capture: DXGI 不可用 → 回退 GDI BitBlt\n");
            }
        }
    }
    return c;
}

enum class CapMode { DXGI, GDI };
CapMode g_capMode = CapMode::GDI;
bool    g_capDecided = false;
int     g_dxgiFails = 0;               
int     g_dxgiFresh = 0;               
long long g_dxgiFreshSum = 0;          
long long g_gdiBaseMs = -1;            

static bool GdiCapture(cv::Mat& rgb, int& ox, int& oy) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return false;

    HDC screen = GetDC(NULL);
    if (!screen) return false;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, vw, vh);
    if (!mem || !bmp) {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        ReleaseDC(NULL, screen);
        return false;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    
    BOOL ok = BitBlt(mem, 0, 0, vw, vh, screen, vx, vy, SRCCOPY | CAPTUREBLT);

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = vw;
    bi.bmiHeader.biHeight = -vh;          
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    cv::Mat bgra(vh, vw, CV_8UC4);
    int got = 0;
    if (ok) got = GetDIBits(mem, bmp, 0, (UINT)vh, bgra.data, &bi, DIB_RGB_COLORS);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    if (!ok || got == 0) return false;

    cv::cvtColor(bgra, rgb, cv::COLOR_BGRA2RGB);
    ox = vx; oy = vy;
    screenOx = vx; screenOy = vy;
    return true;
}

void CapSelect() {
    if (g_capDecided) return;
    g_capDecided = true;
    char v[8] = {0};
    // CPU-only by default (matches the 10-04 assistant build: GDI BitBlt,
    // no GPU/DXGI dependency). GB_FORCE_CAPTURE=dxgi still opts into the
    // DXGI desktop-duplication path for testing.
    bool forceGdi = true, forceDxgi = false;
    if (GetEnvironmentVariableA("GB_FORCE_CAPTURE", v, sizeof(v)) > 0) {
        if (_stricmp(v, "gdi") == 0) forceGdi = true;
        else if (_stricmp(v, "dxgi") == 0) forceDxgi = true;
    }
    if (forceGdi) {
        g_capMode = CapMode::GDI;
        fprintf(stderr, "[vision] capture: 纯 CPU 抓屏（GDI BitBlt）—— 默认全 CPU（与 10-04 助手版一致）\n");
        return;
    }
    if (!forceDxgi && !DxgiCap().usable) {
        g_capMode = CapMode::GDI;
        fprintf(stderr, "[vision] capture: 纯 CPU 抓屏（GDI BitBlt）—— DXGI 不可用"
                        "（多屏拼接 / RDP / 无显卡驱动 / 被独占拒绝）\n");
        return;
    }
    g_capMode = CapMode::DXGI;
    fprintf(stderr, "[vision] capture: 显卡抓屏（DXGI 桌面复制）—— 将用真机数据持续校验它是否真比 CPU 快\n");
}

void DxgiPerfSample(bool reused, bool firstFresh, long long ms) {
    if (g_capMode != CapMode::DXGI) return;
    if (reused || firstFresh) return;      
    g_dxgiFresh++;
    g_dxgiFreshSum += ms;
    if (g_dxgiFresh == 4 && g_gdiBaseMs < 0) {
        
        cv::Mat t; int tx = 0, ty = 0; long long b = -1;
        for (int i = 0; i < 2; ++i) {
            long long t0 = nowMs();
            if (GdiCapture(t, tx, ty)) { long long d = nowMs() - t0; if (b < 0 || d < b) b = d; }
        }
        g_gdiBaseMs = b;
        if (b > 0)
            fprintf(stderr, "[vision] capture: 纯 CPU 基准 %.1f ms/帧（同一时刻实测）\n", (double)b);
    }
    if (g_dxgiFresh >= 12 && g_gdiBaseMs > 0) {
        double avg = (double)g_dxgiFreshSum / (double)g_dxgiFresh;
        if (avg > (double)g_gdiBaseMs * 0.9) {
            g_capMode = CapMode::GDI;
            fprintf(stderr, "[vision] capture: 实测显卡抓屏 %.1f ms/帧 不优于纯 CPU %.1f ms/帧"
                            " → 永久切换为全 CPU\n", avg, (double)g_gdiBaseMs);
        }
    }
}

}  

bool captureScreen(cv::Mat& rgb, int& ox, int& oy) {
    setDpiAware();
    const bool perf = getenv("GB_VISION_PERF") != nullptr;
    const long long t0 = perf ? nowMs() : 0;

    CapSelect();

    if (g_capMode == CapMode::DXGI) {
        DxgiCapturer& cap = DxgiCap();
        bool reused = false;
        const bool firstFresh = !cap.everWorked;   
        const long long t1 = nowMs();
        if (cap.Grab(rgb, reused)) {
            g_dxgiFails = 0;
            ox = cap.ox;
            oy = cap.oy;
            screenOx = cap.ox;
            screenOy = cap.oy;
            if (perf)
                fprintf(stderr, "[perf] capture mode=dxgi ms=%lld%s\n", nowMs() - t0,
                        reused ? " (desktop unchanged, reused)" : "");
            DxgiPerfSample(reused, firstFresh, nowMs() - t1);
            return true;
        }
        
        if (++g_dxgiFails >= 3) {
            g_capMode = CapMode::GDI;
            fprintf(stderr, "[vision] capture: 显卡抓屏连续 %d 帧失败 → 永久切换为全 CPU（GDI BitBlt）\n",
                    g_dxgiFails);
        }
    }

    if (GdiCapture(rgb, ox, oy)) {
        if (perf) fprintf(stderr, "[perf] capture mode=gdi ms=%lld\n", nowMs() - t0);
        return true;
    }
    return false;
}

// ★ 2026-10-18（用户：「识别速度再快一点，当前稍慢」＋「每拍读子更快」）：
//   连线稳态每拍只关心「已框定区域」这一小块像素，但旧路径每拍都 BitBlt 整个虚拟屏
//   （1920×1080 的 32 位抓取 + 全屏 BGRA→RGB 转换）再裁到 region —— 每拍固定多花十几毫秒。
//   这里按 region 直接抓那一块（400×400 时抓取/转换量约全屏的 1/13）。
//   只走 CPU(GDI) 抓屏：DXGI 分支本就有整帧复用，走它反而更慢；越界或抓取失败一律返回
//   false，由上层原样回退到整屏抓取，行为不变。
bool captureScreenRect(cv::Mat& rgb, int& ox, int& oy, int x, int y, int w, int h) {
    setDpiAware();
    if (w <= 0 || h <= 0) return false;

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return false;
    if (x < vx || y < vy || x + w > vx + vw || y + h > vy + vh) return false;

    const bool perf = getenv("GB_VISION_PERF") != nullptr;
    const long long t0 = perf ? nowMs() : 0;

    HDC screen = GetDC(NULL);
    if (!screen) return false;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    if (!mem || !bmp) {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        ReleaseDC(NULL, screen);
        return false;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    BOOL ok = BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    cv::Mat bgra(h, w, CV_8UC4);
    int got = 0;
    if (ok) got = GetDIBits(mem, bmp, 0, (UINT)h, bgra.data, &bi, DIB_RGB_COLORS);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    if (!ok || got == 0) return false;

    cv::cvtColor(bgra, rgb, cv::COLOR_BGRA2RGB);
    ox = x; oy = y;
    screenOx = x; screenOy = y;
    if (perf) fprintf(stderr, "[perf] capture mode=gdi-rect %dx%d ms=%lld\n",
                      w, h, nowMs() - t0);
    return true;
}

namespace {

struct Cluster { double mean; double len; double lo; double hi; };

std::vector<Cluster> clusterSegments(const std::vector<double>& vals, double tol) {
    std::vector<Cluster> out;
    if (vals.empty()) return out;
    std::vector<double> v = vals;
    std::sort(v.begin(), v.end());
    std::vector<double> cur{v[0]};
    for (size_t i = 1; i < v.size(); ++i) {
        if (v[i] - cur.back() <= tol) cur.push_back(v[i]);
        else {
            out.push_back({mean(cur), 0.0, 0.0, 0.0});
            cur.assign(1, v[i]);
        }
    }
    out.push_back({mean(cur), 0.0, 0.0, 0.0});
    return out;
}

std::vector<Cluster> weightedClusters(std::vector<double> vals, std::vector<double> lens,
                                      std::vector<std::pair<double, double>> exts, double tol) {
    std::vector<Cluster> out;
    if (vals.empty()) return out;
    std::vector<int> order = argsort(vals);
    std::vector<double> curV{vals[order[0]]}, curL{lens[order[0]]};
    std::vector<std::pair<double, double>> curE{exts[order[0]]};
    for (size_t k = 1; k < order.size(); ++k) {
        int i = order[k];
        if (vals[i] - curV.back() <= tol) {
            curV.push_back(vals[i]);
            curL.push_back(lens[i]);
            curE.push_back(exts[i]);
        } else {
            double lo = curE[0].first, hi = curE[0].second, lenSum = 0;
            for (auto& e : curE) { lo = std::min(lo, e.first); hi = std::max(hi, e.second); }
            for (double l : curL) lenSum += l;
            out.push_back({mean(curV), lenSum, lo, hi});
            curV.assign(1, vals[i]);
            curL.assign(1, lens[i]);
            curE.assign(1, exts[i]);
        }
    }
    {
        double lo = curE[0].first, hi = curE[0].second, lenSum = 0;
        for (auto& e : curE) { lo = std::min(lo, e.first); hi = std::max(hi, e.second); }
        for (double l : curL) lenSum += l;
        out.push_back({mean(curV), lenSum, lo, hi});
    }
    return out;
}

cv::Mat cannyScreen(const cv::Mat& gray, double ratio = 0.40) {
    cv::Mat g, gx, gy;
    cv::GaussianBlur(gray, g, cv::Size(3, 3), 0);
    cv::Sobel(g, gx, CV_32F, 1, 0, 3);
    cv::Sobel(g, gy, CV_32F, 0, 1, 3);
    cv::Mat mag;
    cv::add(cv::abs(gx), cv::abs(gy), mag);
    
    std::vector<float> all(mag.total());
    std::copy(mag.ptr<float>(0), mag.ptr<float>(0) + all.size(), all.data());
    double mx = percentileF(all, 99.5);
    if (mx <= 1e-6) return cv::Mat::zeros(gray.size(), CV_8U);

    cv::Mat m8(mag.rows, mag.cols, CV_8U);
    for (int r = 0; r < mag.rows; ++r) {
        const float* sp = mag.ptr<float>(r);
        uint8_t* dp = m8.ptr<uint8_t>(r);
        for (int c = 0; c < mag.cols; ++c) {
            double v = (double)sp[c] / mx * 255.0;
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            dp[c] = (uint8_t)v;          
        }
    }
    double t = cv::threshold(m8, m8, 0, 255, cv::THRESH_BINARY + cv::THRESH_OTSU);
    double hi = t / 255.0 * mx;
    if (hi < 4.0) hi = std::max(8.0, mx * 0.35);
    double lo = std::max(4.0, hi * ratio);
    cv::Mat edges;
    cv::Canny(gray, edges, lo, hi);
    return edges;
}

void collectLinesEdges(const cv::Mat& edges, double minLen, std::vector<Cluster>& hOut,
                       std::vector<Cluster>& vOut, int maxGap = 6, int thresh = 80);

void collectLines(const cv::Mat& gray, double minLen, std::vector<Cluster>& hOut,
                  std::vector<Cluster>& vOut, int maxGap = 6, int thresh = 80) {
    cv::Mat edges = cannyScreen(gray);
    collectLinesEdges(edges, minLen, hOut, vOut, maxGap, thresh);
}

void collectLinesEdges(const cv::Mat& edges, double minLen, std::vector<Cluster>& hOut,
                       std::vector<Cluster>& vOut, int maxGap, int thresh) {
    hOut.clear();
    vOut.clear();
    std::vector<cv::Vec4i> lines;
    cv::HoughLinesP(edges, lines, 1.0, CV_PI / 180.0, thresh, (int)minLen, maxGap);
    if (lines.empty()) return;
    std::vector<double> hV, vV, hL, vL;
    std::vector<std::pair<double, double>> hE, vE;
    for (const cv::Vec4i& s : lines) {
        double x1 = s[0], y1 = s[1], x2 = s[2], y2 = s[3];
        double dx = std::abs(x2 - x1), dy = std::abs(y2 - y1);
        double ln = std::hypot(dx, dy);
        if (dx >= dy) {
            hV.push_back((y1 + y2) / 2.0);
            hL.push_back(ln);
            hE.push_back({std::min(x1, x2), std::max(x1, x2)});
        } else {
            vV.push_back((x1 + x2) / 2.0);
            vL.push_back(ln);
            vE.push_back({std::min(y1, y2), std::max(y1, y2)});
        }
    }
    hOut = weightedClusters(hV, hL, hE, 3.0);
    vOut = weightedClusters(vV, vL, vE, 3.0);
}

struct Family {
    std::vector<double> pos;
    double len = 0;
    double step = 0;
    double score = 0;
    double lo = 0;
    double hi = 0;
};

std::vector<Family> arithmeticCandidates(const std::vector<Cluster>& clusters,
                                         int minCount, double maxStep) {
    std::vector<Family> cands;
    if ((int)clusters.size() < minCount) return cands;
    
    std::vector<double> lens;
    for (auto& c : clusters) lens.push_back(c.len);
    double maxLen = lens.empty() ? 0 : *std::max_element(lens.begin(), lens.end());
    double thrLen = maxLen * 0.15;
    std::vector<Cluster> cand;
    for (auto& c : clusters) if (c.len >= thrLen) cand.push_back(c);
    if ((int)cand.size() < minCount) cand = clusters;

    int n = (int)cand.size();
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n && j < i + 40; ++j) {
            double d = (cand[j].mean - cand[i].mean) / double(j - i);
            if (d < 8.0 || d > maxStep) continue;
            double tol = std::max(2.5, d * 0.06);
            std::vector<double> seq{cand[i].mean, cand[j].mean};
            std::vector<double> tot{cand[i].len, cand[j].len};
            std::vector<Cluster> idx{cand[i], cand[j]};
            double expect = cand[j].mean + d;
            int k = j + 1;
            while (k < n) {
                double df = cand[k].mean - expect;
                if (std::abs(df) <= tol) {
                    seq.push_back(cand[k].mean);
                    tot.push_back(cand[k].len);
                    idx.push_back(cand[k]);
                    expect = cand[k].mean + d;
                } else if (df > tol) {
                    
                    int m = iRound(df / d);
                    if (m >= 1 && std::abs(cand[k].mean - (expect + m * d)) <= tol) {
                        seq.push_back(cand[k].mean);
                        tot.push_back(cand[k].len);
                        idx.push_back(cand[k]);
                        expect = cand[k].mean + d;
                    } else break;
                }
                k++;
            }
            if ((int)seq.size() >= minCount) {
                double ls = 0, los = 1e300, his = -1e300;
                for (double t : tot) ls += t;
                for (auto& c : idx) { los = std::min(los, c.lo); his = std::max(his, c.hi); }
                Family f;
                f.pos = seq;
                f.len = ls / (double)tot.size();
                f.step = d;
                f.score = (double)seq.size() * (ls / (double)tot.size());
                f.lo = los;
                f.hi = his;
                cands.push_back(f);
            }
        }
    }
    std::stable_sort(cands.begin(), cands.end(),
                     [](const Family& a, const Family& b) { return a.score > b.score; });
    return cands;
}

std::vector<Family> findArithmeticFamilies(const std::vector<Cluster>& clusters,
                                           int minCount = 8, double maxStep = 260.0,
                                           int topk = 6) {
    std::vector<Family> fams = arithmeticCandidates(clusters, minCount, maxStep);
    std::vector<Family> out;
    for (auto& f : fams) {
        bool dup = false;
        for (auto& g : out) {
            
            std::vector<double> a, b;
            for (double v : f.pos) a.push_back(npRound(v * 10.0) / 10.0);
            for (double v : g.pos) b.push_back(npRound(v * 10.0) / 10.0);
            std::sort(a.begin(), a.end());
            std::sort(b.begin(), b.end());
            std::vector<double> inter;
            std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                                  std::back_inserter(inter));
            size_t mn = std::min(a.size(), b.size());
            if (!mn && inter.empty()) { dup = true; break; }
            if ((double)inter.size() >= 0.7 * (double)mn) { dup = true; break; }
        }
        if (!dup) out.push_back(f);
        if ((int)out.size() >= topk) break;
    }
    return out;
}

bool subsampleFamily(const Family& fam, int m, Family& out) {
    if (m <= 1) { out = fam; return true; }
    Family f;
    for (size_t i = 0; i < fam.pos.size(); i += m) f.pos.push_back(fam.pos[i]);
    if (f.pos.size() < 6) return false;
    f.step = fam.step * m;
    f.len = fam.len;
    f.lo = fam.lo;
    f.hi = fam.hi;
    out = f;
    return true;
}

struct BoardCand {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    double step = 0;
    std::vector<double> vx, hy;
    double score = 0;
    int size = 0;
    double scale = 1.0;
};

bool pairOnce(const Family& hy, const Family& vx, int gw, int gh, double sc,
              BoardCand& out) {
    double dx = vx.step, dy = hy.step;
    if (dx <= 0 || dy <= 0) return false;
    if (std::abs(dx - dy) / std::max(dx, dy) > 0.12) return false;   
    double x0 = *std::min_element(vx.pos.begin(), vx.pos.end());
    double x1 = *std::max_element(vx.pos.begin(), vx.pos.end());
    double y0 = *std::min_element(hy.pos.begin(), hy.pos.end());
    double y1 = *std::max_element(hy.pos.begin(), hy.pos.end());
    
    if (hy.hi > hy.lo && vx.hi > vx.lo) {
        double interX = std::min(x1, hy.hi) - std::max(x0, hy.lo);
        double interY = std::min(y1, vx.hi) - std::max(y0, vx.lo);
        if (interX < 0.35 * std::min(x1 - x0, hy.hi - hy.lo)) return false;
        if (interY < 0.35 * std::min(y1 - y0, vx.hi - vx.lo)) return false;
    }
    double area = std::max(1.0, (x1 - x0) * (y1 - y0));
    if (area < 0.008 * gw * gh || area > 0.98 * gw * gh) return false;
    double gshort = std::min(gw, gh);
    int n = (int)vx.pos.size() + (int)hy.pos.size();
    double relLen = (vx.len + hy.len) * 0.5 / std::max(1.0, gshort);
    out.x0 = x0 / sc; out.y0 = y0 / sc; out.x1 = x1 / sc; out.y1 = y1 / sc;
    out.step = (dx + dy) * 0.5 / sc;
    out.vx.resize(vx.pos.size());
    for (size_t i = 0; i < vx.pos.size(); ++i) out.vx[i] = vx.pos[i] / sc;
    out.hy.resize(hy.pos.size());
    for (size_t i = 0; i < hy.pos.size(); ++i) out.hy[i] = hy.pos[i] / sc;
    out.score = n * 100.0 + relLen * 60.0;
    out.size = iRound(std::max(vx.pos.size(), hy.pos.size()));
    out.scale = sc;
    return true;
}

bool pairFamilies(const Family& hy, const Family& vx, int gw, int gh, double sc,
                  BoardCand& best) {
    double dx0 = vx.step, dy0 = hy.step;
    if (dx0 <= 0 || dy0 <= 0) return false;
    double ratio = dx0 / dy0;
    bool have = false;
    for (int who = 0; who < 3; ++who) {
        Family hy2 = hy, vx2 = vx;
        if (who == 1) {   
            int m = dy0 > dx0 ? iRound(dy0 / dx0) : iRound(ratio);
            if (!(m >= 2 && m <= 4)) continue;
            if (!subsampleFamily(hy, m, hy2)) continue;
        } else if (who == 2) {
            int m = dx0 > dy0 ? iRound(dx0 / dy0) : iRound(1.0 / ratio);
            if (!(m >= 2 && m <= 4)) continue;
            if (!subsampleFamily(vx, m, vx2)) continue;
        }
        BoardCand c;
        if (pairOnce(hy2, vx2, gw, gh, sc, c))
            if (!have || c.score > best.score) { best = c; have = true; }
    }
    return have;
}

double rectOverlapRatio(const BoardCand& a, const BoardCand& b) {
    double ix = std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
    double iy = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
    if (ix <= 0 || iy <= 0) return 0.0;
    double inter = ix * iy;
    double smaller = std::min((a.x1 - a.x0) * (a.y1 - a.y0), (b.x1 - b.x0) * (b.y1 - b.y0));
    return smaller > 1e-6 ? inter / smaller : 0.0;
}

int countDistinctBoards(const std::vector<BoardCand>& tops, double scoreRatio = 0.55,
                        double overlap = 0.5) {
    if (tops.empty()) return 0;
    double bestScore = tops[0].score != 0 ? tops[0].score : 1.0;
    std::vector<BoardCand> picked;
    for (auto& c : tops) {
        if (c.score < bestScore * scoreRatio) continue;
        bool dup = false;
        for (auto& p : picked) if (rectOverlapRatio(c, p) >= overlap) { dup = true; break; }
        if (!dup) picked.push_back(c);
    }
    return (int)picked.size();
}

std::vector<BoardCand> findBoardRects(const cv::Mat& gray, int lineCount,
                                      int minLines = 8, int topn = 1,
                                      const std::vector<double>& scales = {1.0, 0.6},
                                      const std::vector<double>& lenRatios = {0.05, 0.10}) {
    int H = gray.rows, W = gray.cols;
    std::vector<BoardCand> cands;
    for (double sc : scales) {
        cv::Mat g;
        if (sc == 1.0) g = gray;
        else {
            int nw = std::max(64, (int)(W * sc)), nh = std::max(64, (int)(H * sc));
            cv::resize(gray, g, cv::Size(nw, nh), 0, 0, cv::INTER_AREA);
        }
        int gw = g.cols, gh = g.rows;
        double gshort = (double)std::min(gw, gh);
        double maxStep = gshort * 0.25;
        
        cv::Mat edges = cannyScreen(g);
        for (double lr : lenRatios) {
            double minLen = std::max(20.0, gshort * lr);
            std::vector<Cluster> hc, vc;
            collectLinesEdges(edges, minLen, hc, vc);
            if (hc.empty() || vc.empty()) continue;
            std::vector<Family> hFams = findArithmeticFamilies(hc, minLines, maxStep);
            std::vector<Family> vFams = findArithmeticFamilies(vc, minLines, maxStep);
            for (auto& hy : hFams)
                for (auto& vx : vFams) {
                    BoardCand c;
                if (pairFamilies(hy, vx, gw, gh, sc, c)) cands.push_back(c);
            }
            
            if (cands.empty() && hc.size() >= 2 && vc.size() >= 2) {
                double h0 = 1e18, h1 = -1e18, v0 = 1e18, v1 = -1e18;
                for (const auto& c : hc) { h0 = std::min(h0, c.mean); h1 = std::max(h1, c.mean); }
                for (const auto& c : vc) { v0 = std::min(v0, c.mean); v1 = std::max(v1, c.mean); }
                double dH = h1 - h0, dW = v1 - v0;
                if (dH > 0 && dW > 0 &&
                    std::abs(dH - dW) / std::max(dH, dW) < 0.18 &&
                    dW / (lineCount - 1) > 6.0) {
                    BoardCand c;
                    c.x0 = v0 / sc; c.x1 = v1 / sc; c.y0 = h0 / sc; c.y1 = h1 / sc;
                    c.step = dW / (lineCount - 1) / sc;
                    c.vx = {v0 / sc, v1 / sc};
                    c.hy = {h0 / sc, h1 / sc};
                    c.size = lineCount;
                    c.scale = sc;
                    c.score = 420.0;    
                    cands.push_back(c);
                }
            }
        }
        
        if (topn == 1 && !cands.empty()) break;
    }
    if (cands.empty()) return {};
    std::stable_sort(cands.begin(), cands.end(),
                     [](const BoardCand& a, const BoardCand& b) { return a.score > b.score; });
    int take = std::max(1, topn);
    if ((int)cands.size() > take) cands.resize(take);
    return cands;
}

struct RefineResult {
    bool ok = false;
    std::vector<double> xLines, yLines;
    double spacing = 0;
    std::string source;
    int ox = 0, oy = 0;
};

static void fullBoardSpans(const std::vector<double>& lines, int want, double step,
                           std::vector<std::pair<double, double>>& out) {
    out.clear();
    if (lines.empty() || want < 3 || step <= 0) return;
    std::vector<double> p = lines;
    std::sort(p.begin(), p.end());
    int m = (int)p.size();
    int miss = want - m;
    if (miss <= 0) {
        
        for (int k = 0; k + want <= m; ++k) out.emplace_back(p[k], p[k + want - 1]);
        return;
    }
    for (int a = 0; a <= miss; ++a) out.emplace_back(p.front() - a * step, p.back() + (miss - a) * step);
}

static double evenness(const std::vector<double>& v) {
    std::vector<double> d = diff(v);
    if (d.empty()) return 0.0;
    double m = mean(d);
    if (m <= 0) return 0.0;
    return std::min(1.0, std::max(0.0, 1.0 - stddev(d) / m));
}

static double coarseAgree(const std::vector<double>& fitted,
                          const std::vector<double>& coarse, double step) {
    if (coarse.empty() || fitted.empty()) return 0.0;
    double tol = std::max(6.0, step * 0.35);
    int hit = 0;
    for (double c : coarse) {
        double best = 1e300;
        for (double f : fitted) best = std::min(best, std::abs(f - c));
        if (best <= tol) hit++;
    }
    return (double)hit / (double)coarse.size();
}

static const double kSquareAspectMin = 0.80;

static void relayoutEven(std::vector<double>& v, double s) {
    if (v.size() < 2 || s <= 0) return;
    double c = (v.front() + v.back()) * 0.5;
    double half = (double)(v.size() - 1) * 0.5;
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = c + ((double)i - half) * s;
}

RefineResult refineGrid(const cv::Mat& rgb, const BoardCand& cand, int size) {
    RefineResult out;
    int H = rgb.rows, W = rgb.cols;
    double step = cand.step;
    double pad = step * 1.5;
    int ax0 = std::max(0, (int)(cand.x0 - pad));
    int ay0 = std::max(0, (int)(cand.y0 - pad));
    int ax1 = std::min(W, (int)(cand.x1 + pad));
    int ay1 = std::min(H, (int)(cand.y1 + pad));
    if (ax1 <= ax0 || ay1 <= ay0) return out;
    cv::Mat crop = rgb(cv::Rect(ax0, ay0, ax1 - ax0, ay1 - ay0));
    cv::Mat g;
    cv::cvtColor(crop, g, cv::COLOR_RGB2GRAY);

    std::vector<double> xs, ys;
    bool have = false;
    double spacing = step;
    std::string source = "?";
    const bool trace = getenv("GB_VISION_TRACE") != nullptr;
    if (trace) {
        fprintf(stderr, "[trace] refineGrid cand vx=%d hy=%d size=%d step=%.3f "
                        "x0=%.1f x1=%.1f y0=%.1f y1=%.1f cropwin=(%d,%d)-(%d,%d)\n",
                (int)cand.vx.size(), (int)cand.hy.size(), cand.size, step,
                cand.x0, cand.x1, cand.y0, cand.y1, ax0, ay0, ax1, ay1);
    }
    
    try {
        Geometry bg = locateBoard(crop, size);
        if (trace) {
            fprintf(stderr, "[trace] refineGrid crop=%dx%d at(%d,%d) step=%.2f locateBoard x=%d y=%d conf=%.3f src=%s\n",
                    crop.cols, crop.rows, ax0, ay0, step,
                    (int)bg.x_lines.size(), (int)bg.y_lines.size(), bg.confidence, bg.source.c_str());
        }
        if ((int)bg.x_lines.size() == size && (int)bg.y_lines.size() == size) {
            xs = bg.x_lines;
            ys = bg.y_lines;
            spacing = bg.spacing();
            source = "projection";
            have = true;
        }
    } catch (const std::exception& e) {
        if (trace) fprintf(stderr, "[trace] locateBoard threw: %s\n", e.what());
    } catch (...) {
        if (trace) fprintf(stderr, "[trace] locateBoard threw (unknown)\n");
    }
    
    if (!have) {
        std::vector<double> hx, hy;
        double res = 0;
        try {
            bool okH = locateGridHough(g, size, hx, hy, res);
            if (trace) fprintf(stderr, "[trace] hough ok=%d x=%d y=%d res=%.3f\n",
                               (int)okH, (int)hx.size(), (int)hy.size(), res);
            if (okH &&
                (int)hx.size() == size && (int)hy.size() == size) {
                xs = hx; ys = hy;
                spacing = size > 1 ? mean(diff(xs)) : spacing;
                source = "hough";
                have = true;
            }
        } catch (const std::exception& e) {
            if (trace) fprintf(stderr, "[trace] hough threw: %s\n", e.what());
        } catch (...) {
            if (trace) fprintf(stderr, "[trace] hough threw (unknown)\n");
        }
    }
    
    if (!have) {
        std::vector<std::pair<double, double>> xsp, ysp;
        fullBoardSpans(cand.vx, size, step, xsp);
        fullBoardSpans(cand.hy, size, step, ysp);
        double padG = step * 0.6;
        if (padG < step * 0.6) padG = step * 0.6;
        const int budget = 36;      
        double bestScore = -1.0;
        int tried = 0;
        for (auto& sx : xsp) {
            for (auto& sy : ysp) {
                if (++tried > budget) break;
                
                double edge = step * 0.5;
                if (sx.first < edge || sy.first < edge ||
                    sx.second > W - 1 - edge || sy.second > H - 1 - edge) continue;
                int wx0 = std::max(0, (int)std::floor(sx.first - padG));
                int wy0 = std::max(0, (int)std::floor(sy.first - padG));
                int wx1 = std::min(W - 1, (int)std::ceil(sx.second + padG));
                int wy1 = std::min(H - 1, (int)std::ceil(sy.second + padG));
                if (wx1 - wx0 + 1 < size * 4 || wy1 - wy0 + 1 < size * 4) continue;
                for (int lock = 0; lock < 2; ++lock) {
                    Geometry bg;
                    try {
                        bg = geometryFromRect(rgb, wx0, wy0, wx1, wy1, size,
                                              "anchor", lock == 1);
                    } catch (...) { continue; }
                    if ((int)bg.x_lines.size() != size || (int)bg.y_lines.size() != size) continue;
                    double xSpan = bg.x_lines.back() - bg.x_lines.front();
                    double ySpan = bg.y_lines.back() - bg.y_lines.front();
                    if (xSpan <= 0 || ySpan <= 0) continue;
                    double aspect = std::min(xSpan / ySpan, ySpan / xSpan);
                    double regularity = std::min(evenness(bg.x_lines), evenness(bg.y_lines));
                    double extent = std::min(xSpan / std::max(wx1 - wx0, 1),
                                             ySpan / std::max(wy1 - wy0, 1));
                    double agX = coarseAgree(bg.x_lines, cand.vx, step);
                    double agY = coarseAgree(bg.y_lines, cand.hy, step);
                    if (aspect < 0.84 || regularity < 0.42 || agX < 0.70 || agY < 0.70) continue;
                    
                    double score = 0.70 * bg.confidence + 0.20 * regularity + 0.10 * extent
                                 + 0.05 * std::min(agX, agY);
                    if (trace)
                        fprintf(stderr, "[trace]   anchor win=(%d,%d)-(%d,%d)%s aspect=%.3f "
                                        "reg=%.3f ext=%.3f agree=%.2f/%.2f conf=%.3f score=%.3f\n",
                                wx0, wy0, wx1, wy1, lock ? " lock" : "",
                                aspect, regularity, extent, agX, agY, bg.confidence, score);
                    if (score > bestScore) {
                        bestScore = score;
                        
                        xs.resize(bg.x_lines.size());
                        for (size_t i = 0; i < xs.size(); ++i) xs[i] = bg.x_lines[i] - ax0;
                        ys.resize(bg.y_lines.size());
                        for (size_t i = 0; i < ys.size(); ++i) ys[i] = bg.y_lines[i] - ay0;
                        spacing = bg.spacing();
                        source = lock ? "anchor-lock" : "anchor";
                        have = true;
                    }
                }
            }
        }
        if (trace) fprintf(stderr, "[trace] ③ anchor windows=%d bestScore=%.3f\n",
                           tried, bestScore);
    }
    if (!have) {
        if (trace) fprintf(stderr, "[trace] refineGrid FAILED (no %d-line fit)\n", size);
        return out;
    }
    
    if (size > 1 && (int)xs.size() == size && (int)ys.size() == size) {
        double sx = mean(diff(xs));
        double sy = mean(diff(ys));
        if (sx <= 0 || sy <= 0) {
            if (trace) fprintf(stderr, "[trace] refineGrid reject: 步长非正 sx=%.3f sy=%.3f\n", sx, sy);
            return out;
        }
        double aspect = std::min(sx, sy) / std::max(sx, sy);      
        if (aspect < kSquareAspectMin) {
            if (trace)
                fprintf(stderr, "[trace] refineGrid reject: 两轴步长差过大 sx=%.3f sy=%.3f "
                                "aspect=%.3f < %.2f（不是同一个方形棋盘）\n",
                        sx, sy, aspect, kSquareAspectMin);
            return out;
        }
        if (std::fabs(sx - sy) > 0.5) {                          
            double s;
            if (step > 0) {
                bool keepX = std::fabs(sx - step) <= std::fabs(sy - step);
                s = keepX ? sx : sy;
                relayoutEven(keepX ? ys : xs, s);                
                if (trace)
                    fprintf(stderr, "[trace] refineGrid square-snap: sx=%.3f sy=%.3f -> s=%.3f "
                                    "keep=%s relaid=%s\n",
                            sx, sy, s, keepX ? "x" : "y", keepX ? "y" : "x");
            } else {
                s = std::sqrt(sx * sy);                          
                relayoutEven(xs, s);
                relayoutEven(ys, s);
                if (trace)
                    fprintf(stderr, "[trace] refineGrid square-snap(no-step): sx=%.3f sy=%.3f -> s=%.3f both\n",
                            sx, sy, s);
            }
            spacing = s;
            source += "+square";
        } else {
            spacing = (sx + sy) * 0.5;
        }
    }
    out.ok = true;
    out.xLines.resize(xs.size());
    for (size_t i = 0; i < xs.size(); ++i) out.xLines[i] = xs[i] + ax0;
    out.yLines.resize(ys.size());
    for (size_t i = 0; i < ys.size(); ++i) out.yLines[i] = ys[i] + ay0;
    out.spacing = spacing;
    out.source = source;
    out.ox = ax0;
    out.oy = ay0;
    return out;
}

bool searchNear(const cv::Mat& gray, const ScanHint& hint, int size, BoardCand& out,
                double expand = 0.35) {
    int H = gray.rows, W = gray.cols;
    if (hint.w <= 0 || hint.h <= 0) return false;
    double ex = hint.w * expand, ey = hint.h * expand;
    int x0 = std::max(0, (int)(hint.x - ex)), y0 = std::max(0, (int)(hint.y - ey));
    int x1 = std::min(W, (int)(hint.x + hint.w + ex));
    int y1 = std::min(H, (int)(hint.y + hint.h + ey));
    if (x1 - x0 < 40 || y1 - y0 < 40) return false;
    cv::Mat sub = gray(cv::Rect(x0, y0, x1 - x0, y1 - y0));
    std::vector<BoardCand> c = findBoardRects(sub, size);
    if (c.empty()) return false;
    BoardCand cand = c[0];
    cand.x0 += x0; cand.y0 += y0; cand.x1 += x0; cand.y1 += y0;
    for (double& v : cand.vx) v += x0;
    for (double& v : cand.hy) v += y0;
    out = cand;
    return true;
}

void cropBoard(const cv::Mat& rgb, const std::vector<double>& xLines,
               const std::vector<double>& yLines, double spacing,
               cv::Mat& crop, int& ox, int& oy, double padMul = 1.0) {
    int H = rgb.rows, W = rgb.cols;
    double pad = (spacing * 0.6) * padMul;   
    if (pad < spacing * 0.6) pad = spacing * 0.6;          
    int x0 = std::max(0, (int)(xLines.front() - pad));
    int x1 = std::min(W, (int)(xLines.back() + pad) + 1);
    int y0 = std::max(0, (int)(yLines.front() - pad));
    int y1 = std::min(H, (int)(yLines.back() + pad) + 1);
    crop = rgb(cv::Rect(x0, y0, x1 - x0, y1 - y0));
    ox = x0;
    oy = y0;
}

void maskExcluded(cv::Mat& rgb, const std::vector<ExcludeRect>& exclude, int offX, int offY) {
    if (exclude.empty()) return;
    std::vector<double> samples[3];
    for (int r = 0; r < rgb.rows; r += 16)
        for (int c = 0; c < rgb.cols; c += 16) {
            const uint8_t* p = rgb.ptr<uint8_t>(r) + c * 3;
            for (int k = 0; k < 3; ++k) samples[k].push_back((double)p[k]);
        }
    double fill[3] = {128, 128, 128};
    for (int k = 0; k < 3; ++k)
        if (!samples[k].empty()) fill[k] = median(samples[k]);

    for (const ExcludeRect& rr : exclude) {
        int x = rr.x - offX, y = rr.y - offY;
        int w = rr.w, h = rr.h;
        if (w <= 0 || h <= 0) continue;
        int x0 = std::max(0, x), y0 = std::max(0, y);
        int x1 = std::min(rgb.cols, x + w), y1 = std::min(rgb.rows, y + h);
        if (x1 <= x0 || y1 <= y0) continue;
        for (int r = y0; r < y1; ++r) {
            uint8_t* p = rgb.ptr<uint8_t>(r);
            for (int c = x0; c < x1; ++c) {
                p[c * 3 + 0] = (uint8_t)fill[0];
                p[c * 3 + 1] = (uint8_t)fill[1];
                p[c * 3 + 2] = (uint8_t)fill[2];
            }
        }
    }
}

Json diagKV(const std::string& k, const std::string& v) {
    Json j = Json::makeObj();
    j.set(k, Json::makeStr(v));
    return j;
}

Json numD(double v) { return Json::makeNum(v); }

void shiftJsonNum(Json& container, const std::string& key, double d) {
    Json* p = container.findMutable(key);
    if (p && p->type == Json::NUM) {
        if (p->isInt) { p->isInt = false; p->num = (double)p->inum + d; }
        else p->num += d;
    }
}

void shiftJsonArr(Json& container, const std::string& key, double d) {
    Json* p = container.findMutable(key);
    if (!p || p->type != Json::ARR) return;
    for (Json& v : p->arr)
        if (v.type == Json::NUM) {
            if (v.isInt) { v.isInt = false; v.num = (double)v.inum + d; }
            else v.num += d;
        }
}

struct StabGeo {
    std::vector<double> xl, yl;
    double sp = 0.0;
    bool has = false;
    double shiftX = 0.0, shiftY = 0.0;
    int age = 0;

    bool Adopt(std::vector<double>& xLines, std::vector<double>& yLines, double& spacing) {
        if (!has || (int)xLines.size() != (int)xl.size() || (int)yLines.size() != (int)yl.size()) {
            xl = xLines; yl = yLines; sp = spacing; has = true; shiftX = shiftY = 0.0; age = 1;
            return false;
        }
        double sx = 0, sy = 0, ssp = 0;
        for (size_t i = 0; i < xLines.size(); ++i) sx += std::abs(xLines[i] - xl[i]);
        for (size_t i = 0; i < yLines.size(); ++i) sy += std::abs(yLines[i] - yl[i]);
        sx /= xLines.size(); sy /= yLines.size();
        ssp = std::abs(spacing - sp);
        
        const double SMALL = 1.6;                      
        const double BIGPIX = 8.0;                     
        const double BIGSP = std::max(0.6, sp * 0.03); 
        if (sx <= SMALL && sy <= SMALL && ssp <= 0.4) {
            ++age;
            xLines = xl; yLines = yl; spacing = sp;    
            return true;
        }
        if (sx >= BIGPIX || sy >= BIGPIX || ssp >= BIGSP) {
            xl = xLines; yl = yLines; sp = spacing;    
            shiftX = shiftY = 0.0; age = 1;
            return false;
        }
        shiftX += sx; shiftY += sy;
        if (shiftX >= 5.0 || shiftY >= 5.0) {          
            xl = xLines; yl = yLines; sp = spacing;
            shiftX = shiftY = 0.0; age = 1;
            return false;
        }
        xLines = xl; yLines = yl; spacing = sp;        
        return true;
    }
};

// ★ 2026-10-18：指纹缓存提到文件作用域 —— 全新定位成功后要**主动播种**，
//   否则 stab 复用分支第一次询问时 prevFp 还是空的，必然判「窗口变了」⇒ 多跑一次全屏定位，
//   两级融合要多等一帧才切到同步读子（真机上就是多一次 ~200ms 的全搜）。
static cv::Mat g_boardFp;

static void boardFpStore(const cv::Mat& rgb, const std::vector<double>& xL,
                         const std::vector<double>& yL, double spacing) {
    if (xL.size() < 2 || yL.size() < 2 || spacing <= 0 || rgb.empty()) return;
    int x0 = std::max(0, (int)std::floor(xL.front() - spacing * 0.6));
    int y0 = std::max(0, (int)std::floor(yL.front() - spacing * 0.6));
    int x1 = std::min(rgb.cols - 1, (int)std::ceil(xL.back() + spacing * 0.6));
    int y1 = std::min(rgb.rows - 1, (int)std::ceil(yL.back() + spacing * 0.6));
    if (x1 <= x0 || y1 <= y0) return;
    cv::Mat gw, sm;
    cv::cvtColor(rgb(cv::Rect(x0, y0, x1 - x0, y1 - y0)), gw, cv::COLOR_RGB2GRAY);
    cv::resize(gw, sm, cv::Size(32, 32), 0, 0, cv::INTER_AREA);
    sm.convertTo(sm, CV_32F);
    g_boardFp = sm.clone();
}

static bool boardWindowChanged(const cv::Mat& rgb, const std::vector<double>& xL,
                               const std::vector<double>& yL, double spacing) {
    if (xL.size() < 2 || yL.size() < 2 || spacing <= 0) return true;
    int x0 = std::max(0, (int)std::floor(xL.front() - spacing * 0.6));
    int y0 = std::max(0, (int)std::floor(yL.front() - spacing * 0.6));
    int x1 = std::min(rgb.cols - 1, (int)std::ceil(xL.back() + spacing * 0.6));
    int y1 = std::min(rgb.rows - 1, (int)std::ceil(yL.back() + spacing * 0.6));
    if (x1 <= x0 || y1 <= y0) return true;
    
    cv::Mat gw, sm;
    cv::cvtColor(rgb(cv::Rect(x0, y0, x1 - x0, y1 - y0)), gw, cv::COLOR_RGB2GRAY);
    cv::resize(gw, sm, cv::Size(32, 32), 0, 0, cv::INTER_AREA);
    sm.convertTo(sm, CV_32F);
    double d = 1e9;
    if (!g_boardFp.empty() && g_boardFp.size() == sm.size()) {
        double s = 0;
        const float* a = sm.ptr<float>(0);
        const float* b = g_boardFp.ptr<float>(0);
        int cnt = sm.rows * sm.cols;
        for (int i = 0; i < cnt; ++i) s += std::fabs((double)a[i] - (double)b[i]);
        d = s / (double)cnt;
    }
    g_boardFp = sm.clone();
    
    return d > 2.2;
}

ScanResult scanSub(const cv::Mat& rgbIn, int size, const ScanHint& hint,
                   const std::vector<ExcludeRect>& exclude, bool probe,
                   double offX, double offY, const LockedGeo* locked,
                   const SyncRecogParams* sync, bool forceSync);

ScanResult notFound(double offX, double offY, int multi, const std::string& reason) {
    ScanResult r;
    r.ok = true;
    r.found = false;
    r.ox = offX;
    r.oy = offY;
    r.multiBoard = multi;
    r.diag = diagKV("reason", reason);
    return r;
}

// ★★ 2026-10-19：读子结果「像不像一盘棋」的粗判据（只用于识图单帧的兜底决策）。
//   broken = 明显坏了：全空 / 只有一色 / 子数超过棋盘 60% / 黑白差 > 6 / suspect。
//   sane   = 至少有子、没超限、不 suspect（不强求黑白平衡 —— VC 题面可以只有一色）。
static bool boardLooksBroken(int b, int w, int n, bool suspect) {
    if (suspect) return true;
    if (b + w == 0) return true;
    if (b == 0 || w == 0) return true;
    if (b + w > (int)(n * n * 3 / 5)) return true;
    if (std::abs(b - w) > 6) return true;
    return false;
}
static bool boardLooksSane(int b, int w, int n, bool suspect) {
    if (suspect) return false;
    if (b + w == 0) return false;
    if (b + w > (int)(n * n * 3 / 5)) return false;
    return true;
}
static void countBoard(const cv::Mat& board, int& b, int& w) {
    b = 0; w = 0;
    for (int r = 0; r < board.rows; ++r)
        for (int c = 0; c < board.cols; ++c) {
            const int8_t v = board.at<int8_t>(r, c);
            if (v == (int8_t)BLACK) ++b;
            else if (v == (int8_t)WHITE) ++w;
        }
}

// ★ 2026-10-19：整行/整列 15 格**同色**（且非空）在五子棋里不可能（连五就终局了）——
//   出现就说明这条边线落在棋盘**外面**（截图的 UI 留白 / 桌面背景被当成第 15 条线），
//   也就是「网格整体错位一格」。返回需要平移的方向（单位 = 一格）。
static bool uniformEdgeShift(const cv::Mat& b, int& dx, int& dy) {
    const int n = b.rows;
    if (n < 5) return false;
    auto rowSame = [&](int r) {
        const int8_t v0 = b.at<int8_t>(r, 0);
        if (v0 == (int8_t)EMPTY) return false;
        for (int c = 1; c < n; ++c) if (b.at<int8_t>(r, c) != v0) return false;
        return true;
    };
    auto colSame = [&](int c) {
        const int8_t v0 = b.at<int8_t>(0, c);
        if (v0 == (int8_t)EMPTY) return false;
        for (int r = 1; r < n; ++r) if (b.at<int8_t>(r, c) != v0) return false;
        return true;
    };
    dx = 0; dy = 0;
    if (rowSame(n - 1)) { dy = -1; return true; }
    if (rowSame(0)) { dy = 1; return true; }
    if (colSame(n - 1)) { dx = -1; return true; }
    if (colSame(0)) { dx = 1; return true; }
    return false;
}


ScanResult finishWithGeometry(const cv::Mat& masked, int size,
                              const std::vector<double>& xL,
                              const std::vector<double>& yL,
                              double spacing, int multi,
                              double offX, double offY,
                              long long* readMsOut = nullptr,
                              bool livenessGate = false,
                              const SyncRecogParams* sync = nullptr,
                              bool forceSync = false) {
    const bool perf = getenv("GB_VISION_PERF") != nullptr;
    const long long _t3 = perf ? (long long)(cv::getTickCount() * 1000 / cv::getTickFrequency()) : 0;

    cv::Mat crop;
    int cox = 0, coy = 0;
    cropBoard(masked, xL, yL, spacing, crop, cox, coy);

    GeoLike g;
    g.size = size;
    g.spacing = spacing;
    g.x_lines.resize(xL.size());
    for (size_t i = 0; i < xL.size(); ++i) g.x_lines[i] = xL[i] - cox;
    g.y_lines.resize(yL.size());
    for (size_t i = 0; i < yL.size(); ++i) g.y_lines[i] = yL[i] - coy;

    if (livenessGate) {
        Geometry chk;
        chk.x_lines = g.x_lines;
        chk.y_lines = g.y_lines;
        double live = gridLiveness(crop, chk);
        // ★★ 2026-10-13：闸门恢复桌面版助手（10-09）的 0.25 —— 10-12 那次优化把它放宽到
        //   0.18，等于允许「几何已经不太对」的缓存几何继续产出落子坐标（错位棋子会漏读/误读）。
        //   0.25 是旧口径：只要缓存网格在当前帧里活得不够好，就判 false → 上层全屏重定位。
        if (live < 0.25) {
            ScanResult r = notFound(offX, offY, 0, "locked-grid-dead");
            r.diag.set("liveness", Json::makeNum(live));
            return r;
        }
    }

    if (const char* dp = getenv("GB_VISION_DUMP_CROP")) {
        cv::Mat bgr;
        cv::cvtColor(crop, bgr, cv::COLOR_RGB2BGR);
        cv::imwrite(dp, bgr);
        fprintf(stderr, "[dump] crop %dx%d ox=%d oy=%d spacing=%.3f x0=%.3f y0=%.3f\n",
                crop.cols, crop.rows, cox, coy, spacing,
                xL.front(), yL.front());
    }

    cv::Mat board, conf;
    Json diag;

    // ★ 2026-10-19：识图单帧额外量一次「网格活得怎么样」（几何是否真的对上了线）。
    //   旋转/透视/被 UI 遮挡的图，轴线采样会明显偏低 —— 这是把「几何对了但线没对上」
    //   和「几何压根不对」区分开的唯一廉价信号。
    //   注意：读子函数会**重建** diag，所以要先把值存局部变量，最后再写回 diag。
    double liveForce = -1.0;
    if (forceSync && !livenessGate) {
        Geometry chk;
        chk.x_lines = g.x_lines;
        chk.y_lines = g.y_lines;
        liveForce = gridLiveness(crop, chk);
    }

    // ★★ 2026-10-18 两级融合（用户口径：「初始先用我们的代码定位棋盘位置、自动选棋盘、识别棋盘，
    //   这个做好之后，自动转换到人家的棋子识别层」）：
    //
    //   ① 第一级 = **我们**：全新定位（livenessGate=false，即本帧刚跑完 findBoardRects +
    //      refineGrid）时读子仍走自制的 readStonesAdaptive —— 首次拿到的棋盘必须是自家标定过的，
    //      不把两套判别器混在同一帧里。
    //   ② 第二级 = **对方的**：几何来自宿主锁定（上一帧锁定）或 stab 复用（livenessGate=true）⇒
    //      棋盘已经被我们定位并稳定住，这时自动切到 readStonesSync（逐格判定 + 变化识别）。
    //      交接点是 scanSub 里 `stab.Adopt(...)`：全新定位成功那一帧写入 stab，下一帧就进复用分支。
    //  ③ 兜底：同步层若把整盘判成同一色（swamped，即 suspect）视为不可信 —— 这一帧退回自制读子，
    //      不让一次坏读数把整盘喂给引擎。
    // ★ 2026-10-19：forceSync（识图单帧）同样走 sync —— 单帧没有「上一帧锁定几何」可等，若不放行
    //   就永远只用自制读子，那 5 项识别阈值对识图就是死参数。
    const bool useSync = (sync != nullptr) && (livenessGate || forceSync);
    if (useSync) {
        // ★ 2026-10-19：forceSync（识图单帧）允许「整盘读空 → 松阈值重判」，见 gbsync.cpp。
        readStonesSync(crop, g, *sync, board, diag, /*allowRelax*/forceSync);
        bool swamped = (diag.find("suspect") && diag.find("suspect")->b);
        auto countStones = [](const cv::Mat& b) {
            int c = 0;
            for (int r = 0; r < b.rows; ++r)
                for (int k = 0; k < b.cols; ++k)
                    if (b.at<int8_t>(r, k) != (int8_t)EMPTY) ++c;
            return c;
        };
        const int cntSync = countStones(board);
        // ★★ 2026-10-19（用户：「只识别了棋盘，常常不会自动识别棋子」）：
        //   识图单帧读到「0 颗子」时，再让**自制读子**读一遍，谁读出的子多用谁。
        //   松阈值重判（gbsync 里）解决「棋子比格子小」这一类；自制读子解决「阈值怎么调都
        //   读不出来」的另一类。两条都只在 forceSync（识图单帧）触发，连线稳态不受影响。
        const bool emptyFrame = (cntSync == 0);
        if (swamped || (emptyFrame && forceSync)) {
            cv::Mat boardA, confA;
            Json diagA;
            readStonesAdaptive(crop, g, boardA, confA, diagA);
            const int cntA = countStones(boardA);
            if (swamped || cntA > cntSync) {
                board = boardA;
                diag = diagA;
                diag.set("sync_fallback", Json::makeStr(swamped ? "swamped" : "empty"));
                if (cntA > cntSync) diag.set("reader", Json::makeStr("self"));
            } else {
                diag.set("sync_fallback_tried", Json::makeBool(true));
                diag.set("reader", Json::makeStr("sync"));
            }
        } else {
            diag.set("reader", Json::makeStr("sync"));
        }
    } else {
    readStonesAdaptive(crop, g, board, conf, diag);
    if (sync) diag.set("reader", Json::makeStr("self"));   // 第一级：自家定位+读子

    {
        bool sus0 = (diag.find("suspect") && diag.find("suspect")->b);
        if (sus0) {
            cv::Mat crop2;
            int cx2 = 0, cy2 = 0;
            cropBoard(masked, xL, yL, spacing, crop2, cx2, cy2, 2.0);
            if (crop2.cols > crop.cols || crop2.rows > crop.rows) {
                GeoLike g2;
                g2.size = size;
                g2.spacing = spacing;
                g2.x_lines.resize(xL.size());
                for (size_t i = 0; i < xL.size(); ++i) g2.x_lines[i] = xL[i] - cx2;
                g2.y_lines.resize(yL.size());
                for (size_t i = 0; i < yL.size(); ++i) g2.y_lines[i] = yL[i] - cy2;
                cv::Mat board2, conf2;
                Json diag2;
                readStonesAdaptive(crop2, g2, board2, conf2, diag2);
                bool sus2 = (diag2.find("suspect") && diag2.find("suspect")->b);
                if (!sus2) {
                    crop = crop2; cox = cx2; coy = cy2;
                    g = g2; board = board2; conf = conf2; diag = diag2;
                    diag.set("wide_crop_retry", Json::makeBool(true));
                }
            }
        }
    }
    }

    // ★★ 2026-10-19（用户：「识图不太智能，明明能识别却没识别出来，优化图片适应性」）：
    //   第三层兜底 —— **局部对比读子**（readStonesLocal）。
    //   走到这里的结果如果「明显不像一盘棋」（全空 / 只有一色 / 子数超 60% / 黑白差 > 6 /
    //   suspect），就再让局部对比读一遍；只有它读得像样时才接管。
    //   典型能救回来的场景：
    //     · 深色主题盘：黑子只比底色暗一点，绝对阈值（≤96）判不出来 ⇒ 整盘只剩白子；
    //     · 浅色木盘：空格底色本身就 ≥159，白子判定把**空盘**读成满盘白（W=200+）；
    //     · 截图缩放/坐标边距导致黑白子占比都偏低。
    //   只在识图单帧（forceSync）启用，连线稳态不受影响（每帧多跑一次采样不划算）。
    if (forceSync) {
        int b0 = 0, w0 = 0;
        countBoard(board, b0, w0);
        const bool sus0b = (diag.find("suspect") && diag.find("suspect")->b);
        if (boardLooksBroken(b0, w0, size, sus0b)) {
            cv::Mat boardL;
            Json diagL;
            readStonesLocal(crop, g, boardL, diagL);
            int b1 = 0, w1 = 0;
            countBoard(boardL, b1, w1);
            const bool sus1 = (diagL.find("suspect") && diagL.find("suspect")->b);
            if (boardLooksSane(b1, w1, size, sus1) &&
                !boardLooksSane(b0, w0, size, sus0b)) {
                board = boardL;
                diag = diagL;
                diag.set("local_takeover", Json::makeBool(true));
                diag.set("prev_black", Json::makeInt(b0));
                diag.set("prev_white", Json::makeInt(w0));
            } else {
                diag.set("local_checked", Json::makeBool(true));
                diag.set("local_b", Json::makeInt(b1));
                diag.set("local_w", Json::makeInt(w1));
            }
        }
        if (liveForce >= 0.0 && diag.type == Json::OBJ)
            diag.set("liveness", Json::makeNum(liveForce));

        // ★★ 2026-10-19：网格错位一格的自愈（见 uniformEdgeShift 注释）。
        //   截图里棋盘外面有留白/UI 时，第 15 条线经常被定到棋盘外 —— 表现为最外一行/列
        //   整排同色（实测：一张 900px 的截图上整行 15 颗白子 = 把页面底色当成了棋子）。
        //   把网格整体平移一格重读，只要平移后不再有「整行同色」就采用。
        if (sync) {
            int sdx = 0, sdy = 0;
            if (uniformEdgeShift(board, sdx, sdy)) {
                GeoLike g2 = g;
                for (size_t t = 0; t < g2.x_lines.size(); ++t) g2.x_lines[t] += sdx * g.spacing;
                for (size_t t = 0; t < g2.y_lines.size(); ++t) g2.y_lines[t] += sdy * g.spacing;
                cv::Mat b2; Json d2;
                readStonesSync(crop, g2, *sync, b2, d2, /*allowRelax*/false);
                int ux = 0, uy = 0;
                if (!uniformEdgeShift(b2, ux, uy)) {
                    int nb2 = 0, nw2 = 0;
                    countBoard(b2, nb2, nw2);
                    if (nb2 + nw2 > 0) {
                        board = b2;
                        diag = d2;
                        diag.set("grid_shift", Json::makeStr(std::to_string(sdx) + "," + std::to_string(sdy)));
                    }
                }
            }
        }
    }
    if (perf && readMsOut)
        *readMsOut = (long long)(cv::getTickCount() * 1000 / cv::getTickFrequency()) - _t3;

    ScanResult res;
    res.ok = true;
    res.found = true;
    res.ox = offX;
    res.oy = offY;
    res.suspect = diag.find("suspect") && diag.find("suspect")->b;
    res.multiBoard = multi;
    res.diag = diag;

    int n = board.rows;
    for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c) {
            int8_t v = board.at<int8_t>(r, c);
            if (v == BLACK) res.black.emplace_back(c, r);
            else if (v == WHITE) res.white.emplace_back(c, r);
        }

    Json br = Json::makeObj();
    br.set("x", Json::makeNum(xL.front()));
    br.set("y", Json::makeNum(yL.front()));
    br.set("w", Json::makeNum(xL.back() - xL.front()));
    br.set("h", Json::makeNum(yL.back() - yL.front()));
    res.boardRect = br;

    Json gj = Json::makeObj();
    Json xa = Json::makeArr(), ya = Json::makeArr();
    for (double v : xL) xa.push(numD(v));
    for (double v : yL) ya.push(numD(v));
    gj.set("x_lines", xa);
    gj.set("y_lines", ya);
    gj.set("spacing", Json::makeNum(spacing));
    gj.set("size", Json::makeInt(size));
    res.geometry = gj;
    res.crop = crop;
    return res;
}

ScanResult scanSub(const cv::Mat& rgbIn, int size, const ScanHint& hint,
                   const std::vector<ExcludeRect>& exclude, bool probe,
                   double offX, double offY, const LockedGeo* locked,
                   const SyncRecogParams* sync, bool forceSync) {
    
    const bool perf = getenv("GB_VISION_PERF") != nullptr;
    long long tPrepMs = 0, tFindMs = 0, tRefineMs = 0, tReadMs = 0;
  const long long _t0 = perf ? (long long)(cv::getTickCount() * 1000 / cv::getTickFrequency()) : 0;
  
  cv::Mat masked = rgbIn.clone();
  maskExcluded(masked, exclude, (int)offX, (int)offY);

  // ★ 2026-10-14（用户：「识别棋子前期非常快，随着游戏时间变长就变慢了，是不是管道堆积？」）
  //   实测定位（真实运行日志 %TEMP%\GomokuTrainer.log + 管道基准 tools/_bench_pipe.cpp）：
  //   不是管道堆积 —— 管道/shm/outbox 全部有界且平稳（300 连发中位 30~54ms 无增长）。
  //   真凶是 **suspect 帧的重复全搜**：几何锁定/缓存命中时读子本只要 ~50-150ms，但读出
  //   suspect（棋子越多越容易：贴边/遮挡/低置信）就会 ①2x 裁剪重读 ②stab 缓存重读
  //   ③再跌落到 findBoardRects+refineGrid+第三次读 —— 同样的像素一遍遍搜，单 tick 从
  //   ~0.3s 涨到 0.5-1s（p90 4.7s，叠加 auto 全屏重搜窗口最长 10.7s），正好是
  //   「越往后越慢」。修法：几何还活着时，suspect 帧的**全区域重搜限流 1.2s 一次**，
  //   窗口内直接把 suspect 结果交还宿主（页面本就有 suspectTip 处理，行为不变、只去掉
  //   无谓的重复搜索）；liveness 判死（几何失效）不受限流，照常重定位。
  //   （2026-10-14 11:19 曾按用户方案试过「锁定态 suspect 一律早退、不自动全搜」，
  //    真机反馈反而更慢 —— 锁定几何一旦失真，suspect 垃圾局面每帧刷新导致页面/引擎
  //    反复重算；已整体回退到本限流版，回退见 git-less 项目日志。）
  ScanResult suspectHold;
  bool haveSuspectHold = false;

  if (locked && locked->has && !probe &&
      (int)locked->xLines.size() == size && (int)locked->yLines.size() == size &&
      locked->spacing > 0) {
      long long readMs = 0;
      ScanResult r = finishWithGeometry(masked, size, locked->xLines, locked->yLines,
                                        locked->spacing, 0, offX, offY, &readMs,
                                        true, sync, forceSync);
      if (perf)
          fprintf(stderr, "[perf] locked scan size=%d read=%lldms suspect=%d\n",
                  size, readMs, (int)r.suspect);
      if (r.found && !r.suspect) return r;
      if (r.found) { suspectHold = r; haveSuspectHold = true; }
  }

  static StabGeo stab;
  // ★★ 2026-10-13（用户：「当前棋盘识别的准度较低」＋「自动框选棋盘的逻辑，学习桌面版本
  //   助手的参数和算法」）：
  //   10-12 21:10 那次「帧差复用」性能优化（280ms → 118ms）把**棋盘是否移动的判定整个
  //   删掉了** —— 只剩 `stab.sp > 0` 就直接用上一帧的几何去读子，并且把 liveness 闸门
  //   从 0.25 放宽到 0.18。后果：棋盘被拖动 / 窗口被缩放 / 页面滚动 / 换平台切换边框时，
  //   仍然拿**过期的线位置**去读子 ⇒ 整盘错位，这就是「准度较低」。
  //   桌面版助手（Desktop version/，10-09 那份）用的就是**带 boardWindowChanged 判定**的
  //   老参数，所以这里按它的口径恢复：几何指纹变了（平均灰度差 > 2.2）⇒ 老老实实全屏重定位。
  //   （一颗棋子只占窗口 ~0.2% 面积，指纹差约 0.3 ≪ 2.2，所以正常落子**不会**触发重扫，
  //    复用的加速仍然保留；只有棋盘真的动了才重扫。）
  // ★ 2026-10-18 修订：几何复用不再对 sync 关闭 —— sync 现在只替换「棋子判定」，
  //   棋盘定位仍走自制逻辑，所以这条缓存几何复用对 sync 同样安全且必要（否则每帧都要
  //   重新找盘，正是「算得快但不准/棋盘不方正」的来源）。
  if (!probe && stab.has && (int)stab.xl.size() == size && (int)stab.yl.size() == size &&
      stab.sp > 0 && !boardWindowChanged(masked, stab.xl, stab.yl, stab.sp)) {
      long long readMs = 0;
      ScanResult r = finishWithGeometry(masked, size, stab.xl, stab.yl, stab.sp,
                                        0, offX, offY, &readMs, true, sync, forceSync);
      if (perf)
          fprintf(stderr, "[perf] scan reuse cached geo read=%lldms suspect=%d (no find/refine)\n",
                  readMs, (int)r.suspect);
      if (r.found && !r.suspect) return r;
      if (r.found) { suspectHold = r; haveSuspectHold = true; }
      if (perf) fprintf(stderr, "[perf] scan reuse fell back (found=%d suspect=%d)\n",
                        (int)r.found, (int)r.suspect);
  }

  if (haveSuspectHold) {
      // ★ 窗口取 2500ms —— 必须**大于宿主扫描循环的 2s 稳态周期**（watch 事件触发 +
      //   2000ms 兜底），否则稳态 suspect 风暴下每个 tick 都重新放行全搜，限流形同虚设。
      //   代价：真正的网格漂移恢复最多延迟一个窗口（几何彻底失效仍由 liveness 闸门兜底，
      //   不受本限流约束，立即全屏重定位）。
      static DWORD s_lastGeoFullRetry = 0;
      DWORD nowMs = GetTickCount();
      bool geoAlive = (locked && locked->has && (int)locked->xLines.size() == size &&
                       (int)locked->yLines.size() == size && locked->spacing > 0) ||
                      (stab.has && (int)stab.xl.size() == size && stab.sp > 0);
      if (geoAlive && nowMs - s_lastGeoFullRetry < 2500)
          return suspectHold;            // ★ 限流窗口内：直接报 suspect，不做全区域重搜
      s_lastGeoFullRetry = nowMs;        // 窗口外：放行一次全搜并刷新时钟
  }

  // ★★ 2026-10-18 修订（用户：「定位只走自制，禁用 sync 的 Hough 找盘」）：
  //   连线识别维持混合口径 —— 棋盘定位一律走自制的 findBoardRects + refineGrid（方正、边框稳），
  //   只有「棋子黑白判定」用同步算法的 syncCellInfo。同步工具的 locateBoardSync（HoughLinesP
  //   独立求 gapV/gapH）**不再接入连线识别路径**：它会把已方正的网格改成非正方形，并让找盘
  //   多跑一遍 Hough，正是「棋盘不是方的、识别还慢」的来源。函数本体保留（未接线），便于回退。
  cv::Mat gray;
    cv::cvtColor(masked, gray, cv::COLOR_RGB2GRAY);
    if (perf) {
        tPrepMs = (long long)(cv::getTickCount() * 1000 / cv::getTickFrequency()) - _t0;
    }

    bool haveCand = false;
    BoardCand cand;
    int multi = 0;
    if (probe) {
        
        std::vector<BoardCand> tops = findBoardRects(gray, size, 8, 6);
        if (!tops.empty()) {
            cand = tops[0];
            haveCand = true;
            multi = countDistinctBoards(tops);
        }
    }
    const long long _t1 = perf ? (long long)(cv::getTickCount() * 1000 / cv::getTickFrequency()) : 0;
    // ★★ 2026-10-19（用户：「识图不太智能，明明能识别却没识别出来，优化图片适应性」）：
    //   找盘 / 定型从「只试第一个候选」升级为**阶梯重试**：
    //     车道 1：原图灰度，topn=4 逐个 refine（以前只 refine 第 1 个候选，错一个整帧就废）
    //     车道 2：3×3 高斯（压 JPEG 块效应 / 相机噪点）
    //     车道 3：CLAHE 均衡（压不均匀光照 / 低对比）
    //     车道 4：1.5× 放大（细线、棋盘在整张图里很小）
    //   只有前面车道失败才往下走 —— 正常图仍是原来的耗时。
    bool haveGeo = false;
    RefineResult geo;
    int lane = 0;
    auto attempt = [&](const cv::Mat& gimg, const cv::Mat& rgbimg, double sc,
                       int minLines, int topn) -> bool {
        std::vector<BoardCand> cs = findBoardRects(gimg, size, minLines, topn);
        for (size_t k = 0; k < cs.size(); ++k) {
            RefineResult r = refineGrid(rgbimg, cs[k], size);
            if (!r.ok) continue;
            cand = cs[k];
            if (sc != 1.0) {
                for (size_t t = 0; t < r.xLines.size(); ++t) r.xLines[t] /= sc;
                for (size_t t = 0; t < r.yLines.size(); ++t) r.yLines[t] /= sc;
                r.spacing /= sc;
                cand.x0 /= sc; cand.x1 /= sc; cand.y0 /= sc; cand.y1 /= sc; cand.step /= sc;
            }
            geo = r;
            haveGeo = true;
            return true;
        }
        return false;
    };

    if (haveCand) {                                   // probe：候选已经有了
        geo = refineGrid(masked, cand, size);
        haveGeo = geo.ok;
        lane = 1;
    } else if (hint.has && searchNear(gray, hint, size, cand)) {
        geo = refineGrid(masked, cand, size);
        haveGeo = geo.ok;
        lane = 1;
    }
    if (!haveGeo) {
        const int topn = forceSync ? 4 : 1;
        if (attempt(gray, masked, 1.0, 8, topn)) {
            lane = 1;
        } else if (forceSync) {
            // 以下三条只给识图单帧用：连线稳态没有棋盘（没进游戏/窗口被遮）时每拍都跑
            // 四套预处理会把 2s 的扫描周期直接吃满，得不偿失。
            cv::Mat gb, rgbB;
            cv::GaussianBlur(gray, gb, cv::Size(3, 3), 0);
            cv::cvtColor(gb, rgbB, cv::COLOR_GRAY2RGB);
            if (attempt(gb, rgbB, 1.0, 8, topn)) {
                lane = 2;
            } else {
                cv::Mat eq, rgbE;
                cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(3.0, cv::Size(8, 8));
                clahe->apply(gray, eq);
                cv::cvtColor(eq, rgbE, cv::COLOR_GRAY2RGB);
                if (attempt(eq, rgbE, 1.0, 7, topn)) {
                    lane = 3;
                } else {
                    cv::Mat big, gbig;
                    cv::resize(masked, big, cv::Size(), 1.5, 1.5, cv::INTER_CUBIC);
                    cv::cvtColor(big, gbig, cv::COLOR_RGB2GRAY);
                    if (attempt(gbig, big, 1.5, 8, topn)) lane = 4;
                }
            }
        }
    }
    if (perf) {
        tFindMs = (long long)(cv::getTickCount() * 1000 / cv::getTickFrequency()) - _t1;
    }
    if (!haveGeo) {
        if (perf) fprintf(stderr, "[perf] scan size=%d found=0 prep=%lldms find=%lldms\n",
                          size, (long long)tPrepMs, (long long)tFindMs);
        return notFound(offX, offY, 0, "no-board");
    }

    const long long _t2 = perf ? (long long)(cv::getTickCount() * 1000 / cv::getTickFrequency()) : 0;
    if (perf) {
        tRefineMs = (long long)(cv::getTickCount() * 1000 / cv::getTickFrequency()) - _t2;
    }
    if (geo.ok) {
        
        double spTmp = geo.spacing != 0 ? geo.spacing : cand.step;
        if (stab.Adopt(geo.xLines, geo.yLines, spTmp)) {
            geo.spacing = spTmp;
            if (perf) fprintf(stderr, "[perf] scan stab=adopted-prev(noise jitter suppressed)\n");
        } else {
            geo.spacing = spTmp;
            if (perf) fprintf(stderr, "[perf] scan stab=adopted-new(board moved or first)\n");
        }
        // ★ 两级融合的交接：这一帧是我们自己定位出来的棋盘，立刻把窗口指纹播种进去，
        //   下一帧 stab 复用分支就能直接命中 ⇒ 从第二帧起自动切到同步读子。
        boardFpStore(masked, geo.xLines, geo.yLines, spTmp);
    }
    double spacing = geo.spacing != 0 ? geo.spacing : cand.step;
    
    // ★ 2026-10-18 修订：自制定位（findBoardRects + refineGrid）产出的正交几何 →
    //   读子换成交互算法的 syncCellInfo（sync 非空时）。
    ScanResult res = finishWithGeometry(masked, size, geo.xLines, geo.yLines,
                                        spacing, multi, offX, offY, &tReadMs, false, sync, forceSync);
    if (lane > 1 && res.diag.type == Json::OBJ) res.diag.set("lane", Json::makeInt(lane));
    if (perf) {
        fprintf(stderr, "[perf] scan size=%d found=%d prep=%lldms find=%lldms refine=%lldms read=%lldms total=%lldms\n",
                size, 1, (long long)tPrepMs, (long long)tFindMs, (long long)tRefineMs,
                (long long)tReadMs,
                (long long)(cv::getTickCount() * 1000 / cv::getTickFrequency()) - _t0);
    }
    return res;
}

}  

ScanResult scanOnce(const cv::Mat* rgbIn, int size, const ScanHint& hint,
                    const std::vector<ExcludeRect>& exclude, bool probe,
                    const std::vector<int>* region, const LockedGeo* locked,
                    const SyncRecogParams* sync, bool forceSync) {
    cv::Mat owned;
    const cv::Mat* rgb = rgbIn;
    double offX = 0, offY = 0;
    bool directRegion = false;     // ★ 图像本体即 region（未走整屏抓取再裁剪）
    if (!rgb) {
        int ox = 0, oy = 0;
        CapSelect();
        // ★ 2026-10-18：连线稳态每拍只读「已框定区域」，按 region 直接抓取即可，
        //   不必整屏抓取后再裁（详见 captureScreenRect 注释）。越界/失败自动回退整屏。
        if (region && region->size() >= 4 && g_capMode == CapMode::GDI) {
            directRegion = captureScreenRect(
                owned, ox, oy,
                iRound((double)(*region)[0]), iRound((double)(*region)[1]),
                iRound((double)(*region)[2]), iRound((double)(*region)[3]));
        }
        if (!directRegion && !captureScreen(owned, ox, oy)) {
            ScanResult r;
            r.ok = false;
            r.diag = Json::makeObj();
            r.diag.set("reason", Json::makeStr("capture-failed"));
            return r;
        }
        rgb = &owned;
        offX = ox;
        offY = oy;
    } else {
        offX = screenOx;
        offY = screenOy;
    }

    if (region && region->size() >= 4) {
        int oxI = iRound(offX), oyI = iRound(offY);
        int rx0 = iRound((double)(*region)[0]), ry0 = iRound((double)(*region)[1]);
        int rw0 = iRound((double)(*region)[2]), rh0 = iRound((double)(*region)[3]);
        // directRegion：图像本体就是该区域，区域在图像内的偏移恒为 0；否则按整屏图内的偏移裁剪。
        int rx = directRegion ? 0 : rx0 - oxI, ry = directRegion ? 0 : ry0 - oyI;
        int h = rgb->rows, w = rgb->cols;
        rx = std::max(0, std::min(rx, w - 1));
        ry = std::max(0, std::min(ry, h - 1));
        int rw = std::max(1, std::min(rw0, w - rx));
        int rh = std::max(1, std::min(rh0, h - ry));
        // 子图原点的绝对坐标（bx,by）：用于把绝对坐标的 locked/几何换算成子图局部坐标，
        // 以及把结果再从局部坐标平移回绝对坐标。
        int bx = directRegion ? rx0 : rx, by = directRegion ? ry0 : ry;

        cv::Mat sub = directRegion ? owned : (*rgb)(cv::Rect(rx, ry, rw, rh)).clone();
        if (!exclude.empty()) {
            
            maskExcluded(sub, exclude, rx0, ry0);
        }
        
        LockedGeo lg;
        const LockedGeo* useLocked = nullptr;
        if (locked && locked->has && (int)locked->xLines.size() == size &&
            (int)locked->yLines.size() == size && locked->spacing > 0) {
            lg.has = true;
            lg.spacing = locked->spacing;
            lg.xLines.resize(locked->xLines.size());
            for (size_t i = 0; i < locked->xLines.size(); ++i) lg.xLines[i] = locked->xLines[i] - bx;
            lg.yLines.resize(locked->yLines.size());
            for (size_t i = 0; i < locked->yLines.size(); ++i) lg.yLines[i] = locked->yLines[i] - by;
            useLocked = &lg;
        }
        ScanResult res = scanSub(sub, size, ScanHint{}, {}, probe, 0, 0, useLocked, sync, forceSync);
        if (res.found) {
            shiftJsonNum(res.boardRect, "x", bx);
            shiftJsonNum(res.boardRect, "y", by);
            shiftJsonArr(res.geometry, "x_lines", bx);
            shiftJsonArr(res.geometry, "y_lines", by);
            res.ox = offX + rx;      // directRegion 时 offX=rx0、rx=0 → 仍是区域左上角绝对坐标
            res.oy = offY + ry;
        }
        return res;
    }
    return scanSub(*rgb, size, hint, exclude, probe, offX, offY, locked, sync, forceSync);
}

std::vector<BoardRectCand> snapBoardRects(const cv::Mat& gray, int lineCount, int topn) {
    std::vector<BoardRectCand> out;
    for (const BoardCand& c : findBoardRects(gray, lineCount, 8, topn)) {
        BoardRectCand r;
        r.x0 = c.x0; r.y0 = c.y0; r.x1 = c.x1; r.y1 = c.y1;
        r.step = c.step; r.score = c.score;
        out.push_back(r);
    }
    return out;
}

}  
