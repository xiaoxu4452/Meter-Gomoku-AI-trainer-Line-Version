/* Vision - boardscanapi. */


#include "gbscanapi.h"

#include <typeinfo>
#include <utility>
#include <vector>

namespace gb {

Json offsetJson(double ox, double oy) {
    Json a = Json::makeArr();
    a.push(Json::makeInt((long long)ox));
    a.push(Json::makeInt((long long)oy));
    return a;
}

Json stonesArr(const std::vector<std::pair<int, int>>& v) {
    Json a = Json::makeArr();
    for (auto& p : v) {
        Json s = Json::makeObj();
        s.set("x", Json::makeInt(p.first));
        s.set("y", Json::makeInt(p.second));
        a.push(s);
    }
    return a;
}

Json ScanResultJson(const ScanResult& r) {
    Json j = Json::makeObj();
    j.set("ok", Json::makeBool(r.ok));
    j.set("found", Json::makeBool(r.found));
    j.set("offset", offsetJson(r.ox, r.oy));
    if (!r.found) {
        j.set("multi_board", Json::makeInt(r.multiBoard));
        j.set("diag", r.diag);
        return j;
    }
    j.set("board_rect", r.boardRect);
    j.set("geometry", r.geometry);
    j.set("black", stonesArr(r.black));
    j.set("white", stonesArr(r.white));
    j.set("suspect", Json::makeBool(r.suspect));
    j.set("multi_board", Json::makeInt(r.multiBoard));
    j.set("diag", r.diag);
    return j;
}

namespace {

std::vector<ExcludeRect> parseExclude(const Json& req) {
    std::vector<ExcludeRect> out;
    const Json* p = req.find("exclude");
    if (!p || p->type != Json::ARR) return out;
    for (const Json& it : p->arr) {
        if (it.type != Json::OBJ) continue;
        ExcludeRect e;
        e.x = (int)it.numOr("x", 0);
        e.y = (int)it.numOr("y", 0);
        e.w = (int)it.numOr("w", 0);
        e.h = (int)it.numOr("h", 0);
        out.push_back(e);
    }
    return out;
}

}  

std::string ScanDirect(const std::string& body) {
    
    static ScanHint lastHint;
    static bool lastHintValid = false;

    Json reqJson;
    ScanHint hint;
    bool hintGiven = false;
    int size = 15;
    bool probe = false;
    std::vector<int> region;
    std::vector<ExcludeRect> exclude;
    LockedGeo locked;                 
    SyncRecogParams syncP;
    const SyncRecogParams* useSync = nullptr;
    if (!body.empty()) {
        if (jsonParse(body, reqJson)) {
            const Json* h = reqJson.find("hint");
            if (h && h->type == Json::OBJ) {
                hint.has = true;
                hint.x = h->numOr("x", 0);
                hint.y = h->numOr("y", 0);
                hint.w = h->numOr("w", 0);
                hint.h = h->numOr("h", 0);
                hintGiven = true;
            }
            size = (int)reqJson.numOr("size", 15);
            probe = reqJson.boolOr("probe", false);
            exclude = parseExclude(reqJson);
            const Json* rg = reqJson.find("region");
            if (rg && rg->type == Json::ARR && rg->arr.size() >= 4) {
                for (int i = 0; i < 4; ++i) region.push_back((int)rg->arr[i].num);
            }
            
            const Json* lg = reqJson.find("geometry");
            if (lg && lg->type == Json::OBJ) {
                const Json* xa = lg->find("x_lines");
                const Json* ya = lg->find("y_lines");
                if (xa && xa->type == Json::ARR && ya && ya->type == Json::ARR) {
                    for (const Json& v : xa->arr)
                        if (v.type == Json::NUM) locked.xLines.push_back(v.num);
                    for (const Json& v : ya->arr)
                        if (v.type == Json::NUM) locked.yLines.push_back(v.num);
                    locked.spacing = lg->numOr("spacing", 0.0);
                    locked.has = locked.spacing > 0 &&
                                 locked.xLines.size() >= 3 && locked.yLines.size() >= 3;
                }
            }

            if (reqJson.strOr("algo", "") == "sync") {
                auto norm = [](int v, int fb) { return v > 0 ? v : fb; };
                const Json* rc = reqJson.find("recog");
                if (rc && rc->type == Json::OBJ) {
                    syncP.grayOffset   = norm((int)rc->numOr("gray_offset",   50), 50);
                    syncP.blackOffset  = norm((int)rc->numOr("black_offset",  96), 96);
                    syncP.whiteOffset  = norm((int)rc->numOr("white_offset",  96), 96);
                    syncP.blackPercent = norm((int)rc->numOr("black_percent", 33), 33);
                    syncP.whitePercent = norm((int)rc->numOr("white_percent", 33), 33);
                }
                useSync = &syncP;
            }
        }
    }
    if (!hintGiven && lastHintValid) hint = lastHint;

    ScanResult res;
    try {
        res = scanOnce(nullptr, size, hint, exclude, probe,
                       region.empty() ? nullptr : &region,
                       locked.has ? &locked : nullptr, useSync);
    } catch (const std::exception& e) {
        Json j = Json::makeObj();
        j.set("ok", Json::makeBool(false));
        j.set("err", Json::makeStr(std::string(typeid(e).name()) + ": " + e.what()));
        return j.dump();
    } catch (...) {
        Json j = Json::makeObj();
        j.set("ok", Json::makeBool(false));
        j.set("err", Json::makeStr("unknown error"));
        return j.dump();
    }

    if (res.ok) res.diag.set("algo", Json::makeStr(useSync ? "sync" : "adaptive"));

    if (res.found && !res.boardRect.obj.empty()) {
        const Json* x = res.boardRect.find("x");
        const Json* y = res.boardRect.find("y");
        const Json* w = res.boardRect.find("w");
        const Json* hh = res.boardRect.find("h");
        if (x && y && w && hh) {
            lastHint.has = true;
            lastHint.x = x->num;
            lastHint.y = y->num;
            lastHint.w = w->num;
            lastHint.h = hh->num;
            lastHintValid = true;
        }
    }

    return ScanResultJson(res).dump();
}

}  
