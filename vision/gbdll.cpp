// Meter Gomoku Link — v4.43
/* Vision - boarddll. */


#include "gbscanapi.h"
#include "gbvision.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace gb;

static std::string base64Encode(const unsigned char* data, size_t len) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned b0 = data[i];
        unsigned b1 = i + 1 < len ? data[i + 1] : 0;
        unsigned b2 = i + 2 < len ? data[i + 2] : 0;
        out.push_back(T[b0 >> 2]);
        out.push_back(T[((b0 & 3) << 4) | (b1 >> 4)]);
        out.push_back(i + 1 < len ? T[((b1 & 15) << 2) | (b2 >> 6)] : '=');
        out.push_back(i + 2 < len ? T[b2 & 63] : '=');
    }
    return out;
}

namespace {

std::mutex g_mu;   

int deliver(const std::string& s, char* out, int cap) {
    int need = (int)s.size() + 1;
    if (!out || cap < need) return -need;
    memcpy(out, s.c_str(), (size_t)need);
    return (int)s.size();
}

}  

extern "C" __declspec(dllexport)
int GbvScan(const char* req, int reqLen, char* out, int cap) {
    if (!req || reqLen < 0) return -1;
    std::string resp;
    try {
        std::string body(req, (size_t)reqLen);
        std::lock_guard<std::mutex> lk(g_mu);
        resp = ScanDirect(body);
    } catch (...) {
        return -1;
    }
    return deliver(resp, out, cap);
}

extern "C" __declspec(dllexport)
int GbvScanImage(const char* img, int imgLen, int probe, char* out, int cap) {
    if (!img || imgLen <= 0) return -1;
    std::string resp;
    try {
        
        std::vector<unsigned char> raw(img, img + imgLen);
        cv::Mat decoded = cv::imdecode(raw, cv::IMREAD_COLOR);
        if (decoded.empty()) return -1;
        cv::Mat rgb;
        cv::cvtColor(decoded, rgb, cv::COLOR_BGR2RGB);
        std::lock_guard<std::mutex> lk(g_mu);
        ScanResult res = scanOnce(&rgb, 15, ScanHint{}, {}, probe != 0, nullptr);
        
        bool stonesEmptyRes = res.black.empty() && res.white.empty();
        bool needRescue = res.ok && (!res.found ||
                                     (!res.suspect && stonesEmptyRes));
        if (needRescue) {
            cv::Mat gray;
            cv::cvtColor(rgb, gray, cv::COLOR_RGB2GRAY);
            double cx0 = res.boardRect.numOr("x", -1e9);
            double cy0 = res.boardRect.numOr("y", -1e9);
            auto tryAlt = [&](const cv::Mat& srcGray, double dx, double dy,
                              const char* tag, int topn) -> bool {
                for (const BoardRectCand& c : snapBoardRects(srcGray, 15, topn)) {
                    if (std::abs(c.x0 / dx - cx0) < 2.0 && std::abs(c.y0 / dy - cy0) < 2.0) continue;
                    double pad = (c.step > 0 ? c.step : 20.0) * 0.6;
                    int x0 = std::max(0, (int)((c.x0 - pad) / dx));
                    int y0 = std::max(0, (int)((c.y0 - pad) / dy));
                    int x1 = std::min(rgb.cols, (int)((c.x1 + pad) / dx));
                    int y1 = std::min(rgb.rows, (int)((c.y1 + pad) / dy));
                    if (x1 - x0 < 120 || y1 - y0 < 120) continue;
                    cv::Mat sub = rgb(cv::Rect(x0, y0, x1 - x0, y1 - y0)).clone();
                    
                    cv::Mat bgr;
                    cv::cvtColor(sub, bgr, cv::COLOR_RGB2BGR);
                    std::vector<unsigned char> png;
                    cv::imencode(".png", bgr, png);
                    Json j = recognize(base64Encode(png.data(), png.size()), 15, true);
                    if (!j.boolOr("ok", false) || j.boolOr("suspect", false) || j.boolOr("partial", false)) continue;
                    const Json* bk = j.find("black");
                    const Json* wt = j.find("white");
                    int n = (bk ? (int)bk->arr.size() : 0) + (wt ? (int)wt->arr.size() : 0);
                    if (n == 0) continue;
                    ScanResult r2;
                    r2.ok = true; r2.found = true; r2.suspect = false;
                    r2.ox = res.ox; r2.oy = res.oy;
                    const Json* dj = j.find("diag");
                    r2.diag = (dj && dj->type == Json::OBJ) ? *dj : Json::makeObj();
                    r2.diag.set("alt_board", Json::makeBool(true));
                    r2.diag.set("method", Json::makeStr(tag));
                    if (bk) for (const Json& e : bk->arr)
                        r2.black.emplace_back((int)e.numOr("x", 0), (int)e.numOr("y", 0));
                    if (wt) for (const Json& e : wt->arr)
                        r2.white.emplace_back((int)e.numOr("x", 0), (int)e.numOr("y", 0));
                    
                    Json g2;
                    if (const Json* g = j.find("geometry"); g && g->type == Json::OBJ) g2 = *g;
                    double gx0 = -1, gy0 = -1, gx1 = -1, gy1 = -1;
                    if (Json* xs = g2.findMutable("xs"); xs && xs->type == Json::ARR &&
                        xs->arr.size() >= 2) {
                        for (Json& v : xs->arr)
                            if (v.type == Json::NUM) {
                                v.num += x0;
                                if (v.isInt) v.inum = (long long)v.num;   
                                if (gx0 < 0 || v.num < gx0) gx0 = v.num;
                                if (v.num > gx1) gx1 = v.num; }
                    }
                    if (Json* ys = g2.findMutable("ys"); ys && ys->type == Json::ARR &&
                        ys->arr.size() >= 2) {
                        for (Json& v : ys->arr)
                            if (v.type == Json::NUM) {
                                v.num += y0;
                                if (v.isInt) v.inum = (long long)v.num;   
                                if (gy0 < 0 || v.num < gy0) gy0 = v.num;
                                if (v.num > gy1) gy1 = v.num; }
                    }
                    if (gx0 >= 0 && gy0 >= 0 && gx1 > gx0 && gy1 > gy0) {
                        r2.geometry = g2;
                        Json br = Json::makeObj();
                        br.set("x", Json::makeNum(gx0));
                        br.set("y", Json::makeNum(gy0));
                        br.set("w", Json::makeNum(gx1 - gx0));
                        br.set("h", Json::makeNum(gy1 - gy0));
                        r2.boardRect = br;
                    } else {
                        
                        Json br2 = Json::makeObj();
                        br2.set("x", Json::makeNum((double)x0));
                        br2.set("y", Json::makeNum((double)y0));
                        br2.set("w", Json::makeNum((double)(x1 - x0)));
                        br2.set("h", Json::makeNum((double)(y1 - y0)));
                        r2.boardRect = br2;
                    }
                    res = r2;
                    return true;
                }
                return false;
            };
            
            cv::Mat big, bgray;
            if (!res.found) {
                if (tryAlt(gray, 1.0, 1.0, "snap_alt", 6)) {}
                else {
                    cv::resize(rgb, big, cv::Size(rgb.cols * 2, rgb.rows * 2), 0, 0, cv::INTER_CUBIC);
                    cv::cvtColor(big, bgray, cv::COLOR_RGB2GRAY);
                    tryAlt(bgray, 2.0, 2.0, "snap_alt_big", 6);
                }
            } else {
                cv::resize(rgb, big, cv::Size(rgb.cols * 2, rgb.rows * 2), 0, 0, cv::INTER_CUBIC);
                cv::cvtColor(big, bgray, cv::COLOR_RGB2GRAY);
                if (tryAlt(bgray, 2.0, 2.0, "snap_alt_big", 8)) {}
                else if (!tryAlt(gray, 1.0, 1.0, "snap_alt", 8)) {}
            }
            if (!res.found)
                res.diag.set("notfound_multi_alt", Json::makeBool(true));
            else if (res.black.empty() && res.white.empty())
                res.diag.set("empty_multi_alt", Json::makeBool(true));
        }
        resp = ScanResultJson(res).dump();
    } catch (...) {
        return -1;
    }
    return deliver(resp, out, cap);
}

extern "C" __declspec(dllexport)
int GbvRecognizeJson(const char* req, int reqLen, char* out, int cap) {
    if (!req || reqLen < 0) return -1;
    std::string resp;
    try {
        
        std::string body(req, (size_t)reqLen);
        Json reqJson, result;
        if (!jsonParse(body, reqJson)) return -1;
        int size = (int)reqJson.numOr("size", 15);
        std::string mode = reqJson.strOr("mode", "image");
        if (mode == "wulin") {
            const Json* geo = reqJson.find("geo");
            const Json* win = reqJson.find("win");
            std::lock_guard<std::mutex> lk(g_mu);
            result = recognizeWulin(geo ? *geo : Json::makeObj(),
                                    win ? *win : Json::makeObj(), size);
        } else if (reqJson.strOr("algo", "") == "sync") {
            // ★ 2026-10-19：识图走「新的视觉引擎」——自制定位(findBoardRects+refineGrid) + 同步读子。
            //   旧的 recognize()（暗细节投影/格轴那套）是另一条算法链，识图页的 5 项阈值对它完全无效，
            //   这正是「拖动识别参数没有任何变化」的根因。此处改为和连线窗同一条 scanOnce 链，并用
            //   forceSync=true 让单帧图片也直接进 sync 分支（单帧等不到「上一帧锁定几何」）。
            std::string image = reqJson.strOr("image", "");
            if (image.compare(0, 5, "data:") == 0) {
                size_t p = image.find("base64,");
                if (p != std::string::npos) image = image.substr(p + 7);
            }
            std::vector<unsigned char> raw;
            base64Decode(image, raw);
            cv::Mat decoded = cv::imdecode(raw, cv::IMREAD_COLOR);
            if (decoded.empty()) return -1;
            cv::Mat rgb;
            cv::cvtColor(decoded, rgb, cv::COLOR_BGR2RGB);

            // 参数归一化口径与 ScanDirect 的 algo=sync 完全一致（非正值回落默认）。
            auto norm = [](int v, int fb) { return v > 0 ? v : fb; };
            SyncRecogParams syncP;
            const Json* rc = reqJson.find("recog");
            if (rc && rc->type == Json::OBJ) {
                syncP.grayOffset   = norm((int)rc->numOr("gray_offset",   50), 50);
                syncP.blackOffset  = norm((int)rc->numOr("black_offset",  96), 96);
                syncP.whiteOffset  = norm((int)rc->numOr("white_offset",  96), 96);
                syncP.blackPercent = norm((int)rc->numOr("black_percent", 33), 33);
                syncP.whitePercent = norm((int)rc->numOr("white_percent", 33), 33);
            }
            std::lock_guard<std::mutex> lk(g_mu);
            ScanResult res = scanOnce(&rgb, size, ScanHint{}, {}, false, nullptr, nullptr,
                                      &syncP, /*forceSync*/true);
            if (res.ok) res.diag.set("algo", Json::makeStr("sync"));

            // ★★ 2026-10-19（用户：「识图不太智能，明明能识别却没识别出来，优化图片适应性」）：
            //   **几何校验兜底**。sync 链要求棋盘正放（网格线必须与坐标轴平行）；旋转 3° 以上、
            //   拍屏照片、透视变形会让 refineGrid 定出一个"假网格" —— found=true、liveness≈0、
            //   棋子全在错的位置（实测：旋转 3° 时 sync 读出 0 黑 14 白，实际 10 黑 10 白）。
            //   这时改用旧识别器（投影 + 部分格点，对旋转容忍得多）再算一遍，谁读出的子多用谁。
            //   触发条件：没找到盘，或 liveness < 0.15。细线图 liveness 也可能偏低，所以还要求
            //   旧识别器**真的读出了子**且数量不少于 sync，才允许接管。
            bool usedLegacy = false;
            {
                const Json* lv = res.diag.find("liveness");
                const double live = (lv && lv->type == Json::NUM) ? lv->num : 1.0;
                if (!res.found || live < 0.15) {
                    Json legacy = recognize(image, size, true);
                    const Json* lb = legacy.find("black");
                    const Json* lw = legacy.find("white");
                    const int lbn = (lb && lb->type == Json::ARR) ? (int)lb->arr.size() : 0;
                    const int lwn = (lw && lw->type == Json::ARR) ? (int)lw->arr.size() : 0;
                    const int lc = lbn + lwn;
                    const int sbn = (int)res.black.size();
                    const int swn = (int)res.white.size();
                    const int sc = sbn + swn;
                    const int lim = size * size * 3 / 5;
                    auto sane = [&](int b, int w) { const int t = b + w; return t > 0 && t <= lim; };
                    auto broken = [&](int b, int w) {
                        const int t = b + w, d = (b > w) ? (b - w) : (w - b);
                        return b == 0 || w == 0 || t > lim || d > 6;
                    };
                    const bool legacyOk = legacy.boolOr("ok", false) && lc > 0 &&
                        (!res.found || (sane(lbn, lwn) && (broken(sbn, swn) || lc > sc)));
                    if (legacyOk && legacy.type == Json::OBJ) {
                        legacy.set("fell_back", Json::makeBool(true));
                        legacy.set("live", Json::makeNum(live));
                        legacy.set("sync_stones", Json::makeInt(sc));
                        result = legacy;
                        usedLegacy = true;
                    }
                }
            }
            if (!usedLegacy) result = ScanResultJson(res);
        } else {
            std::string image = reqJson.strOr("image", "");
            bool allowSnap = !reqJson.boolOr("nosnap", false);
            std::lock_guard<std::mutex> lk(g_mu);
            result = recognize(image, size, allowSnap);
        }
        resp = result.dump();
    } catch (...) {
        return -1;
    }
    return deliver(resp, out, cap);
}

extern "C" __declspec(dllexport)
int GbvVersion(void) {
    return 2;
}
