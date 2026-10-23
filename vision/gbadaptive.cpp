// Meter Gomoku Link — v4.43
/* Vision - boardadaptive. */


#include "gbvision.h"

#include <cstdlib>
#include <climits>
#include <limits>
#include <cstring>
#include <set>
#include <thread>
#include <atomic>

namespace gb {

namespace {

inline double npRound(double v) { return std::nearbyint(v); }
inline int iRound(double v) { return (int)std::nearbyint(v); }

inline int oddKernel(double n, int lo, int hi) {
    int v = iRound(n);
    if (v % 2 == 0) v += 1;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

constexpr double CORE_R = 0.26;
constexpr double RING_R0 = 0.34;
constexpr double RING_R1 = 0.50;
constexpr int RING_SECTORS = 24;
constexpr double RING_COVER_MIN = 0.70;
constexpr double RING_ARC_MIN = 0.50;
constexpr double RING_FILL_MIN = 0.10;

constexpr double OUT_R0 = 0.50;
constexpr double OUT_R1 = 0.555;

constexpr double CELL_CONTRAST_MIN = 14.0;
constexpr double CELL_THR_RATIO = 0.45;
constexpr double CELL_BG_R = 0.12;
constexpr double CELL_BG_OFF = 0.50;
constexpr double CELL_FILL_REL = 0.25;
constexpr double CELL_FILL_MIN = 0.60;

constexpr double CELL_FILL_MAIN_MIN = 0.55;

constexpr double BASE_CONTRAST = 10.0;
constexpr double MAD_K = 4.0;
constexpr double MIN_CIRCULARITY = 0.55;
constexpr double HULL_CIRC_MIN = 0.80;
constexpr double SCORE_MIN_RATIO = 0.25;
constexpr double SCORE_MAX_RATIO = 3.2;

struct MaskIdx {
    cv::Mat m;                  
    std::vector<int> idx;       
    bool empty() const { return idx.empty(); }
};

MaskIdx makeMask(const cv::Mat& bin) {
    MaskIdx out;
    out.m = bin.clone();
    out.idx.reserve((size_t)cv::countNonZero(bin));
    for (int r = 0; r < bin.rows; ++r) {
        const uint8_t* p = bin.ptr<uint8_t>(r);
        for (int c = 0; c < bin.cols; ++c)
            if (p[c]) out.idx.push_back(r * bin.cols + c);
    }
    return out;
}

inline std::vector<int16_t> gatherI16(const cv::Mat& patch, const MaskIdx& mi) {
    std::vector<int16_t> v;
    v.reserve(mi.idx.size());
    const int16_t* p = patch.ptr<int16_t>(0);
    for (int i : mi.idx) v.push_back(p[i]);
    return v;
}

inline std::vector<int> gatherI32(const cv::Mat& patch, const MaskIdx& mi) {
    std::vector<int> v;
    v.reserve(mi.idx.size());
    const int32_t* p = patch.ptr<int32_t>(0);
    for (int i : mi.idx) v.push_back(p[i]);
    return v;
}

inline std::vector<double> toDouble(const std::vector<int16_t>& v) {
    std::vector<double> d(v.size());
    for (size_t i = 0; i < v.size(); ++i) d[i] = (double)v[i];
    return d;
}

inline cv::Mat maskOr(const cv::Mat& a, const cv::Mat& b) {
    cv::Mat out;
    cv::bitwise_or(a, b, out);
    return out;
}

}  

double noiseSigma(const cv::Mat& diff16) {
    std::vector<double> v;
    v.reserve((size_t)diff16.rows * diff16.cols);
    for (int r = 0; r < diff16.rows; ++r) {
        const int16_t* p = diff16.ptr<int16_t>(r);
        for (int c = 0; c < diff16.cols; ++c) v.push_back((double)p[c]);
    }
    return medianAbsDev(std::move(v));
}

BgEstimate estimateBackground(const cv::Mat& gray, double spacing) {
    BgEstimate out;
    int bg0 = globalMode(gray);
    out.bg0 = bg0;

    cv::Mat g;
    gray.convertTo(g, CV_32F);
    cv::Mat i16;
    gray.convertTo(i16, CV_16S);
    double sigma0 = noiseSigma(i16);

    double fgThr = std::max(10.0, MAD_K * sigma0);
    cv::Mat bgMask(g.rows, g.cols, CV_32F);
    for (int r = 0; r < g.rows; ++r) {
        const float* gp = g.ptr<float>(r);
        float* bp = bgMask.ptr<float>(r);
        for (int c = 0; c < g.cols; ++c)
            bp[c] = (std::abs((double)gp[c] - (double)bg0) > fgThr) ? 0.f : 1.f;
    }

    int k = oddKernel(spacing * 4.0, 9, 301);
    cv::Mat wsum, vsum, gmask;
    cv::boxFilter(bgMask, wsum, -1, cv::Size(k, k), cv::Point(-1, -1), false);
    cv::multiply(g, bgMask, gmask);
    cv::boxFilter(gmask, vsum, -1, cv::Size(k, k), cv::Point(-1, -1), false);

    cv::Mat field(g.rows, g.cols, CV_32F);
    for (int r = 0; r < g.rows; ++r) {
        const float* wp = wsum.ptr<float>(r);
        const float* vp = vsum.ptr<float>(r);
        float* fp = field.ptr<float>(r);
        for (int c = 0; c < g.cols; ++c) {
            float w = std::max(wp[c], 1.0f);
            float val = vp[c] / w;
            fp[c] = (wp[c] < 5.0f) ? (float)bg0 : val;
        }
    }

    std::vector<double> bp;
    for (int r = 0; r < g.rows; ++r) {
        const float* gp = g.ptr<float>(r);
        const float* mp = bgMask.ptr<float>(r);
        for (int c = 0; c < g.cols; ++c)
            if (mp[c] > 0.5f) bp.push_back((double)gp[c]);
    }
    double sigmaBg = 0.0;
    if (bp.size() > 100) sigmaBg = medianAbsDev(std::move(bp));

    out.field = field;
    out.sigma = sigmaBg;
    return out;
}

bool plateauLikeMed(double outMed, double ref) {
    double thr = std::max(10.0, 0.35 * std::abs(ref));
    if (ref >= 0) return outMed > thr;
    return outMed < -thr;
}

bool quadsPlateauVals(const std::vector<double>& quadMeds, double ref) {
    double thr = std::max(12.0, 0.35 * std::abs(ref));
    bool pos = ref >= 0;
    for (double v : quadMeds) {
        if (pos && v > thr) return true;
        if (!pos && v < -thr) return true;
    }
    return false;
}

bool plateauGateVeto(bool outPlateau, double ref, double edge) {
    if (!outPlateau) return false;
    return edge < std::max(26.0, 0.12 * std::abs(ref));
}

static double circularity(const std::vector<cv::Point>& contour) {
    double area = std::abs(cv::contourArea(contour));
    double per = cv::arcLength(contour, true);
    if (area <= 0 || per <= 0) return 0.0;
    return 4.0 * CV_PI * area / (per * per);
}

static void radialMaps(int half, double spacing, MaskIdx& coreM, MaskIdx& ringM, cv::Mat& ringSec) {
    int n = 2 * half + 1;
    cv::Mat core(n, n, CV_8U, cv::Scalar(0));
    cv::Mat ring(n, n, CV_8U, cv::Scalar(0));
    ringSec = cv::Mat(n, n, CV_32S, cv::Scalar(-1));
    for (int r = 0; r < n; ++r) {
        double yy = (double)(r - half);
        uint8_t* cp = core.ptr<uint8_t>(r);
        uint8_t* rp = ring.ptr<uint8_t>(r);
        int32_t* sp = ringSec.ptr<int32_t>(r);
        for (int c = 0; c < n; ++c) {
            double xx = (double)(c - half);
            double dist = std::hypot(xx, yy);
            if (dist <= CORE_R * spacing) cp[c] = 1;
            if (dist >= RING_R0 * spacing && dist <= RING_R1 * spacing) {
                rp[c] = 1;
                double ang = (std::atan2(yy, xx) + CV_PI) / (2.0 * CV_PI);
                int sec = (int)std::floor(ang * RING_SECTORS);
                if (sec < 0) sec = 0;
                if (sec > RING_SECTORS - 1) sec = RING_SECTORS - 1;
                sp[c] = sec;
            }
        }
    }
    coreM = makeMask(core);
    ringM = makeMask(ring);
}

static void cellBgMasks(int half, double spacing, MaskIdx out[4]) {
    int n = 2 * half + 1;
    double rr = CELL_BG_R * spacing;
    for (int si = 0; si < 2; ++si) {
        for (int sj = 0; sj < 2; ++sj) {
            double sx = si == 0 ? -1.0 : 1.0;
            double sy = sj == 0 ? -1.0 : 1.0;
            double cx = sx * CELL_BG_OFF * spacing;
            double cy = sy * CELL_BG_OFF * spacing;
            cv::Mat m(n, n, CV_8U, cv::Scalar(0));
            for (int r = 0; r < n; ++r) {
                uint8_t* p = m.ptr<uint8_t>(r);
                for (int c = 0; c < n; ++c) {
                    double xx = (double)(c - half), yy = (double)(r - half);
                    if (std::hypot(xx - cx, yy - cy) <= rr) p[c] = 1;
                }
            }
            out[si * 2 + sj] = makeMask(m);
        }
    }
}

static MaskIdx cellBgUnion(const MaskIdx cellBg[4], int row, int col, int n) {
    std::vector<int> sxList, syList;
    if (col - 1 >= 0) sxList.push_back(-1);
    if (col + 1 < n) sxList.push_back(1);
    if (row - 1 >= 0) syList.push_back(-1);
    if (row + 1 < n) syList.push_back(1);
    if (sxList.empty()) sxList.push_back(-1);
    if (syList.empty()) syList.push_back(-1);

    auto at = [&](int sx, int sy) -> const MaskIdx& {
        return cellBg[(sx > 0 ? 1 : 0) * 2 + (sy > 0 ? 1 : 0)];
    };
    cv::Mat acc = at(sxList[0], syList[0]).m.clone();
    for (int sx : sxList)
        for (int sy : syList) {
            if (sx == sxList[0] && sy == syList[0]) continue;
            cv::bitwise_or(acc, at(sx, sy).m, acc);
        }
    if (sxList.size() < 2 || syList.size() < 2) {
        for (int sx : {-1, 1})
            for (int sy : {-1, 1}) {
                bool inside = (col + sx >= 0 && col + sx < n && row + sy >= 0 && row + sy < n);
                if (inside) continue;
                cv::bitwise_or(acc, at(sx, sy).m, acc);
            }
    }
    return makeMask(acc);
}

static std::pair<int, double> decideColor(const cv::Mat& patch, const MaskIdx& coreM,
                                          const MaskIdx& ringM, const cv::Mat& ringSec,
                                          double thr) {
    std::vector<int16_t> ring = gatherI16(patch, ringM);
    if (ring.empty()) return {EMPTY, 0.0};
    std::vector<char> hit(ring.size());
    double hitSum = 0.0;
    for (size_t i = 0; i < ring.size(); ++i) {
        hit[i] = (std::abs((int)ring[i]) > thr) ? 1 : 0;
        hitSum += hit[i];
    }
    if (hitSum / (double)ring.size() < RING_FILL_MIN) return {EMPTY, 0.0};

    std::vector<int> rs = gatherI32(ringSec, ringM);

    std::vector<int> secTot(RING_SECTORS, 0), secHit(RING_SECTORS, 0);
    std::vector<std::vector<double>> secVals(RING_SECTORS);
    for (size_t i = 0; i < ring.size(); ++i) {
        int s = rs[i];
        if (s < 0) continue;
        secTot[s] += 1;
        if (hit[i]) {
            secHit[s] += 1;
            secVals[s].push_back((double)ring[i]);
        }
    }
    std::vector<char> valid(RING_SECTORS, 0);
    std::vector<double> polar(RING_SECTORS, 0.0);
    for (int s = 0; s < RING_SECTORS; ++s) {
        if (secHit[s] > 0 && secTot[s] > 0 &&
            double(secHit[s]) / double(secTot[s]) >= 0.25) {
            valid[s] = 1;
            polar[s] = median(secVals[s]);
        }
    }
    bool anyValid = false;
    for (int s = 0; s < RING_SECTORS; ++s) if (valid[s]) { anyValid = true; break; }
    if (!anyValid) return {EMPTY, 0.0};

    int best = 0, cur = 0;
    for (int s = 0; s < RING_SECTORS * 2; ++s) {
        if (valid[s % RING_SECTORS]) {
            cur += 1;
            best = std::max(best, std::min(cur, RING_SECTORS));
        } else cur = 0;
    }
    double arc = double(best) / double(RING_SECTORS);
    int validCount = 0;
    for (int s = 0; s < RING_SECTORS; ++s) validCount += valid[s] ? 1 : 0;
    double cover = double(validCount) / double(RING_SECTORS);
    if (arc < RING_ARC_MIN && cover < RING_COVER_MIN) return {EMPTY, 0.0};

    std::vector<int16_t> core = gatherI16(patch, coreM);
    if (!core.empty()) {
        int neg = 0, pos = 0;
        for (int16_t v : core) {
            if ((double)v < -thr) neg++;
            if ((double)v > thr) pos++;
        }
        double m = (double)core.size();
        
        if (double(neg) / m >= 0.5) return {BLACK, 0.85};
        if (double(pos) / m >= 0.5) return {WHITE, 0.85};
    }
    
    std::vector<double> pv;
    for (int s = 0; s < RING_SECTORS; ++s) if (valid[s]) pv.push_back(polar[s]);
    int pos = 0, neg = 0;
    for (double v : pv) {
        if (v > 0) pos++;
        else if (v < 0) neg++;
    }
    if (pos == neg) return {EMPTY, 0.0};
    double med = median(pv);
    if (med == 0.0) med = (pos > neg) ? 1.0 : -1.0;
    return {med < 0 ? WHITE : BLACK, 0.55};
}

static const double SYM_RING_MIN = 0.58;
static bool ringUniformSymmetric(const cv::Mat& patch, double thr,
                                 int* agreeOut = nullptr, int* totOut = nullptr) {
  const int n = patch.rows;
  if (n < 5 || patch.cols != n) return true;          
  const double cx = (n - 1) / 2.0;
  const int S = 24;
  int bestAgree = 0, bestTot = 0;
  for (double rf : {0.30, 0.38, 0.45}) {
    const double r = (double)n * rf;
    int dark = 0, bright = 0, tot = 0;
    for (int i = 0; i < S; ++i) {
      const double a = 2.0 * CV_PI * (double)i / (double)S;
      const int x = (int)lround(cx + r * std::cos(a));
      const int y = (int)lround(cx + r * std::sin(a));
      if (x < 0 || y < 0 || x >= n || y >= n) continue;
      const double v = (double)patch.at<int16_t>(y, x);
      ++tot;
      if (v < -thr) ++dark;
      else if (v > thr) ++bright;
    }
    const int ag = (dark > bright) ? dark : bright;
    if (tot > 0 && ag > bestAgree) { bestAgree = ag; bestTot = tot; }
  }
  if (agreeOut) *agreeOut = bestAgree;
  if (totOut) *totOut = bestTot;
  if (bestTot <= 0) return true;
  return (double)bestAgree / (double)bestTot >= SYM_RING_MIN;
}

int stripBorderArtifacts(cv::Mat& board, cv::Mat* conf, double minRatio) {
    int n = board.rows;
    if (n < 2) return 0;
    int minRun = std::max(8, iRound(n * minRatio));
    int removed = 0;

    auto cleanLine = [&](const std::vector<std::pair<int, int>>& cells) {
        size_t i = 0;
        while (i < cells.size()) {
            int v = board.at<int8_t>(cells[i].first, cells[i].second);
            size_t j = i;
            while (j + 1 < cells.size() &&
                   board.at<int8_t>(cells[j + 1].first, cells[j + 1].second) == v) j++;
            if (v != EMPTY && (int)(j - i + 1) >= minRun) {
                for (size_t k = i; k <= j; ++k) {
                    board.at<int8_t>(cells[k].first, cells[k].second) = EMPTY;
                    if (conf) conf->at<float>(cells[k].first, cells[k].second) = 0.0f;
                    removed++;
                }
            }
            i = j + 1;
        }
    };

    for (int r : {0, n - 1}) {
        std::vector<std::pair<int, int>> line;
        for (int c = 0; c < n; ++c) line.emplace_back(r, c);
        cleanLine(line);
    }
    for (int c : {0, n - 1}) {
        std::vector<std::pair<int, int>> line;
        for (int r = 0; r < n; ++r) line.emplace_back(r, c);
        cleanLine(line);
    }
    return removed;
}

Json invariants(const cv::Mat& board) {
    int nb = 0, nw = 0;
    int n = board.rows;
    for (int r = 0; r < n; ++r) {
        const int8_t* p = board.ptr<int8_t>(r);
        for (int c = 0; c < n; ++c) {
            if (p[c] == BLACK) nb++;
            else if (p[c] == WHITE) nw++;
        }
    }
    int total = nb + nw;
    bool balanced = std::abs(nb - nw) <= 2;
    bool notSwamped = double(total) <= 0.80 * double(n) * double(n);
    Json j = Json::makeObj();
    j.set("black", Json::makeInt(nb));
    j.set("white", Json::makeInt(nw));
    j.set("total", Json::makeInt(total));
    j.set("balanced", Json::makeBool(balanced));
    j.set("not_swamped", Json::makeBool(notSwamped));
    j.set("suspect", Json::makeBool((total >= 6 && !balanced) || !notSwamped));
    return j;
}

void readStonesAdaptive(const cv::Mat& rgb, const GeoLike& geo,
                        cv::Mat& board, cv::Mat& conf, Json& diag) {
    cv::Mat gray;
    cv::cvtColor(rgb, gray, cv::COLOR_RGB2GRAY);
    int n = geo.size;
    board = cv::Mat(n, n, CV_8S, cv::Scalar(EMPTY));
    conf = cv::Mat(n, n, CV_32F, cv::Scalar(0.f));
    double spacing = geo.spacing;
    int h = gray.rows, w = gray.cols;

    BgEstimate bgEst = estimateBackground(gray, spacing);
    double sigma = bgEst.sigma;
    double thr = std::max(BASE_CONTRAST, MAD_K * sigma);

    double stoneR = spacing * 0.44;
    double stoneArea = CV_PI * stoneR * stoneR;
    double minArea = stoneArea * SCORE_MIN_RATIO;
    double maxArea = stoneArea * SCORE_MAX_RATIO;
    int half = iRound(spacing * 0.62);

    cv::Mat diff16(gray.rows, gray.cols, CV_16S);
    for (int r = 0; r < gray.rows; ++r) {
        const uint8_t* gp = gray.ptr<uint8_t>(r);
        const float* fp = bgEst.field.ptr<float>(r);
        int16_t* dp = diff16.ptr<int16_t>(r);
        for (int c = 0; c < gray.cols; ++c)
            dp[c] = (int16_t)((int)gp[c] - (int)(float)fp[c]);
    }

    int pad = half;
    cv::Mat diff;
    cv::copyMakeBorder(diff16, diff, pad, pad, pad, pad, cv::BORDER_CONSTANT, cv::Scalar(0));

    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3));
    MaskIdx coreM, ringM;
    cv::Mat ringSec;
    radialMaps(half, spacing, coreM, ringM, ringSec);
    MaskIdx cellBg[4];
    cellBgMasks(half, spacing, cellBg);

    int win = 2 * half + 1;
    
    cv::Mat outM(win, win, CV_8U, cv::Scalar(0));
    cv::Mat quadMs[4];   
    for (int k = 0; k < 4; ++k) quadMs[k] = cv::Mat(win, win, CV_8U, cv::Scalar(0));
    cv::Mat edgeIn(win, win, CV_8U, cv::Scalar(0));
    cv::Mat edgeOut(win, win, CV_8U, cv::Scalar(0));
    cv::Mat yyPos(win, win, CV_8U, cv::Scalar(0)), yyNeg(win, win, CV_8U, cv::Scalar(0));
    cv::Mat xxPos(win, win, CV_8U, cv::Scalar(0)), xxNeg(win, win, CV_8U, cv::Scalar(0));
    for (int r = 0; r < win; ++r) {
        double yy = (double)(r - half);
        for (int c = 0; c < win; ++c) {
            double xx = (double)(c - half);
            double rr = std::hypot(xx, yy);
            if (rr >= OUT_R0 * spacing && rr <= OUT_R1 * spacing) outM.at<uint8_t>(r, c) = 1;
            if (rr >= spacing * 0.27 && rr <= spacing * 0.33) edgeIn.at<uint8_t>(r, c) = 1;
            if (rr >= spacing * 0.495 && rr <= spacing * 0.605) edgeOut.at<uint8_t>(r, c) = 1;
            if (yy > 0) yyPos.at<uint8_t>(r, c) = 1; else if (yy < 0) yyNeg.at<uint8_t>(r, c) = 1;
            if (xx > 0) xxPos.at<uint8_t>(r, c) = 1; else if (xx < 0) xxNeg.at<uint8_t>(r, c) = 1;
        }
    }
    {
        cv::Mat a, b;
        cv::bitwise_and(yyPos, xxPos, a); quadMs[0] = a;
        cv::bitwise_and(yyPos, xxNeg, b); quadMs[1] = b;
        cv::bitwise_and(yyNeg, xxPos, b); quadMs[2] = b;
        cv::bitwise_and(yyNeg, xxNeg, b); quadMs[3] = b;
    }

    auto cat = [&](int idx, int nline) { return idx == 0 ? 0 : (idx == nline - 1 ? 2 : 1); };
    MaskIdx bgIdxCache[3][3], outIdxCache[3][3];
    std::vector<MaskIdx> outQuadCache[3][3];
    bool bgCacheReady[3][3] = {{false}};
    for (int ri = 0; ri < 3; ++ri)
        for (int ci = 0; ci < 3; ++ci) {
            int row = (ri == 0 ? 0 : (ri == 1 ? n / 2 : n - 1));
            int col = (ci == 0 ? 0 : (ci == 1 ? n / 2 : n - 1));
            if (row >= n) row = n - 1;
            if (col >= n) col = n - 1;
            bgIdxCache[ri][ci] = cellBgUnion(cellBg, row, col, n);
            cv::Mat om = outM.clone();
            if (row == 0) cv::bitwise_and(om, yyPos, om);
            else if (row == n - 1) cv::bitwise_and(om, yyNeg, om);
            if (col == 0) cv::bitwise_and(om, xxPos, om);
            else if (col == n - 1) cv::bitwise_and(om, xxNeg, om);
            outIdxCache[ri][ci] = makeMask(om);
            
            int rowsOk[4] = {1, 1, 1, 1};
            if (row == 0) { rowsOk[2] = 0; rowsOk[3] = 0; }
            else if (row == n - 1) { rowsOk[0] = 0; rowsOk[1] = 0; }
            int colsOk[4] = {1, 1, 1, 1};
            if (col == 0) { colsOk[1] = 0; colsOk[3] = 0; }
            else if (col == n - 1) { colsOk[0] = 0; colsOk[2] = 0; }
            for (int k = 0; k < 4; ++k) {
                if (!rowsOk[k] || !colsOk[k]) continue;
                cv::Mat qm;
                cv::bitwise_and(outM, quadMs[k], qm);
                outQuadCache[ri][ci].push_back(makeMask(qm));
            }
            bgCacheReady[ri][ci] = true;
        }

    MaskIdx edgeInM = makeMask(edgeIn), edgeOutM = makeMask(edgeOut);

    cv::Mat sat;
    {
        std::vector<cv::Mat> ch;
        cv::split(rgb, ch);
        cv::Mat mx, mn;
        cv::max(ch[0], ch[1], mx); cv::max(mx, ch[2], mx);
        cv::min(ch[0], ch[1], mn); cv::min(mn, ch[2], mn);
        cv::subtract(mx, mn, sat);
    }
    double boardMed = 0.0, boardSat = 0.0;
    {
        int bx0 = std::max(0, iRound(geo.x_lines.front())), bx1 = std::min(w - 1, iRound(geo.x_lines.back()));
        int by0 = std::max(0, iRound(geo.y_lines.front())), by1 = std::min(h - 1, iRound(geo.y_lines.back()));
        if (bx1 > bx0 && by1 > by0) {
            std::vector<uint8_t> all,alls;
            all.reserve((size_t)(bx1 - bx0 + 1) * (size_t)(by1 - by0 + 1));
            alls.reserve(all.capacity());
            for (int yy = by0; yy <= by1; ++yy) {
                const uint8_t* gp = gray.ptr<uint8_t>(yy);
                const uint8_t* sp = sat.ptr<uint8_t>(yy);
                for (int xx = bx0; xx <= bx1; ++xx) { all.push_back(gp[xx]); alls.push_back(sp[xx]); }
            }
            boardMed = (double)median(all);
            boardSat = (double)median(alls);
        }
        if (boardMed <= 1.0) boardMed = 255.0;
    }
    
    double satGate = std::max(28.0, boardSat * 0.5);
    
    auto rawMedDisc = [&](int ax, int ay, double rfrac) -> double {
        int rr = iRound(spacing * rfrac);
        if (rr < 2) rr = 2;
        std::vector<uint8_t> vals;
        vals.reserve((size_t)(rr * rr * 3 + 8));
        for (int dy = -rr; dy <= rr; ++dy) {
            int gy = ay + dy; if (gy < 0 || gy >= h) continue;
            const uint8_t* gp = gray.ptr<uint8_t>(gy);
            int dxlim = (int)std::sqrt((double)rr * rr - (double)dy * dy);
            for (int dx = -dxlim; dx <= dxlim; ++dx) {
                int gx = ax + dx; if (gx < 0 || gx >= w) continue;
                vals.push_back(gp[gx]);
            }
        }
        return vals.empty() ? 255.0 : (double)median(vals);
    };
    
    auto rawMedRing = [&](int ax, int ay, double r0frac, double r1frac) -> double {
        int r0 = iRound(spacing * r0frac), r1 = iRound(spacing * r1frac);
        if (r1 < 3) r1 = 3;
        if (r0 >= r1) r0 = r1 - 1;
        std::vector<uint8_t> vals;
        vals.reserve((size_t)((r1 * 2 + 1) * (r1 * 2 + 1) / 2));
        for (int dy = -r1; dy <= r1; ++dy) {
            int gy = ay + dy; if (gy < 0 || gy >= h) continue;
            const uint8_t* gp = gray.ptr<uint8_t>(gy);
            for (int dx = -r1; dx <= r1; ++dx) {
                int gx = ax + dx; if (gx < 0 || gx >= w) continue;
                double rad = std::hypot((double)dx, (double)dy);
                if (rad < (double)r0 || rad > (double)r1) continue;
                vals.push_back(gp[gx]);
            }
        }
        return vals.empty() ? 255.0 : (double)median(vals);
    };
    
    auto rawSatDisc = [&](int ax, int ay, double rfrac) -> double {
        int rr = iRound(spacing * rfrac);
        if (rr < 2) rr = 2;
        std::vector<uint8_t> vals;
        vals.reserve((size_t)(rr * rr * 3 + 8));
        for (int dy = -rr; dy <= rr; ++dy) {
            int gy = ay + dy; if (gy < 0 || gy >= h) continue;
            const uint8_t* sp = sat.ptr<uint8_t>(gy);
            int dxlim = (int)std::sqrt((double)rr * rr - (double)dy * dy);
            for (int dx = -dxlim; dx <= dxlim; ++dx) {
                int gx = ax + dx; if (gx < 0 || gx >= w) continue;
                vals.push_back(sp[gx]);
            }
        }
        return vals.empty() ? 255.0 : (double)median(vals);
    };
    
    auto rawSatRing = [&](int ax, int ay, double r0frac, double r1frac) -> double {
        int r0 = iRound(spacing * r0frac), r1 = iRound(spacing * r1frac);
        if (r1 < 3) r1 = 3;
        if (r0 >= r1) r0 = r1 - 1;
        std::vector<uint8_t> vals;
        for (int dy = -r1; dy <= r1; ++dy) {
            int gy = ay + dy; if (gy < 0 || gy >= h) continue;
            const uint8_t* sp = sat.ptr<uint8_t>(gy);
            for (int dx = -r1; dx <= r1; ++dx) {
                int gx = ax + dx; if (gx < 0 || gx >= w) continue;
                double rad = std::hypot((double)dx, (double)dy);
                if (rad < (double)r0 || rad > (double)r1) continue;
                vals.push_back(sp[gx]);
            }
        }
        return vals.empty() ? 255.0 : (double)median(vals);
    };
    
    auto rawColoredFrac = [&](int ax, int ay, double rfrac) -> double {
        int rr = iRound(spacing * rfrac);
        if (rr < 2) rr = 2;
        long tot = 0, colored = 0;
        for (int dy = -rr; dy <= rr; ++dy) {
            int gy = ay + dy; if (gy < 0 || gy >= h) continue;
            const uint8_t* sp = sat.ptr<uint8_t>(gy);
            int dxlim = (int)std::sqrt((double)rr * rr - (double)dy * dy);
            for (int dx = -dxlim; dx <= dxlim; ++dx) {
                int gx = ax + dx; if (gx < 0 || gx >= w) continue;
                ++tot;
                if (sp[gx] > satGate) ++colored;
            }
        }
        return tot > 0 ? (double)colored / (double)tot : 0.0;
    };
    

    // Red cross / red last-move marker: strong red pixels must never be read
    // as a black stone. Returns fraction of strong red inside the disc.
    auto redMarkerFrac = [&](int ax, int ay, int rr) -> double {
        if (rr < 2) rr = 2;
        long tot = 0, red = 0;
        for (int dy = -rr; dy <= rr; ++dy) {
            int gy = ay + dy; if (gy < 0 || gy >= h) continue;
            const cv::Vec3b* sp = rgb.ptr<cv::Vec3b>(gy);
            int dxlim = (int)std::sqrt((double)rr * rr - (double)dy * dy);
            for (int dx = -dxlim; dx <= dxlim; ++dx) {
                int gx = ax + dx; if (gx < 0 || gx >= w) continue;
                ++tot;
                int R = (int)sp[gx][0], G = (int)sp[gx][1], B = (int)sp[gx][2];
                if (R > 140 && G < 110 && B < 110) ++red;
            }
        }
        return tot > 0 ? (double)red / (double)tot : 0.0;
    };
    // Colored decorations (e.g. lion corner logo over a star point) and red
    // crosses / red last-move markers must not be read as stones. Their color
    // extends OUTSIDE the stone disc (cross arms, logo edge), so we require an
    // outer annulus hit as well: a colored move number printed on a stone
    // stays inside the disc and the stone is still recognized.
    auto annulusRedFrac = [&](int ax, int ay, double r0r, double r1r) -> double {
        int r0 = iRound(spacing * r0r), r1 = iRound(spacing * r1r);
        if (r1 < r0) r1 = r0 + 1;
        long tot = 0, hit = 0;
        for (int dy = -r1; dy <= r1; ++dy) {
            int gy = ay + dy; if (gy < 0 || gy >= h) continue;
            const cv::Vec3b* sp = rgb.ptr<cv::Vec3b>(gy);
            int dxlim = (int)std::sqrt((double)r1 * r1 - (double)dy * dy);
            for (int dx = -dxlim; dx <= dxlim; ++dx) {
                int gx = ax + dx; if (gx < 0 || gx >= w) continue;
                double rad = std::sqrt((double)dx * dx + (double)dy * dy);
                if (rad < (double)r0 || rad > (double)r1) continue;
                ++tot;
                int R = (int)sp[gx][0], G = (int)sp[gx][1], B = (int)sp[gx][2];
                if (R > 140 && G < 110 && B < 110) ++hit;
            }
        }
        return tot > 0 ? (double)hit / (double)tot : 0.0;
    };
    auto coloredMarked = [&](int ax, int ay) -> bool {
        return rawColoredFrac(ax, ay, 0.44) >= 0.18;
    };
    const double RED_MARKER_MIN = 0.18;
    const int redDiscR = iRound(spacing * 0.44);
    auto redMarked = [&](int ax, int ay) -> bool {
        return redMarkerFrac(ax, ay, redDiscR) >= RED_MARKER_MIN &&
               annulusRedFrac(ax, ay, 0.44, 0.62) >= 0.04;
    };
    auto whiteByBody = [&](int ax, int ay) -> int {
        if (boardMed <= 140.0) return EMPTY;
        double shellRaw = rawMedRing(ax, ay, 0.30, 0.42);
        if (shellRaw < boardMed * 0.97) return EMPTY;
        if (rawSatRing(ax, ay, 0.30, 0.42) >= satGate) return EMPTY;
        int rr = iRound(spacing * 0.18);
        if (rr < 2) rr = 2;
        long tot = 0, bright = 0;
        for (int dy = -rr; dy <= rr; ++dy) {
            int gy = ay + dy; if (gy < 0 || gy >= h) continue;
            const uint8_t* gp = gray.ptr<uint8_t>(gy);
            int dxlim = (int)std::sqrt((double)rr * rr - (double)dy * dy);
            for (int dx = -dxlim; dx <= dxlim; ++dx) {
                int gx = ax + dx; if (gx < 0 || gx >= w) continue;
                ++tot;
                if (gp[gx] > boardMed * 1.03) ++bright;
            }
        }
        if (tot <= 0 || (double)bright / (double)tot < 0.22) return EMPTY;
        return WHITE;
    };
    
    auto rawClassify = [&](int ax, int ay) -> int {
        if (boardMed <= 140.0) return EMPTY;
        double coreRaw = rawMedDisc(ax, ay, 0.18);
        double shellRaw = rawMedRing(ax, ay, 0.30, 0.42);
        if (shellRaw < boardMed * 0.55 && coreRaw < boardMed * 0.80) return BLACK;
        
        if (shellRaw > boardMed * 1.06 && coreRaw > boardMed * 0.60) {
            if (rawSatDisc(ax, ay, 0.24) < satGate &&
                rawSatRing(ax, ay, 0.30, 0.42) < satGate) return WHITE;
        }
        
        if (shellRaw > boardMed * 1.06 &&
            rawSatRing(ax, ay, 0.30, 0.42) < satGate &&
            rawColoredFrac(ax, ay, 0.18) >= 0.25) return WHITE;
        
        return whiteByBody(ax, ay);
    };
    
    auto rawRefine = [&](int ax, int ay, int color) -> int {
        double coreRaw = rawMedDisc(ax, ay, 0.18);
        double shellRaw = rawMedRing(ax, ay, 0.30, 0.42);   
        if (color == BLACK) {
            
            if (boardMed > 140.0 && shellRaw > boardMed * 1.06) return WHITE;
            if (coreRaw > boardMed * 0.75) {
                if (boardMed > 140.0 && shellRaw < boardMed * 0.5) return BLACK;
                
                return whiteByBody(ax, ay);
            }
        } else if (color == WHITE && boardMed > 140.0) {
            
            int r0i = iRound(spacing * 0.34), r1i = iRound(spacing * 0.44);
            std::vector<uint8_t> vals;
            for (int dy = -r1i; dy <= r1i; ++dy) {
                int gy = ay + dy; if (gy < 0 || gy >= h) continue;
                const uint8_t* gp = gray.ptr<uint8_t>(gy);
                for (int dx = -r1i; dx <= r1i; ++dx) {
                    int gx = ax + dx; if (gx < 0 || gx >= w) continue;
                    double rad = std::hypot((double)dx, (double)dy);
                    if (rad < (double)r0i || rad > (double)r1i) continue;
                    vals.push_back(gp[gx]);
                }
            }
            if (vals.empty()) return EMPTY;
            std::vector<double> dv(vals.begin(), vals.end());
            if (percentile(dv, 25) > boardMed * 0.80) {
                
                if (coreRaw > boardMed * 1.12) return WHITE;
                return rawClassify(ax, ay);
            }
            
            if (coreRaw > boardMed * 0.80 && shellRaw < boardMed * 0.5) return BLACK;
        }
        return color;
    };

    cv::Mat cellStat = cv::Mat::zeros(n, n, CV_32F);
    cv::Mat cellFill = cv::Mat::zeros(n, n, CV_32F);
    cv::Mat cellOut = cv::Mat::zeros(n, n, CV_32F);
    cv::Mat cellOutq(n, n, CV_32FC4, cv::Scalar(9999.f, 9999.f, 9999.f, 9999.f));
    cv::Mat cellEdge = cv::Mat::zeros(n, n, CV_32F);

    int hits = 0, cellHits = 0;
    int nY = (int)geo.y_lines.size(), nX = (int)geo.x_lines.size();

    const char* traceEnv = getenv("GB_VISION_TRACE");
    bool traceAll = traceEnv && strcmp(traceEnv, "all") == 0;
    std::set<std::pair<int,int>> traceCells;
    if (traceEnv && !traceAll) {
        const char* p0 = traceEnv;
        while (*p0) {
            char* endp = nullptr;
            long cval = strtol(p0, &endp, 10);
            if (endp == p0) break;
            p0 = endp;
            long rval = -1;
            if (*p0 == ':') { ++p0; rval = strtol(p0, (char**)&p0, 10); }
            traceCells.insert({(int)cval, (int)rval});
            if (*p0 == ',') ++p0; else break;
        }
    }
    auto traceOn = [&](int col, int row) {
        return traceAll || traceCells.count({col, row}) > 0;
    };
#define GB_TRACE(col, row, ...) do { if (traceOn(col, row)) { fprintf(stderr, "[trace %d:%d] ", col, row); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while(0)

    auto processCell = [&](int row, int col, int& hits, int& cellHits) {
            int x = iRound(geo.x_lines[col]);
            int y = iRound(geo.y_lines[row]);
            if (!(x >= 0 && x < w && y >= 0 && y < h)) return;
            int xo = x, yo = y;                 
            x += pad; y += pad;
            
            cv::Mat validM(win, win, CV_8U, cv::Scalar(0));
            int discTotal = 0, discValid = 0;
            for (int rr2 = 0; rr2 < win; ++rr2) {
                uint8_t* vp = validM.ptr<uint8_t>(rr2);
                int gy = yo + (rr2 - half);
                for (int cc2 = 0; cc2 < win; ++cc2) {
                    int gx = xo + (cc2 - half);
                    bool inImg = (gx >= 0 && gx < w && gy >= 0 && gy < h);
                    vp[cc2] = inImg ? 1 : 0;
                    double rad = std::hypot((double)(cc2 - half), (double)(rr2 - half));
                    if (rad <= spacing * 0.44) { discTotal++; if (inImg) discValid++; }
                }
            }
            
            cv::Mat patch = diff(cv::Rect(x - half, y - half, win, win)).clone();

            int ri = cat(row, nY), ci = cat(col, nX);
            if (ri > 2) ri = 2;
            if (ci > 2) ci = 2;

            std::vector<int16_t> coreV;    
            {
                cv::Mat cb;
                cv::bitwise_and(coreM.m, validM, cb);
                coreV = gatherI16(patch, makeMask(cb));
            }
            const MaskIdx& bgm = bgIdxCache[ri][ci];
            std::vector<int16_t> bgV;
            {
                cv::Mat bb;
                cv::bitwise_and(bgm.m, validM, bb);
                bgV = gatherI16(patch, makeMask(bb));
            }
            double thrCell = thr;
            double localC = 0.0;
            if (!coreV.empty() && !bgV.empty()) {
                localC = median(coreV) - median(bgV);
                
                double tFill = std::max(BASE_CONTRAST * 0.5, std::abs(localC) * CELL_FILL_REL);
                int cnt = 0;
                if (localC < 0) {
                    for (int16_t v : coreV) if ((double)v < -tFill) cnt++;
                } else {
                    for (int16_t v : coreV) if ((double)v > tFill) cnt++;
                }
                cellFill.at<float>(row, col) = float(double(cnt) / double(coreV.size()));
                if (std::abs(localC) >= CELL_CONTRAST_MIN) {
                    thrCell = std::max(BASE_CONTRAST, std::min(thr, std::abs(localC) * CELL_THR_RATIO));
                    cellHits++;
                }
                GB_TRACE(col, row, "localC=%.1f fill=%.2f thrCell=%.1f", localC, cellFill.at<float>(row, col), thrCell);
            }
            cellStat.at<float>(row, col) = (float)localC;

            const MaskIdx& om = outIdxCache[ri][ci];
            std::vector<int16_t> ov = gatherI16(patch, om);
            cellOut.at<float>(row, col) = ov.empty() ? 0.f : (float)median(ov);
            {
                const std::vector<MaskIdx>& qs = outQuadCache[ri][ci];
                float* qp = cellOutq.ptr<float>(row, col);
                for (size_t k = 0; k < qs.size() && k < 4; ++k) {
                    std::vector<int16_t> qv = gatherI16(patch, qs[k]);
                    if (!qv.empty()) qp[k] = (float)median(qv);
                }
            }
            {
                std::vector<int16_t> ev1 = gatherI16(patch, edgeInM);
                std::vector<int16_t> ev2 = gatherI16(patch, edgeOutM);
                double e = 0.0;
                if (!ev1.empty() && !ev2.empty()) e = std::abs(median(ev1) - median(ev2));
                cellEdge.at<float>(row, col) = (float)e;
            }

            cv::Mat mask(win, win, CV_8U, cv::Scalar(0));
            {
                const int16_t* pp = patch.ptr<int16_t>(0);
                uint8_t* mp = mask.ptr<uint8_t>(0);
                for (int i = 0; i < win * win; ++i)
                    mp[i] = (std::abs((int)pp[i]) > thrCell) ? 1 : 0;
            }
            if ((double)cv::countNonZero(mask) < minArea * 0.6) {
                GB_TRACE(col, row, "REJECT tiny-mask nz=%d minArea*0.6=%.1f", (int)cv::countNonZero(mask), minArea * 0.6);
                return;
            }
            
            cv::morphologyEx(mask, mask, cv::MORPH_OPEN, kernel);

            cv::Mat labels, stats, cents;
            int cntN = cv::connectedComponentsWithStats(mask, labels, stats, cents, 8, CV_32S);
            int pick = -1;
            double best = -1e18;
            for (int i = 1; i < cntN; ++i) {
                double area = (double)stats.at<int32_t>(i, cv::CC_STAT_AREA);
                if (area < minArea || area > maxArea) continue;
                double dx = (double)cents.at<double>(i, 0) - half;
                double dy = (double)cents.at<double>(i, 1) - half;
                double dist = std::hypot(dx, dy);
                if (dist > spacing * 0.30) continue;   
                double score = area - dist * 6.0;
                if (score > best) { best = score; pick = i; }
            }
            if (pick < 0) {
                GB_TRACE(col, row, "REJECT no-blob (area/centroid)");
                return;
            }

            cv::Mat comp(win, win, CV_8U, cv::Scalar(0));
            for (int r = 0; r < win; ++r) {
                const int32_t* lp = labels.ptr<int32_t>(r);
                uint8_t* cpp = comp.ptr<uint8_t>(r);
                for (int c = 0; c < win; ++c) cpp[c] = (lp[c] == pick) ? 1 : 0;
            }
            std::vector<std::vector<cv::Point>> conts;
            std::vector<cv::Vec4i> hier;
            cv::findContours(comp, conts, hier, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
            if (conts.empty()) return;
            size_t bigI = 0;
            double bigA = -1;
            for (size_t i = 0; i < conts.size(); ++i) {
                double a = std::abs(cv::contourArea(conts[i]));
                if (a > bigA) { bigA = a; bigI = i; }
            }
            std::vector<cv::Point> big = conts[bigI];
            
            if ((double)cellFill.at<float>(row, col) < CELL_FILL_MAIN_MIN) {
                bool clipped = (discTotal > 0) &&
                               ((double)discValid < (double)discTotal * 0.98);
                
                int rawC = EMPTY;
                if (!(clipped && std::abs(localC) >= CELL_CONTRAST_MIN)) {
                    rawC = rawClassify(xo, yo);
                    if (rawC == EMPTY) {
                        GB_TRACE(col, row, "REJECT fill=%.2f < %.2f localC=%.1f", cellFill.at<float>(row, col), CELL_FILL_MAIN_MIN, localC);
                        return;
                    }
                }
            }
            double circ = circularity(big);
            if (circ < MIN_CIRCULARITY) {
                
                std::vector<cv::Point> hp;
                cv::convexHull(big, hp);
                double hcirc = circularity(hp);
                double harea = std::abs(cv::contourArea(hp));
                if (hcirc < HULL_CIRC_MIN || harea > maxArea) {
                    GB_TRACE(col, row, "REJECT circ=%.2f hull=%.2f", circ, hcirc);
                    return;
                }
                circ = hcirc;
                comp = cv::Mat::zeros(win, win, CV_8U);
                std::vector<std::vector<cv::Point>> one{hp};
                cv::drawContours(comp, one, -1, cv::Scalar(1), -1);
            }

            {
                std::vector<int16_t> ov2 = gatherI16(patch, om);
                std::vector<double> quadMeds;
                const std::vector<MaskIdx>& qs = outQuadCache[ri][ci];
                for (const MaskIdx& qm : qs) {
                    std::vector<int16_t> qv = gatherI16(patch, qm);
                    if (!qv.empty()) quadMeds.push_back(median(qv));
                }
                bool plateau = plateauLikeMed(ov.empty() ? 0.0 : median(ov), localC) ||
                               quadsPlateauVals(quadMeds, localC);
                std::vector<int16_t> e1 = gatherI16(patch, edgeInM);
                std::vector<int16_t> e2 = gatherI16(patch, edgeOutM);
                double edge = 0.0;
                if (!e1.empty() && !e2.empty()) edge = std::abs(median(e1) - median(e2));
                if (plateauGateVeto(plateau, localC, edge)) {
                    GB_TRACE(col, row, "REJECT plateau=%d edge=%.1f", (int)plateau, edge);
                    return;
                }
            }

            {
                int agree = 0, tot = 0;
                if (!ringUniformSymmetric(patch, thrCell, &agree, &tot)) {
                    GB_TRACE(col, row, "REJECT asym-ring agree=%d/%d", agree, tot);
                    return;
                }
            }

            if (coloredMarked(xo, yo) && whiteByBody(xo, yo) != WHITE) { GB_TRACE(col, row, "REJECT decoration"); return; }
            auto dc = decideColor(patch, coreM, ringM, ringSec, thrCell);
            int color = dc.first;
            double cconf = dc.second;
            GB_TRACE(col, row, "decideColor=%d conf=%.2f", (int)color, cconf);
            if (color == EMPTY) {
                
                int rawC0 = rawClassify(xo, yo);
                if (rawC0 != EMPTY) {
                    board.at<int8_t>(row, col) = (int8_t)rawC0;
                    conf.at<float>(row, col) = 0.5f;
                    hits++;
                    GB_TRACE(col, row, "COMMIT rawClassify=%d", (int)rawC0);
                    return;
                }
                
                std::vector<int16_t> vals;
                const uint8_t* cp = comp.ptr<uint8_t>(0);
                const int16_t* pp = patch.ptr<int16_t>(0);
                for (int i = 0; i < win * win; ++i) if (cp[i]) vals.push_back(pp[i]);
                if (vals.empty()) return;
                double med = median(vals);
                if (std::abs(med) < thrCell * 0.75) {
                    GB_TRACE(col, row, "REJECT fallback-med=%.1f thr*0.75=%.1f", med, thrCell * 0.75);
                    return;
                }
                color = med > 0 ? WHITE : BLACK;
                cconf = std::min(1.0, 0.6 * std::min(1.0, std::abs(med) / (thrCell * 3.0)) + 0.4 * circ);
                if (color == BLACK) {
                    
                    int neg = 0, pos = 0;
                    for (int16_t v : coreV) {
                        if ((double)v < -thrCell) neg++;
                        if ((double)v > thrCell) pos++;
                    }
                    double m = coreV.empty() ? 0.0 : (double)coreV.size();
                    if (m > 0 && double(neg) / m < 0.2 && double(pos) / m < 0.2) {
                        color = WHITE;
                        cconf = 0.5;
                    }
                }
            }
            
            int refC = rawRefine(xo, yo, color);
            if (refC == EMPTY) {
                GB_TRACE(col, row, "REJECT rawRefine EMPTY (color=%d)", (int)color);
                return;
            }
            GB_TRACE(col, row, "COMMIT color=%d -> %d", (int)color, refC);
            board.at<int8_t>(row, col) = (int8_t)refC;
            conf.at<float>(row, col) = (float)std::min(1.0, cconf);
            hits++;
    };

    {
        unsigned hw = std::thread::hardware_concurrency();
        bool canPar = (!traceEnv) && hw > 1;           
        int T = canPar ? (int)std::min<unsigned>(hw, 8u) : 1;
        if (T > 1) {
            std::atomic<int> hitsSum{0}, cellSum{0};
            std::vector<std::thread> ths;
            ths.reserve(T);
            for (int t = 0; t < T; ++t) {
                ths.emplace_back([&, t]() {
                    int hL = 0, cL = 0;
                    for (int row = t; row < nY && row < n; row += T)
                        for (int col = 0; col < nX && col < n; ++col)
                            processCell(row, col, hL, cL);
                    hitsSum.fetch_add(hL, std::memory_order_relaxed);
                    cellSum.fetch_add(cL, std::memory_order_relaxed);
                });
            }
            for (auto& th : ths) th.join();
            hits = hitsSum.load(std::memory_order_relaxed);
            cellHits = cellSum.load(std::memory_order_relaxed);
        } else {
            int hL = 0, cL = 0;
            for (int row = 0; row < nY && row < n; ++row)
                for (int col = 0; col < nX && col < n; ++col)
                    processCell(row, col, hL, cL);
            hits = hL; cellHits = cL;
        }
    }

    if (const char* dcell = getenv("GB_VISION_DUMP_CELL")) {
        FILE* fp = fopen(dcell, "wb");
        if (fp) {
            fprintf(fp, "%d %d hits=%d cellHits=%d thr=%.4f half=%d pad=%d\n",
                    n, n, hits, cellHits, thr, half, pad);
            for (int r = 0; r < n; ++r) {
                for (int c = 0; c < n; ++c) fprintf(fp, "%.4f,", cellStat.at<float>(r, c));
                fprintf(fp, "\n");
            }
            for (int r = 0; r < n; ++r) {
                for (int c = 0; c < n; ++c) fprintf(fp, "%.4f,", cellFill.at<float>(r, c));
                fprintf(fp, "\n");
            }
            fclose(fp);
        }
    }

    int cellRescued = 0;
    for (int row = 0; row < n; ++row) {
        for (int col = 0; col < n; ++col) {
            if (board.at<int8_t>(row, col) != EMPTY) continue;
            double lc = cellStat.at<float>(row, col);
            if (std::abs(lc) < CELL_CONTRAST_MIN ||
                cellFill.at<float>(row, col) < CELL_FILL_MIN) continue;
            std::vector<double> qv;
            const float* qp = cellOutq.ptr<float>(row, col);
            for (int k = 0; k < 4; ++k) if (qp[k] < 9000.0) qv.push_back((double)qp[k]);
            bool plateau = plateauLikeMed(cellOut.at<float>(row, col), lc) ||
                           quadsPlateauVals(qv, lc);
            if (plateauGateVeto(plateau, lc, cellEdge.at<float>(row, col))) continue;
            
            {
                if (coloredMarked(iRound(geo.x_lines[col]), iRound(geo.y_lines[row])) && whiteByBody(iRound(geo.x_lines[col]), iRound(geo.y_lines[row])) != WHITE) continue;
                int rcol = rawRefine(geo.x_lines[col], geo.y_lines[row], (lc < 0) ? BLACK : WHITE);
                if (rcol == EMPTY) continue;
                board.at<int8_t>(row, col) = (int8_t)rcol;
                conf.at<float>(row, col) = 0.6f;
                cellRescued++;
            }
        }
    }

    int outlineHits = 0;
    {
        bool anyEmpty = false;
        for (int r = 0; r < n && !anyEmpty; ++r) {
            const int8_t* p = board.ptr<int8_t>(r);
            for (int c = 0; c < n; ++c) if (p[c] == EMPTY) { anyEmpty = true; break; }
        }
        if (anyEmpty) {
            cv::Mat distMap(win, win, CV_32F);
            for (int r = 0; r < win; ++r)
                for (int c = 0; c < win; ++c) {
                    double xx = (double)(c - half), yy = (double)(r - half);
                    distMap.at<float>(r, c) = (float)std::hypot(xx, yy);
                }
            cv::Mat innerB(win, win, CV_8U, cv::Scalar(0)), outRingB(win, win, CV_8U, cv::Scalar(0));
            for (int r = 0; r < win; ++r)
                for (int c = 0; c < win; ++c) {
                    float d = distMap.at<float>(r, c);
                    if (d <= spacing * 0.26) innerB.at<uint8_t>(r, c) = 1;
                    if (d >= spacing * 0.36 && d <= spacing * 0.52) outRingB.at<uint8_t>(r, c) = 1;
                }
            MaskIdx innerM = makeMask(innerB), outRingM = makeMask(outRingB);

            for (int row = 0; row < nY && row < n; ++row) {
                for (int col = 0; col < nX && col < n; ++col) {
                    if (board.at<int8_t>(row, col) != EMPTY) continue;
                    int x = iRound(geo.x_lines[col]);
                    int y = iRound(geo.y_lines[row]);
                    if (!(x >= 0 && x < w && y >= 0 && y < h)) continue;
                    x += pad; y += pad;
                    
                    cv::Mat patch = diff(cv::Rect(x - half, y - half, win, win)).clone();
                    if (patch.rows != innerM.m.rows || patch.cols != innerM.m.cols) continue;

                    std::vector<int16_t> coreV = gatherI16(patch, coreM);
                    double coreMed = coreV.empty() ? 0.0 : median(coreV);
                    int ri = cat(row, nY), ci = cat(col, nX);
                    if (ri > 2) ri = 2;
                    if (ci > 2) ci = 2;
                    std::vector<int16_t> omV = gatherI16(patch, outIdxCache[ri][ci]);
                    std::vector<double> quadMeds;
                    for (const MaskIdx& qm : outQuadCache[ri][ci]) {
                        std::vector<int16_t> qv = gatherI16(patch, qm);
                        if (!qv.empty()) quadMeds.push_back(median(qv));
                    }
                    std::vector<int16_t> e1 = gatherI16(patch, edgeInM);
                    std::vector<int16_t> e2 = gatherI16(patch, edgeOutM);
                    double edge = 0.0;
                    if (!e1.empty() && !e2.empty()) edge = std::abs(median(e1) - median(e2));
                    bool plateau = plateauLikeMed(omV.empty() ? 0.0 : median(omV), coreMed) ||
                                   quadsPlateauVals(quadMeds, coreMed);
                    if (plateauGateVeto(plateau, coreMed, edge)) continue;   

                    if (coloredMarked(x - pad, y - pad) && whiteByBody(x - pad, y - pad) != WHITE) continue;
                    auto dc = decideColor(patch, coreM, ringM, ringSec, thr);
                    if (dc.first != EMPTY) {
                        int refC = rawRefine(x - pad, y - pad, dc.first);
                        if (refC == EMPTY) continue;   
                        board.at<int8_t>(row, col) = (int8_t)refC;
                        conf.at<float>(row, col) = (float)std::min(0.5, dc.second);
                        outlineHits++;
                        continue;
                    }
                    
                    std::vector<int16_t> inner = gatherI16(patch, innerM);
                    std::vector<int16_t> ring = gatherI16(patch, outRingM);
                    if (inner.empty() || ring.empty()) continue;
                    {
                        std::vector<double> av(inner.size());
                        for (size_t i = 0; i < inner.size(); ++i) av[i] = std::abs((double)inner[i]);
                        if (percentile(av, 90) >= thr) continue;   
                    }
                    int dark = 0, bright = 0;
                    for (int16_t v : ring) {
                        if ((double)v < -thr) dark++;
                        if ((double)v > thr) bright++;
                    }
                    double dr = double(dark) / double(ring.size());
                    double br = double(bright) / double(ring.size());
                    if (dr >= 0.45 && br < 0.15) {
                        if (rawRefine(x - pad, y - pad, WHITE) == EMPTY) continue;  
                        board.at<int8_t>(row, col) = WHITE;
                        conf.at<float>(row, col) = 0.45f;
                        outlineHits++;
                    } else if (br >= 0.45 && dr < 0.15) {
                        if (rawRefine(x - pad, y - pad, BLACK) == EMPTY) continue;  
                        board.at<int8_t>(row, col) = BLACK;
                        conf.at<float>(row, col) = 0.45f;
                        outlineHits++;
                    }
                }
            }
        }
    }

    int removed = stripBorderArtifacts(board, &conf);

    // Fixed star points of a 15x15 board: a low-confidence dark blob at a
    // star point is the board marker (possibly occluded by a corner logo),
    // not a stone.
    if (n == 15) {
        // Star points + tengen: a low-confidence dark blob there is the app's
        // cross marker (some machines render it stone-like), not a stone.
        static const int starPts[5][2] = {{3,3},{3,11},{11,3},{11,11},{7,7}};
        for (const auto& sp : starPts) {
            int c = sp[0], r = sp[1];
            if (board.at<int8_t>(r, c) != BLACK) continue;
            if (conf.at<float>(r, c) >= 0.55f) continue;
            board.at<int8_t>(r, c) = EMPTY;
            conf.at<float>(r, c) = 0.f;
        }
    }
    diag = Json::makeObj();
    diag.set("method", Json::makeStr("adaptive"));
    diag.set("threshold", Json::makeNum(npRound(thr * 100.0) / 100.0));
    diag.set("noise_sigma", Json::makeNum(npRound(sigma * 100.0) / 100.0));
    diag.set("cell_contrast_hits", Json::makeInt(cellHits));
    diag.set("cell_rescued", Json::makeInt(cellRescued));
    diag.set("blob_hits", Json::makeInt(hits));
    diag.set("outline_hits", Json::makeInt(outlineHits));
    diag.set("border_artifacts", Json::makeInt(removed));
    Json inv = invariants(board);
    for (auto& kv : inv.obj) diag.set(kv.first, kv.second);
    
}

cv::Mat autoCanny(const cv::Mat& gray, double sigma) {
    std::vector<double> v;
    v.reserve((size_t)gray.rows * gray.cols);
    for (int r = 0; r < gray.rows; ++r) {
        const uint8_t* p = gray.ptr<uint8_t>(r);
        for (int c = 0; c < gray.cols; ++c) v.push_back((double)p[c]);
    }
    double med = median(std::move(v));
    double lo = (double)(int)std::max(0.0, (1.0 - sigma) * med);
    double hi = (double)(int)std::min(255.0, (1.0 + sigma) * med);
    cv::Mat edges;
    cv::Canny(gray, edges, lo, hi, 3);
    return edges;
}

namespace {

bool fitLattice(const std::vector<double>& positions, int count,
                double& oOut, double& dOut, double& residOut) {
    if (positions.size() < 4) return false;
    std::vector<double> p = positions;
    std::sort(p.begin(), p.end());
    double span = p.back() - p.front();
    if (span < 10.0) return false;
    double tol = std::max(2.0, span * 0.01);
    std::vector<double> merged;
    merged.push_back(p[0]);
    for (size_t i = 1; i < p.size(); ++i) {
        if (p[i] - merged.back() <= tol) merged.back() = (merged.back() + p[i]) / 2.0;
        else merged.push_back(p[i]);
    }
    std::vector<double>& m = merged;
    if (m.size() < (size_t)std::max(4, (int)(count * 0.5))) return false;

    std::map<int, int> gapCnt;
    for (size_t i = 1; i < m.size(); ++i) {
        int g = iRound(m[i] - m[i - 1]);
        if (g >= 3) gapCnt[g]++;
    }
    if (gapCnt.empty()) return false;
    int base = 0, bestCnt = -1;
    for (auto& kv : gapCnt)   
        if (kv.second > bestCnt) { bestCnt = kv.second; base = kv.first; }
    double baseD = (double)base;

    bool haveBest = false;
    double bestResid = 0, bestO = 0, bestD = 0;
    for (double o0 : m) {
        std::vector<double> k(m.size());
        for (size_t i = 0; i < m.size(); ++i) k[i] = npRound((m[i] - o0) / baseD);
        
        std::vector<double> ks = k;
        std::sort(ks.begin(), ks.end());
        ks.erase(std::unique(ks.begin(), ks.end()), ks.end());
        if ((double)ks.size() < (double)m.size() * 0.8) continue;
        double slope, intercept;
        polyfit1(k, m, slope, intercept);
        if (slope <= 1.0) continue;
        double s = 0;
        for (size_t i = 0; i < m.size(); ++i)
            s += std::abs(m[i] - (intercept + slope * k[i]));
        double resid = (s / (double)m.size()) / slope;
        if (!haveBest || resid < bestResid) {
            haveBest = true; bestResid = resid; bestO = intercept; bestD = slope;
        }
    }
    if (!haveBest) return false;
    double o = bestO, d = bestD;

    for (int iter = 0; iter < 4; ++iter) {
        std::vector<double> model = ramp(o, d, count);
        std::vector<double> dist(count, 0.0);
        std::vector<int> argminIdx(count, 0);
        for (int i = 0; i < count; ++i) {
            double bd = 1e300; int bi = 0;
            for (size_t j = 0; j < m.size(); ++j) {
                double dd = std::abs(m[j] - model[i]);
                if (dd < bd) { bd = dd; bi = (int)j; }
            }
            dist[i] = bd; argminIdx[i] = bi;
        }
        int selCount = 0;
        for (int i = 0; i < count; ++i) if (dist[i] < d * 0.3) selCount++;
        if ((double)selCount < count * 0.5) return false;
        std::vector<double> selX, claimed;
        std::vector<int> uniq;
        for (int i = 0; i < count; ++i) {
            if (dist[i] < d * 0.3) {
                selX.push_back((double)i);
                claimed.push_back(m[argminIdx[i]]);
                uniq.push_back(argminIdx[i]);
            }
        }
        std::sort(uniq.begin(), uniq.end());
        uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
        if ((double)uniq.size() < (double)selCount * 0.8) return false;
        double slope, intercept;
        polyfit1(selX, claimed, slope, intercept);
        bool converged = (std::abs(slope - d) < 1e-3 && std::abs(intercept - o) < 1e-3);
        o = intercept; d = slope;
        if (converged) break;
    }
    if (d <= 1.0) return false;
    std::vector<double> model = ramp(o, d, count);
    double s = 0;
    for (int i = 0; i < count; ++i) {
        double bd = 1e300, bv = 0;
        for (size_t j = 0; j < m.size(); ++j) {
            double dd = std::abs(m[j] - model[i]);
            if (dd < bd) { bd = dd; bv = m[j]; }
        }
        s += std::abs(bv - model[i]);
    }
    oOut = o; dOut = d; residOut = (s / (double)count) / d;
    return true;
}

double lineEvidence(const cv::Mat& gray, int pos, int axis, double d, int W, int H) {
    int off = std::max(4, (int)(d * 0.3));
    auto colMedian = [&](int x) {
        std::vector<double> v(H);
        for (int y = 0; y < H; ++y) v[y] = (double)gray.at<uint8_t>(y, x);
        return median(std::move(v));
    };
    auto rowMedian = [&](int y) {
        std::vector<double> v(W);
        const uint8_t* p = gray.ptr<uint8_t>(y);
        for (int x = 0; x < W; ++x) v[x] = (double)p[x];
        return median(std::move(v));
    };
    if (axis == 0) {
        if (pos - off < 0 || pos + off >= W) return 0.0;
        double on = colMedian(pos);
        double bg = (colMedian(pos - off) + colMedian(pos + off)) / 2.0;
        return std::abs(on - bg);
    }
    if (pos - off < 0 || pos + off >= H) return 0.0;
    double on = rowMedian(pos);
    double bg = (rowMedian(pos - off) + rowMedian(pos + off)) / 2.0;
    return std::abs(on - bg);
}

double snapLattice(const cv::Mat& gray, double o, double d, int count, int axis,
                   int W, int H, int& strongOut) {
    double span = (double)(axis == 0 ? W : H);
    double center = span / 2.0;
    double total = (count - 1) * d;
    strongOut = 0;
    if (d <= 1.0) return o;
    std::vector<std::array<double, 3>> cands;   
    int tLo = (int)std::floor((-2.0 - o) / d);
    int tHi = (int)std::ceil((span + 2.0 - total - o) / d);
    for (int t = tLo; t <= tHi; ++t) {
        double oo = o + t * d;
        double first = oo, last = oo + d * (count - 1);
        if (first < -2 || last > span + 2) continue;
        int strong = 0;
        for (int i = 0; i < count; ++i) {
            double v = oo + d * i;
            if (lineEvidence(gray, iRound(v), axis, d, W, H) > 6.0) strong++;
        }
        double drift = std::abs((first + last) / 2.0 - center);
        cands.push_back({(double)strong, -drift, oo});
    }
    if (cands.empty()) return o;
    double bestStrong = -1e300;
    for (auto& c : cands) bestStrong = std::max(bestStrong, c[0]);
    std::vector<std::array<double, 3>> tied;
    for (auto& c : cands) if (c[0] >= bestStrong) tied.push_back(c);
    std::stable_sort(tied.begin(), tied.end(),
                     [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
                         return a[1] > b[1];
                     });
    strongOut = (int)tied[0][0];
    return tied[0][2];
}

}  

bool locateGridHough(const cv::Mat& gray, int lineCount,
                     std::vector<double>& xs, std::vector<double>& ys, double& residual) {
    cv::Mat edges = autoCanny(gray);
    int h = gray.rows, w = gray.cols;
    double minLen = std::min(h, w) * 0.35;
    std::vector<cv::Vec4i> segs;
    cv::HoughLinesP(edges, segs, 1, CV_PI / 180.0, 60,
                    (int)minLen, (int)(minLen * 0.04));
    if (segs.empty()) return false;
    std::vector<double> vx, hy;
    for (const cv::Vec4i& s : segs) {
        double x1 = s[0], y1 = s[1], x2 = s[2], y2 = s[3];
        double ang = std::abs(std::atan2(y2 - y1, x2 - x1) * 180.0 / CV_PI);
        if (ang < 6.0) hy.push_back((y1 + y2) / 2.0);
        else if (std::abs(ang - 90.0) < 6.0) vx.push_back((x1 + x2) / 2.0);
    }
    double ox0, dx, rx0, oy0, dy, ry0;
    if (!fitLattice(vx, lineCount, ox0, dx, rx0)) return false;
    if (!fitLattice(hy, lineCount, oy0, dy, ry0)) return false;
    int strongX = 0, strongY = 0;
    double ox = snapLattice(gray, ox0, dx, lineCount, 0, w, h, strongX);
    double oy = snapLattice(gray, oy0, dy, lineCount, 1, w, h, strongY);
    if (std::min(strongX, strongY) < lineCount * 0.5) return false;

    xs = ramp(ox, dx, lineCount);
    ys = ramp(oy, dy, lineCount);
    if (xs.front() < -2 || ys.front() < -2 || xs.back() > w + 2 || ys.back() > h + 2)
        return false;

    auto score = [&](const std::vector<double>& positions, const std::vector<double>& arr,
                     double d) -> std::pair<double, double> {
        if (positions.empty() || d <= 1.0) return {1.0, 0.0};
        std::vector<double> dist(positions.size(), 0.0);
        bool anyHit = false;
        int hit = 0;
        double sum = 0;
        for (size_t i = 0; i < positions.size(); ++i) {
            double bd = 1e300;
            for (double a : arr) bd = std::min(bd, std::abs(positions[i] - a));
            dist[i] = bd;
            if (bd < d * 0.3) { anyHit = true; hit++; sum += bd; }
        }
        double ratio = double(hit) / double(positions.size());
        double resid = anyHit ? (sum / double(hit)) / d : 1.0;
        return {resid, ratio};
    };
    auto sx = score(vx, xs, dx);
    auto sy = score(hy, ys, dy);
    if ((sx.second + sy.second) / 2.0 < 0.5) return false;
    residual = (sx.first + sy.first) / 2.0;
    return true;
}

}  
