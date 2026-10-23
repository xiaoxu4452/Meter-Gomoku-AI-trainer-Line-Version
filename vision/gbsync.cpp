


#include "gbvision.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace gb {

namespace {

inline int iRoundD(double v) { return (int)std::nearbyint(v); }

//
//   VerticalLine(pos=x, StartPoint=(x,minY), EndPoint=(x,maxY))
//   HorizontalLine(pos=y, StartPoint=(minX,y), EndPoint=(maxX,y))
struct SyncVLine { int pos = 0, lo = 0, hi = 0; bool del = false; bool validate = false; };
struct SyncHLine { int pos = 0, lo = 0, hi = 0; bool del = false; bool validate = false; };

struct SyncGapCount { int gap = 0; int loose = 0; int uniq = 0; };
struct SyncOffset   { int minPos = 0, maxPos = 0, off = 0; };

void tryMergeV(SyncVLine& a, SyncVLine& b) {
    if (std::abs(a.pos - b.pos) <= 4) {
        b.pos = (a.pos + b.pos) / 2;
        b.lo = std::min(a.lo, b.lo);
        b.hi = std::max(a.hi, b.hi);
        a.del = true;
    }
}

void tryMergeH(SyncHLine& a, SyncHLine& b) {
    if (std::abs(a.pos - b.pos) <= 4) {
        b.pos = (a.pos + b.pos) / 2;
        b.lo = std::min(a.lo, b.lo);
        b.hi = std::max(a.hi, b.hi);
        a.del = true;
    }
}

void mergeVLines(std::vector<SyncVLine>& v) {
    std::stable_sort(v.begin(), v.end(),
                     [](const SyncVLine& a, const SyncVLine& b) { return a.pos < b.pos; });
    for (int i = 0; i < (int)v.size() / 2; ++i) tryMergeV(v[i], v[i + 1]);
    if (v.size() >= 2) {
        for (int n = (int)v.size(); n > (int)v.size() / 2; --n) {
            SyncVLine& second = v[n - 1];
            SyncVLine& first = v[n - 2];
            if (!first.del) tryMergeV(first, second);
        }
    }
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const SyncVLine& l) { return l.del; }),
            v.end());
}

void mergeHLines(std::vector<SyncHLine>& v) {
    std::stable_sort(v.begin(), v.end(),
                     [](const SyncHLine& a, const SyncHLine& b) { return a.pos < b.pos; });
    for (int i = 0; i < (int)v.size() / 2; ++i) tryMergeH(v[i], v[i + 1]);
    if (v.size() >= 2) {
        for (int n = (int)v.size(); n > (int)v.size() / 2; --n) {
            SyncHLine& second = v[n - 1];
            SyncHLine& first = v[n - 2];
            if (!first.del) tryMergeH(first, second);
        }
    }
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const SyncHLine& l) { return l.del; }),
            v.end());
}

void addGapCountIfMissing(std::vector<SyncGapCount>& gc, int gap) {
    for (const SyncGapCount& g : gc) if (g.gap == gap) return;
    SyncGapCount n; n.gap = gap; gc.push_back(n);
}

void updateGapCounts(std::vector<SyncGapCount>& gc, int gap) {
    for (SyncGapCount& g : gc) {
        if (gap == g.gap) { g.loose++; g.uniq++; }
        else if (std::abs(gap - g.gap) <= 3) g.loose++;
    }
}

std::vector<SyncGapCount> buildGapCounts(const std::vector<int>& gaps) {
    std::vector<SyncGapCount> gc;
    for (int g : gaps) addGapCountIfMissing(gc, g);
    for (int g : gaps) updateGapCounts(gc, g);
    std::stable_sort(gc.begin(), gc.end(), [](const SyncGapCount& l, const SyncGapCount& r) {
        if (l.uniq != r.uniq) return l.uniq > r.uniq;
        if (l.loose != r.loose) return l.loose < r.loose;
        return l.gap < r.gap;
    });
    return gc;
}

int selectMedianGap(int a, int b, int c) {
    if ((a > b && b > c) || (c > b && b > a)) return b;
    if ((b > a && a > c) || (c > a && a > b)) return a;
    return c;
}

bool resolveDominantGap(const std::vector<SyncGapCount>& gc, int& out) {
    if (gc.empty()) return false;
    int gap = gc[0].gap;
    if ((int)gc.size() < 3) { out = gap; return true; }
    const SyncGapCount& c0 = gc[0];
    const SyncGapCount& c1 = gc[1];
    const SyncGapCount& c2 = gc[2];
    if (std::abs(c0.gap - c1.gap) > 2 || std::abs(c0.gap - c2.gap) > 2) { out = gap; return true; }
    if ((double)c1.uniq / (double)c0.uniq <= 0.5 || (double)c2.uniq / (double)c0.uniq <= 0.5) {
        out = gap;
        return true;
    }
    out = selectMedianGap(c0.gap, c1.gap, c2.gap);
    return true;
}

void markValidatedV(std::vector<SyncVLine>& v, int gap) {
    for (size_t i = 0; i < v.size(); ++i)
        for (size_t j = i + 1; j < v.size(); ++j)
            if (std::abs(v[j].pos - v[i].pos - gap) <= std::max(1, gap / 8)) {
                v[i].validate = true;
                v[j].validate = true;
            }
}

void markValidatedH(std::vector<SyncHLine>& h, int gap) {
    for (size_t i = 0; i < h.size(); ++i)
        for (size_t j = i + 1; j < h.size(); ++j)
            if (std::abs(h[j].pos - h[i].pos - gap) <= std::max(1, gap / 8)) {
                h[i].validate = true;
                h[j].validate = true;
            }
}

std::vector<SyncOffset> buildVOffsets(const std::vector<SyncVLine>& v, int boardPixelW) {
    std::vector<SyncOffset> out;
    for (size_t i = 0; i < v.size(); ++i)
        for (size_t j = i + 1; j < v.size(); ++j) {
            SyncOffset o;
            o.minPos = v[i].pos;
            o.maxPos = v[j].pos;
            o.off = std::abs(v[j].pos - v[i].pos - boardPixelW);
            out.push_back(o);
        }
    std::sort(out.begin(), out.end(),
              [](const SyncOffset& a, const SyncOffset& b) { return a.off < b.off; });
    return out;
}

std::vector<SyncOffset> buildHOffsets(const std::vector<SyncHLine>& h, int boardPixelH) {
    std::vector<SyncOffset> out;
    for (size_t i = 0; i < h.size(); ++i)
        for (size_t j = i + 1; j < h.size(); ++j) {
            SyncOffset o;
            o.minPos = h[i].pos;
            o.maxPos = h[j].pos;
            o.off = std::abs(h[j].pos - h[i].pos - boardPixelH);
            out.push_back(o);
        }
    std::sort(out.begin(), out.end(),
              [](const SyncOffset& a, const SyncOffset& b) { return a.off < b.off; });
    return out;
}

int countHCrossings(const std::vector<SyncHLine>& h, int x) {
    int c = 0;
    for (const SyncHLine& l : h)
        if (l.validate && l.lo < x && l.hi > x) c++;
    return c;
}

int countVCrossings(const std::vector<SyncVLine>& v, int y, int upperExclusive) {
    int c = 0;
    for (const SyncVLine& l : v)
        if (l.validate && l.lo < y && l.hi > y && l.lo < upperExclusive) c++;
    return c;
}

int countHOffsetSupport(const SyncOffset& o, const std::vector<SyncHLine>& h, int gapV) {
    int x1 = o.minPos + gapV / 2;
    int x2 = o.maxPos - gapV / 2;
    return std::min(countHCrossings(h, x1), countHCrossings(h, x2));
}

int countVOffsetSupport(const SyncOffset& o, const std::vector<SyncVLine>& v, int gapH) {
    int y1 = o.minPos + gapH / 2;
    int y2 = o.maxPos - gapH / 2;
    return std::min(countVCrossings(v, y1, y1), countVCrossings(v, y2, y2));
}

int countLegacyVOffsetSupport(const SyncOffset& o, const std::vector<SyncVLine>& v,
                              int gapH, const SyncOffset& first) {
    int y1 = o.minPos + gapH / 2;
    int y2 = o.maxPos - gapH / 2;
    int upper = first.maxPos - gapH / 2;
    return std::min(countVCrossings(v, y1, y1), countVCrossings(v, y2, upper));
}

bool canTrimOffsets(const std::vector<SyncOffset>& o, int gap) {
    return o.size() >= 2 && std::abs(o[0].off - o[1].off) <= gap / 3;
}

void trimVOffsets(std::vector<SyncOffset>& o, const std::vector<SyncHLine>& h, int gapV) {
    while (canTrimOffsets(o, gapV)) {
        int a = countHOffsetSupport(o[0], h, gapV);
        int b = countHOffsetSupport(o[1], h, gapV);
        if (a >= b) o.erase(o.begin() + 1);
        else        o.erase(o.begin());
    }
}

void trimHOffsets(std::vector<SyncOffset>& o, const std::vector<SyncVLine>& v, int gapH) {
    while (canTrimOffsets(o, gapH)) {
        int a = countVOffsetSupport(o[0], v, gapH);
        int b = countLegacyVOffsetSupport(o[1], v, gapH, o[0]);
        if (a >= b) o.erase(o.begin() + 1);
        else        o.erase(o.begin());
    }
}

//
//
constexpr uint64_t kFnvBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

inline uint64_t fnvByte(uint64_t h, uint8_t v) { return (h ^ v) * kFnvPrime; }

uint32_t cellSignature(const cv::Mat& rgb, int sx, int sy, int w, int h) {
    const int stepX = std::max(1, w / 6);
    const int stepY = std::max(1, h / 6);
    uint32_t acc = 2166136261u;
    for (int yy = sy + stepY / 2; yy < sy + h; yy += stepY) {
        const uint8_t* row = rgb.ptr<uint8_t>(yy);
        for (int xx = sx + stepX / 2; xx < sx + w; xx += stepX) {
            const uint8_t* p = row + xx * 3;
            acc = (acc ^ p[0]) * 16777619u;
            acc = (acc ^ p[1]) * 16777619u;
            acc = (acc ^ p[2]) * 16777619u;
        }
    }
    return acc;
}

bool sameLines(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i] - b[i]) > 0.05) return false;
    }
    return true;
}

struct SyncReadCacheState {
    bool   valid = false;
    bool   boardOk = true;
    int    n = 0, cellW = 0, cellH = 0, cropW = 0, cropH = 0;
    std::vector<double> xl, yl;
    double spacing = 0;
    SyncRecogParams p{};
    std::vector<uint32_t> cellSig;      // n*n
    std::vector<int8_t>   cellState;
};
SyncReadCacheState g_syncReadCache;

bool sameParams(const SyncRecogParams& a, const SyncRecogParams& b) {
    return a.grayOffset == b.grayOffset && a.blackOffset == b.blackOffset &&
           a.whiteOffset == b.whiteOffset && a.blackPercent == b.blackPercent &&
           a.whitePercent == b.whitePercent;
}

}  


SyncCell syncCellInfo(const cv::Mat& rgb, int sx, int sy, int w, int h,
                      const SyncRecogParams& p) {
    SyncCell out;
    if (rgb.empty() || rgb.type() != CV_8UC3 || w <= 0 || h <= 0) return out;

    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx + w > rgb.cols) sx = std::max(0, rgb.cols - w);
    if (sy + h > rgb.rows) sy = std::max(0, rgb.rows - h);

    const int pureWhiteValue = 225;
    const int whiteValue = 255 - p.whiteOffset;
    const int almostWhiteValue = 190;
    const int sampleY = iRoundD((double)h / 4.0);
    const int sampleWidth = iRoundD((double)w * 2.0 / 6.0);

    int blackCount = 0, whiteCount = 0, pureWhiteCount = 0, almostWhiteCount = 0;
    bool trueWhite = false;

    for (int yy = 0; yy < h; ++yy) {
        const uint8_t* row = rgb.ptr<uint8_t>(sy + yy);
        for (int xx = 0; xx < w; ++xx) {
            const int R = row[(sx + xx) * 3 + 0];
            const int G = row[(sx + xx) * 3 + 1];
            const int B = row[(sx + xx) * 3 + 2];
            if (std::abs(R - G) < p.grayOffset && std::abs(G - B) < p.grayOffset &&
                std::abs(B - R) < p.grayOffset) {
                if (R <= p.blackOffset && G <= p.blackOffset && B <= p.blackOffset) blackCount++;
                if (R >= whiteValue && G >= whiteValue && B >= whiteValue) whiteCount++;
                if (R >= pureWhiteValue && G >= pureWhiteValue && B >= pureWhiteValue) pureWhiteCount++;
                if (R >= almostWhiteValue && G >= almostWhiteValue && B >= almostWhiteValue)
                    almostWhiteCount++;
            }
            if (!trueWhite && yy == sampleY && xx < sampleWidth &&
                (R < pureWhiteValue || G < pureWhiteValue || B < pureWhiteValue))
                trueWhite = true;
        }
    }

    const int total = w * h;
    if (total > 0) {
        out.blackPercent = 100 * blackCount / total;
        out.whitePercent = 100 * whiteCount / total;
        out.pureWhitePercent = 100 * pureWhiteCount / total;
        out.almostWhitePercent = 100 * almostWhiteCount / total;
    }
    out.trueWhite = trueWhite;
    return out;
}


bool locateBoardSync(const cv::Mat& rgb, int size,
                     std::vector<double>& xLines, std::vector<double>& yLines,
                     double& spacing, Json& diag) {
    if (rgb.empty() || size < 2) return false;

    cv::Mat gray;
    cv::cvtColor(rgb, gray, cv::COLOR_RGB2GRAY);

    const int minDim = std::min(rgb.cols, rgb.rows);
    int d = (int)((double)minDim / ((double)(size + 1) * 1.5));
    if (d < 1) d = 1;

    std::vector<SyncVLine> vlines;
    std::vector<SyncHLine> hlines;

    auto collect = [&](int lowCanny, int highCanny) {
        vlines.clear();
        hlines.clear();
        cv::Mat edges;
        cv::Canny(gray, edges, lowCanny, highCanny);
        std::vector<cv::Vec4i> segs;
        cv::HoughLinesP(edges, segs, 1.0, CV_PI / 180.0, d, d, 1.0);
        for (const cv::Vec4i& s : segs) {
            const int x1 = s[0], y1 = s[1], x2 = s[2], y2 = s[3];
            if (std::abs(x1 - x2) < 2 || std::abs(y1 - y2) < 2) {
                if (std::abs(y1 - y2) >= 2) {
                    SyncVLine l;
                    l.pos = (x1 + x2) / 2;
                    l.lo = std::min(y1, y2);
                    l.hi = std::max(y1, y2);
                    vlines.push_back(l);
                } else {
                    SyncHLine l;
                    l.pos = (y1 + y2) / 2;
                    l.lo = std::min(x1, x2);
                    l.hi = std::max(x1, x2);
                    hlines.push_back(l);
                }
            }
        }
        mergeVLines(vlines);
        mergeHLines(hlines);
    };

    double low = 200.0, high = 550.0;
    collect((int)low, (int)high);
    int attempts = 10;
    while ((int)(vlines.size() + hlines.size()) < 10 && attempts > 0) {
        low *= 0.9;
        high *= 0.9;
        attempts--;
        collect((int)low, (int)high);
    }
    if ((int)vlines.size() < 2 || (int)hlines.size() < 2) return false;

    std::vector<int> gapsV, gapsH;
    for (size_t i = 0; i + 1 < vlines.size(); ++i) gapsV.push_back(vlines[i + 1].pos - vlines[i].pos);
    for (size_t i = 0; i + 1 < hlines.size(); ++i) gapsH.push_back(hlines[i + 1].pos - hlines[i].pos);

    int gapV = 0, gapH = 0;
    if (!resolveDominantGap(buildGapCounts(gapsV), gapV)) return false;
    if (!resolveDominantGap(buildGapCounts(gapsH), gapH)) return false;

    {
        int gm = (gapV + gapH + 1) / 2;
        if (gm > 0) { gapV = gm; gapH = gm; }
    }

    markValidatedV(vlines, gapV);
    markValidatedH(hlines, gapH);

    const int boardPixelW = gapV * (size - 1);
    const int boardPixelH = gapH * (size - 1);

    std::vector<SyncOffset> offV = buildVOffsets(vlines, boardPixelW);
    std::vector<SyncOffset> offH = buildHOffsets(hlines, boardPixelH);
    if (offV.empty() || offH.empty()) return false;
    trimVOffsets(offV, hlines, gapV);
    trimHOffsets(offH, vlines, gapH);
    if (offV.empty() || offH.empty()) return false;

    const int padY = (int)std::ceil((double)gapV / 2.0);
    const int padX = (int)std::ceil((double)gapH / 2.0);
    const int left = offV[0].minPos - padX;
    const int top = offH[0].minPos - padY;
    const int right = offV[0].maxPos + padX;
    const int bottom = offH[0].maxPos + padY;

    xLines.resize(size);
    yLines.resize(size);
    for (int i = 0; i < size; ++i) xLines[i] = (double)(offV[0].minPos + i * gapV);
    for (int j = 0; j < size; ++j) yLines[j] = (double)(offH[0].minPos + j * gapH);
    spacing = ((double)gapV + (double)gapH) / 2.0;

    Json g = Json::makeObj();
    g.set("x0", Json::makeInt(left));
    g.set("y0", Json::makeInt(top));
    g.set("x1", Json::makeInt(right));
    g.set("y1", Json::makeInt(bottom));
    g.set("gapV", Json::makeInt(gapV));
    g.set("gapH", Json::makeInt(gapH));
    g.set("vLines", Json::makeInt((long long)vlines.size()));
    g.set("hLines", Json::makeInt((long long)hlines.size()));
    diag.set("sync_geo", g);
    return true;
}


void readStonesSync(const cv::Mat& crop, const GeoLike& g, const SyncRecogParams& p,
                    cv::Mat& board, Json& diag, bool allowRelax) {
    const int n = g.size;
    board = cv::Mat(n, n, CV_8SC1, cv::Scalar(0));

    const int cellW = std::max(1, iRoundD(g.spacing));
    const int cellH = cellW;

    diag = Json::makeObj();

    std::vector<SyncCell> stats;
    if (allowRelax) stats.assign((size_t)n * n, SyncCell());

    const long long t0 = (long long)cv::getTickCount();

    SyncReadCacheState& C = g_syncReadCache;
    bool geomSame = C.valid && C.n == n && C.cellW == cellW && C.cellH == cellH &&
                    C.cropW == crop.cols && C.cropH == crop.rows &&
                    sameParams(C.p, p) && sameLines(C.xl, g.x_lines) && sameLines(C.yl, g.y_lines);
    if (!geomSame) {
        C.valid = false;
        C.n = n; C.cellW = cellW; C.cellH = cellH;
        C.cropW = crop.cols; C.cropH = crop.rows;
        C.xl = g.x_lines; C.yl = g.y_lines; C.spacing = g.spacing;
        C.p = p;
        C.cellSig.assign((size_t)n * n, 0u);
        C.cellState.assign((size_t)n * n, (int8_t)EMPTY);
    }

    auto cellRect = [&](int i, int j, int& sx, int& sy) {
        sx = iRoundD(g.x_lines[i] - cellW / 2.0);
        sy = iRoundD(g.y_lines[j] - cellH / 2.0);
        if (sx + cellW > crop.cols) sx = crop.cols - cellW;
        if (sy + cellH > crop.rows) sy = crop.rows - cellH;
        if (sx < 0) sx = 0;
        if (sy < 0) sy = 0;
    };

    std::vector<uint32_t> sigs((size_t)n * n, 0u);
    int changed = 0;
    uint64_t frameSig = kFnvBasis;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            int sx, sy; cellRect(i, j, sx, sy);
            const size_t idx = (size_t)j * n + i;
            const uint32_t cs = cellSignature(crop, sx, sy, cellW, cellH);
            sigs[idx] = cs;
            frameSig = fnvByte(frameSig, (uint8_t)(cs & 0xFF));
            if (!C.valid || cs != C.cellSig[idx]) changed++;
        }
    }

    if (C.valid && changed == 0) {
        uint64_t stateSig = kFnvBasis;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const int8_t v = C.cellState[(size_t)j * n + i];
                board.at<int8_t>(j, i) = v;
                stateSig = fnvByte(stateSig, (uint8_t)(v + 2));   // EMPTY/BLACK/WHITE → 2/3/1
            }
        const long long hitMs = (long long)((cv::getTickCount() - t0) * 1000 / cv::getTickFrequency());
        diag.set("sync", Json::makeBool(true));
        diag.set("not_swamped", Json::makeBool(C.boardOk));
        diag.set("suspect", Json::makeBool(!C.boardOk));
        diag.set("sync_frame_hit", Json::makeBool(true));
        diag.set("sync_cells_read", Json::makeInt(0));
        diag.set("sync_cells_reused", Json::makeInt(n * n));
        diag.set("sync_read_ms", Json::makeInt((long long)hitMs));
        diag.set("sync_state_sig", Json::makeInt((long long)(stateSig & 0x7FFFFFFFFFFFFFFFULL)));
        diag.set("sync_spacing", Json::makeNum(g.spacing));
        return;
    }

    bool anyBlack = false, anyWhite = false, anyEmpty = false;
    int cellsRead = 0, cellsReused = 0;

    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            int sx, sy; cellRect(i, j, sx, sy);
            const size_t idx = (size_t)j * n + i;
            int8_t v;
            if (C.valid && sigs[idx] == C.cellSig[idx]) {
                v = C.cellState[idx];
                cellsReused++;
            } else {
                SyncCell c = syncCellInfo(crop, sx, sy, cellW, cellH, p);
                if (allowRelax) stats[idx] = c;
                v = (int8_t)EMPTY;
                if (c.blackPercent >= p.blackPercent) {
                    v = (int8_t)BLACK;
                } else if (c.whitePercent >= p.whitePercent &&
                           (c.trueWhite || c.almostWhitePercent - c.pureWhitePercent >= 10)) {
                    v = (int8_t)WHITE;
                }
                C.cellSig[idx] = sigs[idx];
                C.cellState[idx] = v;
                cellsRead++;
            }
            board.at<int8_t>(j, i) = v;
            if (v == BLACK) anyBlack = true;
            else if (v == WHITE) anyWhite = true;
            else anyEmpty = true;
        }
    }

    C.valid = true;

    if (allowRelax && !anyBlack && !anyWhite && cellsRead == n * n && !stats.empty()) {
        auto pickRelax = [&](bool black, int userPct, int& outPct) -> bool {
            std::vector<int> pcts;
            pcts.reserve(stats.size());
            for (const SyncCell& c : stats) pcts.push_back(black ? c.blackPercent : c.whitePercent);
            std::sort(pcts.begin(), pcts.end());
            const int mx = pcts.empty() ? 0 : pcts.back();
            const int med = pcts.empty() ? 0 : pcts[pcts.size() / 2];
            if (mx < 10 || mx < med + 8) return false;
            int r = mx * 6 / 10;
            if (r < 8) r = 8;
            if (r >= userPct) return false;
            outPct = r;
            return true;
        };
        int rb = 0, rw = 0;
        const bool useB = pickRelax(true, p.blackPercent, rb);
        const bool useW = pickRelax(false, p.whitePercent, rw);
        if (useB || useW) {
            int nb = 0, nw = 0;
            for (int j = 0; j < n; ++j) {
                for (int i = 0; i < n; ++i) {
                    const size_t idx = (size_t)j * n + i;
                    const SyncCell& c = stats[idx];
                    int8_t v = (int8_t)EMPTY;
                    if (useB && c.blackPercent >= rb) {
                        v = (int8_t)BLACK;
                    } else if (useW && c.whitePercent >= rw &&
                               (c.trueWhite || c.almostWhitePercent - c.pureWhitePercent >= 10)) {
                        v = (int8_t)WHITE;
                    }
                    board.at<int8_t>(j, i) = v;
                    C.cellState[idx] = v;
                    if (v == BLACK) { nb++; anyBlack = true; }
                    else if (v == WHITE) { nw++; anyWhite = true; }
                    else anyEmpty = true;
                }
            }
            diag.set("sync_relaxed", Json::makeBool(true));
            if (useB) diag.set("sync_relax_black", Json::makeInt(rb));
            if (useW) diag.set("sync_relax_white", Json::makeInt(rw));
            diag.set("sync_relax_stones", Json::makeInt(nb + nw));
        }
    }

    const bool allBlack = (!anyWhite && !anyEmpty);
    const bool allWhite = (!anyBlack && !anyEmpty);
    const bool valid = (!allBlack && !allWhite);
    C.boardOk = valid;

    uint64_t stateSig = kFnvBasis;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            stateSig = fnvByte(stateSig, (uint8_t)(board.at<int8_t>(j, i) + 2));

    const long long ms = (long long)((cv::getTickCount() - t0) * 1000 / cv::getTickFrequency());
    diag.set("sync", Json::makeBool(true));
    diag.set("not_swamped", Json::makeBool(valid));
    diag.set("suspect", Json::makeBool(!valid));
    diag.set("sync_spacing", Json::makeNum(g.spacing));
    diag.set("sync_frame_hit", Json::makeBool(false));
    diag.set("sync_cells_read", Json::makeInt(cellsRead));
    diag.set("sync_cells_reused", Json::makeInt(cellsReused));
    diag.set("sync_read_ms", Json::makeInt((long long)ms));
    diag.set("sync_state_sig", Json::makeInt((long long)(stateSig & 0x7FFFFFFFFFFFFFFFULL)));
}


namespace {

inline int lumOf(const cv::Mat& rgb, int x, int y) {
    const uint8_t* p = rgb.ptr<uint8_t>(y) + (size_t)x * 3;
    return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000;
}

void samplePatchLum(const cv::Mat& rgb, double cx, double cy, double half,
                    std::vector<int>& out) {
    static const double kOff[3] = { -0.6, 0.0, 0.6 };
    for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) {
            int x = iRoundD(cx + kOff[b] * half);
            int y = iRoundD(cy + kOff[a] * half);
            x = std::max(0, std::min(rgb.cols - 1, x));
            y = std::max(0, std::min(rgb.rows - 1, y));
            out.push_back(lumOf(rgb, x, y));
        }
    }
}

int medianOf(std::vector<int>& v) {
    if (v.empty()) return 0;
    size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + m, v.end());
    return v[m];
}

}  

void readStonesLocal(const cv::Mat& crop, const GeoLike& g, cv::Mat& board, Json& diag) {
    const int n = g.size;
    board = cv::Mat(n, n, CV_8SC1, cv::Scalar(0));
    diag = Json::makeObj();
    diag.set("reader", Json::makeStr("local"));

    const double sp = g.spacing;
    if (crop.empty() || crop.type() != CV_8UC3 || n < 2 || sp < 4.0 ||
        (int)g.x_lines.size() != n || (int)g.y_lines.size() != n) {
        diag.set("local", Json::makeStr("bad-geometry"));
        return;
    }

    const double kCtrOff = 0.155, kCtrHalf = 0.075;
    const double kBgOff  = 0.400, kBgHalf  = 0.055;

    const size_t N = (size_t)n * n;
    std::vector<int> diff(N, 0);
    std::vector<int> tmp;
    tmp.reserve(40);
    static const double sx[4] = { -1, -1, 1, 1 };
    static const double sy[4] = { -1, 1, -1, 1 };

    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const double cx = g.x_lines[i], cy = g.y_lines[j];
            int ctrS[4] = { 0, 0, 0, 0 }, bgS[4] = { 0, 0, 0, 0 };
            for (int q = 0; q < 4; ++q) {
                tmp.clear();
                samplePatchLum(crop, cx + sx[q] * kCtrOff * sp, cy + sy[q] * kCtrOff * sp,
                               kCtrHalf * sp, tmp);
                ctrS[q] = medianOf(tmp);
                tmp.clear();
                samplePatchLum(crop, cx + sx[q] * kBgOff * sp, cy + sy[q] * kBgOff * sp,
                               kBgHalf * sp, tmp);
                bgS[q] = medianOf(tmp);
            }
            std::vector<int> cv4(ctrS, ctrS + 4), bv4(bgS, bgS + 4);
            diff[(size_t)j * n + i] = medianOf(bv4) - medianOf(cv4);
        }
    }

    std::vector<int> sorted(N, 0);
    for (size_t i = 0; i < N; ++i) sorted[i] = std::abs(diff[i]);
    std::nth_element(sorted.begin(), sorted.begin() + (size_t)(N * 0.60), sorted.end());
    const int p60 = sorted[(size_t)(N * 0.60)];
    int thr = (int)std::lround(p60 * 0.45);
    if (thr < 8) thr = 8;
    if (thr > 60) thr = 60;

    int nb = 0, nw = 0;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const int d = diff[(size_t)j * n + i];
            int8_t v = (int8_t)EMPTY;
            if (d > thr) { v = (int8_t)BLACK; ++nb; }
            else if (d < -thr) { v = (int8_t)WHITE; ++nw; }
            board.at<int8_t>(j, i) = v;
        }
    }

    const bool tooMany = (nb + nw) > (int)(n * n * 3 / 4);
    diag.set("local", Json::makeBool(true));
    diag.set("local_thr", Json::makeInt(thr));
    diag.set("local_p60", Json::makeInt(p60));
    diag.set("local_black", Json::makeInt(nb));
    diag.set("local_white", Json::makeInt(nw));
    diag.set("suspect", Json::makeBool(tooMany));
}

}  
