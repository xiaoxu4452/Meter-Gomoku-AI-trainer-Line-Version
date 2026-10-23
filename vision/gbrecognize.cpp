/* Vision - boardrecognize. */


#include "gbvision.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <algorithm>

namespace gb {

namespace {

inline int iRound(double v) { return (int)std::nearbyint(v); }

inline double round2(double v) { return std::nearbyint(v * 100.0) / 100.0; }

std::vector<double> refineLines(const cv::Mat& gray, const std::vector<double>& lines,
                                double step, int axis, int W, int H, double polarity) {
    std::vector<double> out;
    int half = std::max(2, (int)(step * 0.30));
    int lo = iRound(lines.front()) - (int)step;
    int hi = iRound(lines.back()) + (int)step;
    for (double p : lines) {
        int p0 = iRound(p);
        int best = p0;
        double bestScore = -1e18;
        for (int d = -half; d <= half; ++d) {
            int pos = p0 + d;
            if (pos < 0 || pos >= (axis == 0 ? W : H)) continue;
            std::vector<double> seg;
            if (axis == 0) {
                int y0 = std::max(0, lo), y1 = std::min(H, hi);
                if (y1 <= y0) continue;
                seg.reserve(y1 - y0);
                for (int y = y0; y < y1; ++y) seg.push_back(gray.at<uint8_t>(y, pos));
            } else {
                int x0 = std::max(0, lo), x1 = std::min(W, hi);
                if (x1 <= x0) continue;
                const uint8_t* row = gray.ptr<uint8_t>(pos);
                for (int x = x0; x < x1; ++x) seg.push_back(row[x]);
            }
            double score = polarity * median(seg);
            if (score > bestScore) { bestScore = score; best = pos; }
        }
        out.push_back((double)best);
    }
    return out;
}

double linePolarity(const cv::Mat& gray, const std::vector<double>& lines, int axis) {
    int H = gray.rows, W = gray.cols;
    double step = lines.size() > 1 ? median(diff(lines)) : 0.0;
    int d = std::max(4, (int)(step * 0.3));
    std::vector<double> votes;
    for (size_t i = 1; i + 1 < lines.size(); ++i) {
        int pos = iRound(lines[i]);
        if (pos - d < 0 || pos + d >= (axis == 0 ? W : H)) continue;
        double off;
        std::vector<double> prof;
        if (axis == 0) {
            std::vector<double> a, b;
            for (int y = 0; y < H; ++y) { a.push_back(gray.at<uint8_t>(y, pos - d)); b.push_back(gray.at<uint8_t>(y, pos + d)); }
            off = (median(a) + median(b)) / 2.0;
            for (int k = -3; k <= 3; ++k) {
                std::vector<double> c;
                for (int y = 0; y < H; ++y) c.push_back(gray.at<uint8_t>(y, pos + k));
                prof.push_back(median(c));
            }
        } else {
            std::vector<double> a, b;
            const uint8_t* r1 = gray.ptr<uint8_t>(pos - d);
            const uint8_t* r2 = gray.ptr<uint8_t>(pos + d);
            for (int x = 0; x < W; ++x) { a.push_back(r1[x]); b.push_back(r2[x]); }
            off = (median(a) + median(b)) / 2.0;
            for (int k = -3; k <= 3; ++k) {
                const uint8_t* r = gray.ptr<uint8_t>(pos + k);
                std::vector<double> c(r, r + W);
                prof.push_back(median(c));
            }
        }
        int best = 0;
        double bestAbs = -1.0;
        for (size_t k = 0; k < prof.size(); ++k) {
            double dv = prof[k] - off;
            if (std::abs(dv) > bestAbs) { bestAbs = std::abs(dv); best = (int)k; }
        }
        double dev = prof[best] - off;
        if (std::abs(dev) > 6.0) votes.push_back(dev > 0 ? 1.0 : -1.0);
    }
    if (votes.empty()) return -1.0;
    return mean(votes) > 0 ? 1.0 : -1.0;
}

bool gridPlausible(const std::vector<double>& xs, const std::vector<double>& ys) {
    for (const std::vector<double>* p : {&xs, &ys}) {
        const std::vector<double>& arr = *p;
        if (arr.size() < 2) return false;
        std::vector<double> d = diff(arr);
        double mn = *std::min_element(d.begin(), d.end());
        double md = median(d);
        if (mn <= 0.5 * md) return false;
        if (stddev(d) / std::max(mean(d), 1e-6) > 0.25) return false;
    }
    return true;
}

struct BoardPair {
    cv::Mat board;      
    bool valid = false;
};

std::string arbitrate(const BoardPair& primary, const BoardPair& secondary, BoardPair& out) {
    if (!primary.valid) { out = secondary; return "legacy"; }
    if (!secondary.valid) { out = primary; return "adaptive"; }
    if (primary.board.rows == secondary.board.rows && primary.board.cols == secondary.board.cols &&
        std::memcmp(primary.board.data, secondary.board.data,
                    (size_t)primary.board.rows * primary.board.cols) == 0) {
        out = primary;
        return "agree";
    }
    auto invOf = [](const cv::Mat& b) {
        Json j = invariants(b);
        return j;
    };
    Json invA = invOf(primary.board), invB = invOf(secondary.board);
    bool susA = invA.find("suspect")->b, susB = invB.find("suspect")->b;
    long long totA = invA.find("total")->inum, totB = invB.find("total")->inum;
    if (susA && !susB) { out = secondary; return "legacy"; }
    out = primary;
    return "adaptive";
}

cv::Mat readStonesRobust(const cv::Mat& gray, const std::vector<double>& xs,
                         const std::vector<double>& ys, int bg, double step) {
    int H = gray.rows, W = gray.cols;
    int ny = (int)ys.size(), nx = (int)xs.size();
    cv::Mat board(ny, nx, CV_8S, cv::Scalar(EMPTY));
    int rw = (int)(step * 0.56);
    int side = 2 * rw + 1;
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            int cx = iRound(xs[i]), cy = iRound(ys[j]);
            int x1p = cx - rw, y1p = cy - rw;
            if (x1p < 0 || y1p < 0 || x1p + side > W || y1p + side > H) continue;
            cv::Mat patch = gray(cv::Rect(x1p, y1p, side, side));
            cv::Mat darkMask(side, side, CV_8U), brightMask(side, side, CV_8U);
            for (int r = 0; r < side; ++r) {
                const uint8_t* p = patch.ptr<uint8_t>(r);
                uint8_t* dp = darkMask.ptr<uint8_t>(r);
                uint8_t* bp = brightMask.ptr<uint8_t>(r);
                for (int c = 0; c < side; ++c) {
                    dp[c] = (p[c] < bg - 25) ? 1 : 0;
                    bp[c] = (p[c] > bg + 15) ? 1 : 0;
                }
            }
            auto largest = [&](const cv::Mat& mask) -> int {
                cv::Mat labels, stats, cents;
                int n = cv::connectedComponentsWithStats(mask, labels, stats, cents, 8, CV_32S);
                if (n <= 1) return 0;
                int mx = 0;
                for (int k = 1; k < n; ++k) mx = std::max(mx, stats.at<int32_t>(k, cv::CC_STAT_AREA));
                return mx;
            };
            int ad = largest(darkMask), ab = largest(brightMask);
            double thr = (double)(side * side) * 0.22;
            if (ad > thr && ad > ab) board.at<int8_t>(j, i) = 1;
            else if (ab > thr && ab > ad) board.at<int8_t>(j, i) = 2;
        }
    }
    return board;
}

Json stonesJson(const cv::Mat& board, int want) {
    Json out = Json::makeArr();
    for (int r = 0; r < board.rows; ++r)
        for (int c = 0; c < board.cols; ++c)
            if (board.at<int8_t>(r, c) == want) {
                Json s = Json::makeObj();
                s.set("x", Json::makeInt(c));
                s.set("y", Json::makeInt(r));
                out.push(s);
            }
    return out;
}

bool decodeImage(const std::string& b64in, cv::Mat& rgb) {
    
    std::string b64 = b64in;
    if (b64.compare(0, 5, "data:") == 0) {
        size_t p = b64.find("base64,");
        if (p != std::string::npos) b64 = b64.substr(p + 7);
    }
    std::vector<uint8_t> buf;
    base64Decode(b64, buf);
    if (buf.empty()) return false;
    cv::Mat raw = cv::imdecode(buf, cv::IMREAD_COLOR);
    if (raw.empty()) return false;
    cv::cvtColor(raw, rgb, cv::COLOR_BGR2RGB);
    return true;
}

void darkDetailProjection(const cv::Mat& gray, int axis, std::vector<double>& out) {
    int H = gray.rows, W = gray.cols;
    double sigma = std::max(2.0, (double)std::min(H, W) / 120.0);
    cv::Mat blur, bf, gf;
    cv::GaussianBlur(gray, blur, cv::Size(0, 0), sigma);
    blur.convertTo(bf, CV_32F);
    gray.convertTo(gf, CV_32F);
    cv::Mat detail;
    cv::subtract(bf, gf, detail);
    if (axis == 0) {
        out.assign(W, 0.0);
        for (int y = 0; y < H; ++y) {
            const float* p = detail.ptr<float>(y);
            for (int x = 0; x < W; ++x) if (p[x] > 0) out[x] += p[x];
        }
        for (auto& v : out) v /= H;
    } else {
        out.assign(H, 0.0);
        for (int y = 0; y < H; ++y) {
            const float* p = detail.ptr<float>(y);
            double s = 0;
            for (int x = 0; x < W; ++x) if (p[x] > 0) s += p[x];
            out[y] = s / W;
        }
    }
}

bool latticeAxis(const std::vector<double>& proj, double& sOut, double& phOut, double& corrOut) {
    int n = (int)proj.size();
    if (n < 48) return false;
    double mu = mean(proj);
    std::vector<double> x(n);
    for (int i = 0; i < n; ++i) x[i] = proj[i] - mu;
    int lo = 8, hi = std::max(9, n / 5);
    double bestCorr = -2.0;
    int bestLag = -1;
    std::vector<double> corr(hi + 1, -2.0);
    for (int lag = lo; lag <= hi; ++lag) {
        double saa = 0, sbb = 0, sab = 0;
        int m = n - lag;
        for (int i = 0; i < m; ++i) {
            saa += x[i] * x[i];
            sbb += x[i + lag] * x[i + lag];
            sab += x[i] * x[i + lag];
        }
        double den = std::sqrt(saa * sbb);
        double c = den > 1e-9 ? sab / den : -2.0;
        corr[lag] = c;
        if (c > bestCorr) { bestCorr = c; bestLag = lag; }
    }
    if (bestLag < 0) return false;
    int pick = bestLag;
    for (int lag = lo; lag <= bestLag; ++lag)
        if (corr[lag] >= 0.85 * bestCorr) { pick = lag; break; }
    int s = pick;
    if (s < 8) return false;
    double bestE = -1e18;
    int phase = 0;
    for (int ph = 0; ph < s; ++ph) {
        double e = 0;
        for (int p = ph; p < n; p += s) e += proj[p];
        if (e > bestE) { bestE = e; phase = ph; }
    }
    sOut = (double)s;
    phOut = (double)phase;
    corrOut = bestCorr;
    return true;
}

std::vector<int> liveLines(const std::vector<double>& proj, int s, int phase) {
    int n = (int)proj.size();
    double base = percentile(proj, 55);
    double spread = std::max(percentile(proj, 95) - base, 0.5);
    int r = std::max(2, (int)(s * 0.12));
    std::vector<int> ks;
    for (int k = 0;; ++k) {
        int p = k * s + phase;
        if (p - r >= n) break;
        int lo = std::max(0, p - r), hi = std::min(n, p + r + 1);
        if (hi <= lo) continue;
        int mp = lo;
        for (int i = lo; i < hi; ++i) if (proj[i] > proj[mp]) mp = i;
        if (proj[mp] > base + 0.6 * spread) ks.push_back(k);
    }
    return ks;
}

bool PartialGridStones(const cv::Mat& rgb, int size, Json& out) {
    try {
        cv::Mat gray;
        cv::cvtColor(rgb, gray, cv::COLOR_RGB2GRAY);
        int H = gray.rows, W = gray.cols;
        std::vector<double> colp, rowp;
        darkDetailProjection(gray, 0, colp);
        darkDetailProjection(gray, 1, rowp);
        double sx, phx, cx, sy, phy, cy;
        if (!latticeAxis(colp, sx, phx, cx) || !latticeAxis(rowp, sy, phy, cy)) return false;
        if (sx < 8 || sy < 8) return false;
        if (std::abs(sx - sy) > 0.25 * std::max(sx, sy)) return false;
        if (cx < 0.30 || cy < 0.30) return false;
        std::vector<int> kxs = liveLines(colp, (int)sx, (int)phx);
        std::vector<int> kys = liveLines(rowp, (int)sy, (int)phy);
        if ((int)kxs.size() < 6 || (int)kys.size() < 6) return false;
        int nx = kxs.back() - kxs.front() + 1;
        int ny = kys.back() - kys.front() + 1;
        if (nx < 6 || ny < 6 || nx > size || ny > size) return false;
        
        int n = std::max(nx, ny);
        GeoLike geo;
        geo.size = n;
        geo.spacing = (sx + sy) / 2.0;
        geo.x_lines.resize(n);
        geo.y_lines.resize(n);
        for (int k = 0; k < n; ++k) {
            geo.x_lines[k] = (kxs.front() + k) * sx + phx;
            geo.y_lines[k] = (kys.front() + k) * sy + phy;
        }
        
        double step = geo.spacing;
        int x0i = std::max(0, (int)geo.x_lines.front() + (int)(step * 2));
        int x1i = std::min(W, (int)geo.x_lines.back() - (int)(step * 2));
        int y0i = std::max(0, (int)geo.y_lines.front() + (int)(step * 2));
        int y1i = std::min(H, (int)geo.y_lines.back() - (int)(step * 2));
        int bg = 200;
        if (x1i > x0i && y1i > y0i) {
            cv::Mat region = gray(cv::Rect(x0i, y0i, x1i - x0i, y1i - y0i));
            std::vector<double> hist = bincount256(region);
            int best = 0;
            for (int i = 1; i < 256; ++i) if (hist[i] > hist[best]) best = i;
            bg = best;
        }
        
        BoardPair pa, pb, picked;
        try {
            cv::Mat conf;
            Json d;
            readStonesAdaptive(rgb, geo, pa.board, conf, d);
            pa.valid = true;
        } catch (...) {
        }
        try {
            cv::Mat conf;
            Geometry g;
            g.x_lines = geo.x_lines;
            g.y_lines = geo.y_lines;
            g.source = "partial_lattice";
            readStonesLegacy(rgb, g, bg, pb.board, conf);
            pb.valid = true;
        } catch (...) {
        }
        arbitrate(pa, pb, picked);
        if (!picked.valid || picked.board.rows != n || picked.board.cols != n) return false;
        cv::Mat svc = boardToService(picked.board);
        int nb = 0, nw = 0;
        int imin = 1 << 30, imax = -(1 << 30), jmin = 1 << 30, jmax = -(1 << 30);
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                int8_t v = svc.at<int8_t>(j, i);
                if (!v) continue;
                if (v == 1) ++nb; else ++nw;
                if (i < imin) imin = i;
                if (i > imax) imax = i;
                if (j < jmin) jmin = j;
                if (j > jmax) jmax = j;
            }
        if (nb + nw < 2 || imax < imin || jmax < jmin) return false;
        if (imax - imin + 1 > size || jmax - jmin + 1 > size) return false;
        if (std::abs(nb - nw) > 3) return out = Json(), false;
        
        cv::Mat boardN(size, size, CV_8S, cv::Scalar(0));
        int offi = (size - (imax - imin + 1)) / 2 - imin;
        int offj = (size - (jmax - jmin + 1)) / 2 - jmin;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                int8_t v = svc.at<int8_t>(j, i);
                if (v) boardN.at<int8_t>(j + offj, i + offi) = v;
            }
        out = Json::makeObj();
        out.set("ok", Json::makeBool(true));
        out.set("size", Json::makeInt(size));
        out.set("partial", Json::makeBool(true));
        out.set("suspect", Json::makeBool(false));
        out.set("black", stonesJson(boardN, 1));
        out.set("white", stonesJson(boardN, 2));
        Json diag = Json::makeObj();
        diag.set("method", Json::makeStr("partial_lattice"));
        out.set("diag", diag);
        return true;
    } catch (...) {
        out = Json();
        return false;
    }
}

bool PartialStonesFallback(const cv::Mat& gray, Json& out) {
    try {
        int H = gray.rows, W = gray.cols;
        double mn = (double)std::min(W, H);
        if (mn < 160) return false;                        
        std::vector<cv::Vec3f> circles;
        cv::HoughCircles(gray, circles, cv::HOUGH_GRADIENT, 1.5, mn / 45.0,
                         110, 0.72, (int)(mn / 50.0), (int)(mn / 10.0));
        if ((int)circles.size() < 2) return false;
        
        std::vector<double> hist = bincount256(gray);
        int bg = 0;
        for (int i = 1; i < 256; ++i) if (hist[i] > hist[bg]) bg = i;
        
        struct Stone { double x, y; int color; };
        std::vector<Stone> st;
        for (const auto& c : circles) {
            float cx = c[0], cy = c[1], rr = c[2];
            int r0 = std::max(1, (int)(rr * 0.5f));
            int x1 = (int)cx - r0, y1 = (int)cy - r0;
            if (x1 < 0 || y1 < 0 || x1 + 2 * r0 + 1 > W || y1 + 2 * r0 + 1 > H) continue;
            cv::Mat patch = gray(cv::Rect(x1, y1, 2 * r0 + 1, 2 * r0 + 1));
            double m = cv::mean(patch)[0];
            int color = 0;
            if (m < bg - 34) color = 1;                    
            else if (m > bg + 26) color = 2;               
            if (color) st.push_back({ cx, cy, color });
        }
        if ((int)st.size() < 2) return false;
        
        std::vector<double> nn;
        for (size_t i = 0; i < st.size(); ++i) {
            double best = 1e18;
            for (size_t k = 0; k < st.size(); ++k) {
                if (k == i) continue;
                double dx = st[i].x - st[k].x, dy = st[i].y - st[k].y;
                double d = std::sqrt(dx * dx + dy * dy);
                if (d < best) best = d;
            }
            if (best < 1e17) nn.push_back(best);
        }
        if (nn.empty()) return false;
        std::vector<double> nns = nn;
        std::sort(nns.begin(), nns.end());
        double med = nns[nns.size() / 2];
        std::vector<double> nn2;
        for (double d : nn) if (d > med * 0.6 && d < med * 1.6) nn2.push_back(d);
        if (nn2.empty()) return false;
        std::sort(nn2.begin(), nn2.end());
        double s = nn2[nn2.size() / 2];
        if (!(s > 8)) return false;
        
        size_t anchor = 0;
        for (size_t i = 1; i < st.size(); ++i)
            if (st[i].x + st[i].y < st[anchor].x + st[anchor].y) anchor = i;
        double ax = st[anchor].x, ay = st[anchor].y;
        cv::Mat board(15, 15, CV_8S, cv::Scalar(0));
        int nb = 0, nw = 0;
        int imin = 1 << 30, imax = -(1 << 30), jmin = 1 << 30, jmax = -(1 << 30);
        for (const auto& p : st) {
            double fi = (p.x - ax) / s, fj = (p.y - ay) / s;
            int i = iRound(fi), j = iRound(fj);
            if (std::abs(fi - i) > 0.38 || std::abs(fj - j) > 0.38) continue;
            
            if (i < 0 || i >= 15 || j < 0 || j >= 15) continue;
            if (board.at<int8_t>(j, i) == 0) {
                board.at<int8_t>(j, i) = (int8_t)p.color;
                if (p.color == 1) ++nb; else ++nw;
                if (i < imin) imin = i;    
                if (i > imax) imax = i;
                if (j < jmin) jmin = j;
                if (j > jmax) jmax = j;
            }
        }
        if (nb + nw < 2 || imax < imin || jmax < jmin) return false;
        if (imax - imin + 1 > 15 || jmax - jmin + 1 > 15) return false;
        if (std::abs(nb - nw) > 3) return out = Json(), false;   
        
        cv::Mat out15(15, 15, CV_8S, cv::Scalar(0));
        int offi = (15 - (imax - imin + 1)) / 2 - imin;
        int offj = (15 - (jmax - jmin + 1)) / 2 - jmin;
        for (int j = jmin; j <= jmax; ++j)
            for (int i = imin; i <= imax; ++i) {
                int8_t v = board.at<int8_t>(j, i);
                if (v) out15.at<int8_t>(j + offj, i + offi) = v;
            }
        out = Json::makeObj();
        out.set("ok", Json::makeBool(true));
        out.set("size", Json::makeInt(15));
        out.set("partial", Json::makeBool(true));
        out.set("suspect", Json::makeBool(false));
        out.set("black", stonesJson(out15, 1));
        out.set("white", stonesJson(out15, 2));
        Json diag = Json::makeObj();
        diag.set("method", Json::makeStr("partial"));
        out.set("diag", diag);
        return true;
    } catch (...) {
        out = Json();
        return false;
    }
}

}  

static bool otsuReadStones(const cv::Mat& gray, const std::vector<double>& xs,
                           const std::vector<double>& ys, double step, cv::Mat& boardOut) {
    try {
        int H = gray.rows, W = gray.cols;
        int n = (int)std::min(xs.size(), ys.size());
        if (n < 5 || !(step > 4)) return false;
        int rd = std::max(2, (int)(step * 0.45));
        int rIn = std::max(3, (int)(step * 0.60));
        int rOut = std::max(4, (int)(step * 0.95));
        std::vector<double> cellC((size_t)n * n, 0.0);
        std::vector<uint8_t> ok((size_t)n * n, 0);
        std::vector<double> all;
        all.reserve((size_t)n * n);
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                int cx = iRound(xs[i]), cy = iRound(ys[j]);
                
                double sum = 0;
                long cnt = 0, full = 0;
                for (int dy = -rd; dy <= rd; ++dy)
                    for (int dx = -rd; dx <= rd; ++dx) {
                        if (dx * dx + dy * dy > rd * rd) continue;
                        ++full;
                        int y = cy + dy, x = cx + dx;
                        if (y < 0 || y >= H || x < 0 || x >= W) continue;
                        sum += gray.at<uint8_t>(y, x);
                        ++cnt;
                    }
                if (full <= 0 || cnt < full / 2) continue;
                double discMean = sum / (double)cnt;
                
                std::vector<double> ring;
                int stride = 1;
                long approx = 0;
                for (int dy = -rOut; dy <= rOut; ++dy)
                    for (int dx = -rOut; dx <= rOut; ++dx) {
                        int d2 = dx * dx + dy * dy;
                        if (d2 < rIn * rIn || d2 > rOut * rOut) continue;
                        ++approx;
                    }
                if (approx > 512) stride = (int)(approx / 512) + 1;
                long taken = 0;
                for (int dy = -rOut; dy <= rOut; dy += stride)
                    for (int dx = -rOut; dx <= rOut; dx += stride) {
                        int d2 = dx * dx + dy * dy;
                        if (d2 < rIn * rIn || d2 > rOut * rOut) continue;
                        int y = cy + dy, x = cx + dx;
                        if (y < 0 || y >= H || x < 0 || x >= W) continue;
                        ring.push_back(gray.at<uint8_t>(y, x));
                        ++taken;
                    }
                if (taken < 16) continue;
                double bg = median(ring);
                double c = bg - discMean;      
                cellC[(size_t)j * n + i] = c;
                ok[(size_t)j * n + i] = 1;
                all.push_back(c);
            }
        }
        if (all.size() < (size_t)n * n * 0.4) return false;
        
        std::vector<double> v = all;
        std::sort(v.begin(), v.end());
        size_t m = v.size();
        std::vector<double> pre(m + 1, 0.0);
        for (size_t k = 0; k < m; ++k) pre[k + 1] = pre[k] + v[k];
        auto segMean = [&](size_t a, size_t b) {   
            return b > a ? (pre[b] - pre[a]) / (double)(b - a) : 0.0;
        };
        double mt = pre[m] / (double)m;
        double bestVar = -1.0;
        double t1 = 0, t2 = 0;
        for (size_t a = 1; a + 1 < m; ++a) {
            if (v[a] - v[a - 1] < 1e-9) continue;
            for (size_t b = a + 1; b < m; ++b) {
                if (v[b] - v[b - 1] < 1e-9) continue;
                double w0 = (double)a, w1 = (double)(b - a), w2 = (double)(m - b);
                double m0 = segMean(0, a), m1 = segMean(a, b), m2 = segMean(b, m);
                double var = w0 * (m0 - mt) * (m0 - mt) +
                             w1 * (m1 - mt) * (m1 - mt) +
                             w2 * (m2 - mt) * (m2 - mt);
                if (var > bestVar) { bestVar = var; t1 = (v[a - 1] + v[a]) / 2.0; t2 = (v[b - 1] + v[b]) / 2.0; }
            }
        }
        if (bestVar <= 0) return false;
        
        cv::Mat board(n, n, CV_8S, cv::Scalar(EMPTY));
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                if (!ok[(size_t)j * n + i]) continue;
                double c = cellC[(size_t)j * n + i];
                if (c > t2) board.at<int8_t>(j, i) = BLACK;
                else if (c < t1) board.at<int8_t>(j, i) = WHITE;
            }
        boardOut = board;
        return true;
    } catch (...) {
        return false;
    }
}

static std::vector<cv::Rect> boardCropCandidates(const cv::Mat& gray) {
    std::vector<cv::Rect> out;
    try {
        int H = gray.rows, W = gray.cols;
        double scale = std::min(1.0, 800.0 / (double)std::max(H, W));
        cv::Mat small;
        if (scale < 1.0)
            cv::resize(gray, small, cv::Size((int)std::nearbyint(W * scale),
                                             (int)std::nearbyint(H * scale)), 0, 0, cv::INTER_AREA);
        else
            small = gray;
        int sh = small.rows, sw = small.cols;
        cv::Mat edges;
        std::vector<std::vector<cv::Point>> cnts;
        cv::Canny(small, edges, 50, 150);
        cv::dilate(edges, edges, cv::getStructuringElement(cv::MORPH_RECT, {3, 3}),
                   cv::Point(-1, -1), 2);
        cv::findContours(edges, cnts, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        double total = (double)sh * sw;
        struct Cand { double area; cv::Rect r; };
        std::vector<Cand> cands;
        for (const auto& c : cnts) {
            cv::Rect b = cv::boundingRect(c);
            if (b.width < 48 || b.height < 48) continue;
            double ar = (double)b.width / (double)b.height;
            if (ar < 0.5 || ar > 2.0) continue;
            double area = (double)b.width * b.height;
            if (area < 0.06 * total || area > 0.97 * total) continue;
            cands.push_back({area, b});
        }
        std::stable_sort(cands.begin(), cands.end(),
                         [](const Cand& a, const Cand& b) { return a.area > b.area; });
        for (size_t i = 0; i < cands.size() && i < 3; ++i) {
            cv::Rect b = cands[i].r;
            int mx = (int)(b.width * 0.04), my = (int)(b.height * 0.04);
            int x0 = std::max(0, (int)((b.x - mx) / scale));
            int y0 = std::max(0, (int)((b.y - my) / scale));
            int x1 = std::min(W, (int)((b.x + b.width + mx) / scale));
            int y1 = std::min(H, (int)((b.y + b.height + my) / scale));
            if (x1 - x0 >= 120 && y1 - y0 >= 120) out.push_back(cv::Rect(x0, y0, x1 - x0, y1 - y0));
        }
    } catch (...) {
    }
    return out;
}

Json recognizeCore(const cv::Mat& img, const cv::Mat& gray, int size) {
    int H = gray.rows, W = gray.cols;

    std::string geoErr;
    std::vector<double> xs, ys;
    bool haveXs = false;
    std::string source = "auto";
    double confidence = 0.0;
    try {
        Geometry g1 = locateBoard(img, size);
        xs = g1.x_lines;
        ys = g1.y_lines;
        haveXs = true;
        source = g1.source;
        confidence = g1.confidence;
    } catch (const std::exception& e) {
        geoErr = std::string("locate: ") + e.what();
    } catch (...) {
        geoErr = "locate: unknown";
    }

    if (!haveXs || !gridPlausible(xs, ys) || confidence < 0.45) {
        std::vector<double> hx, hy;
        double hres = 0;
        bool hOk = false;
        try { hOk = locateGridHough(gray, size, hx, hy, hres); } catch (...) { hOk = false; }
        if (hOk && hres < 0.15) {
            double houghScore = std::min(0.95, 1.0 - hres * 4.0);
            if (!haveXs || !gridPlausible(xs, ys) || houghScore > confidence) {
                xs = hx; ys = hy;
                haveXs = true;
                source = "hough";
                confidence = houghScore;
            }
        }
    }

    if (!haveXs) {
        
        try {
            double sigma = std::max(2.0, (double)std::min(H, W) / 120.0);
            cv::Mat blur, detail;
            cv::GaussianBlur(gray, blur, cv::Size(0, 0), sigma);
            cv::Mat bf, gf;
            blur.convertTo(bf, CV_32F);
            gray.convertTo(gf, CV_32F);
            cv::absdiff(bf, gf, detail);
            cv::Mat hp(H, W, CV_8U);
            for (int r = 0; r < H; ++r) {
                const float* dp = detail.ptr<float>(r);
                uint8_t* op = hp.ptr<uint8_t>(r);
                for (int c = 0; c < W; ++c) {
                    double v = 255.0 - (double)dp[c] * 3.0;
                    if (v < 0) v = 0;
                    if (v > 255) v = 255;
                    op[c] = (uint8_t)v;
                }
            }
            cv::Mat hpRgb;
            cv::cvtColor(hp, hpRgb, cv::COLOR_GRAY2RGB);
            Geometry gh = locateBoard(hpRgb, size);
            xs = gh.x_lines;
            ys = gh.y_lines;
            haveXs = true;
            source = "hp";
            confidence = gh.confidence;
        } catch (...) {}
    }
    if (!haveXs) {
        
        Json p;
        if (PartialGridStones(img, size, p)) return p;
        
        if (PartialStonesFallback(gray, p)) return p;
        Json r = Json::makeObj();
        r.set("ok", Json::makeBool(false));
        r.set("err", Json::makeStr(geoErr.empty() ? "locate failed" : geoErr));
        return r;
    }

    double sx = median(diff(xs));
    double sy = median(diff(ys));
    double xs0 = xs.front(), ys0 = ys.front(), step0 = (sx + sy) / 2.0;

    double polX = linePolarity(gray, xs, 0);
    double polY = linePolarity(gray, ys, 1);
    for (int round = 0; round < 3; ++round) {
        std::vector<double> nx = refineLines(gray, xs, sx, 0, W, H, polX);
        std::vector<double> ny = refineLines(gray, ys, sy, 1, W, H, polY);
        
        if (!gridPlausible(nx, ny) || std::abs(nx.front() - xs0) > step0 * 0.35 ||
            std::abs(ny.front() - ys0) > step0 * 0.35)
            break;
        xs = nx;
        ys = ny;
        sx = median(diff(xs));
        sy = median(diff(ys));
    }
    double step = (sx + sy) / 2;
    if (!(step > 0)) {
        Json r = Json::makeObj();
        r.set("ok", Json::makeBool(false));
        r.set("err", Json::makeStr("locate failed"));
        return r;
    }

    std::string realignNote;
    bool clippedHint = false;
    try {
        std::vector<double> colp, rowp;
        darkDetailProjection(gray, 0, colp);
        darkDetailProjection(gray, 1, rowp);
        
        auto dropPhantom = [&](std::vector<double>& lines, int imgSpan, const char* tag) {
            if (lines.size() < 6) return;
            std::vector<double> d = diff(lines);
            double md = median(d);
            double first = d.front(), last = d.back();
            
            auto endPeak = [&](size_t idx) {
                int p0 = iRound(lines[idx]);
                int r = (int)(step * 0.25);
                double be = 0;
                for (int dd = -r; dd <= r; ++dd) {
                    int p = p0 + dd;
                    if (p < 0 || p >= imgSpan) continue;
                    double v = (imgSpan == W ? colp[p] : rowp[p]);
                    if (v > be) be = v;
                }
                return be;
            };
            std::vector<double> peaks;
            for (size_t i = 1; i + 1 < lines.size(); ++i) peaks.push_back(endPeak(i));
            double medPeak = peaks.empty() ? 0.0 : median(peaks);
            
            bool lastBad = (std::abs(last - md) > 0.15 * md &&
                            d.size() > 1 && std::abs(last - d[d.size() - 2]) > 0.12 * md) ||
                           (medPeak > 1e-6 && endPeak(lines.size() - 1) < medPeak * 0.4) ||
                           (d.size() > 1 && std::abs(last - d[d.size() - 2]) > 0.06 * md &&
                            medPeak > 1e-6 && endPeak(lines.size() - 1) < medPeak * 0.7);
            bool firstBad = (std::abs(first - md) > 0.15 * md &&
                             d.size() > 1 && std::abs(first - d[1]) > 0.12 * md) ||
                            (medPeak > 1e-6 && endPeak(0) < medPeak * 0.4) ||
                            (d.size() > 1 && std::abs(first - d[1]) > 0.06 * md &&
                             medPeak > 1e-6 && endPeak(0) < medPeak * 0.7);
            if (lastBad == firstBad) return;       
            std::vector<double> tmp = lines;
            if (lastBad) tmp.pop_back(); else tmp.erase(tmp.begin());
            if (tmp.size() < 5) return;
            double m2 = median(diff(tmp));
            bool clipFront = tmp.front() < m2 * 0.75;   
            double repl = clipFront ? tmp.front() - m2 : tmp.back() + m2;
            bool replOutside = clipFront ? (repl < 0) : (repl > imgSpan - 2);
            if (!replOutside) return;              
            lines = tmp;
            if (clipFront) lines.insert(lines.begin(), repl);
            else lines.push_back(repl);
            realignNote += tag;
            clippedHint = true;
        };
        dropPhantom(xs, W, "phantom_x");
        dropPhantom(ys, H, "phantom_y");
        if (clippedHint) {
            sx = median(diff(xs));
            sy = median(diff(ys));
            step = (sx + sy) / 2;
        }
        auto axisEnergy = [&](const std::vector<double>& lines, const std::vector<double>& proj) {
            double e = 0;
            for (double L : lines) {
                int p = iRound(L);
                if (p < 0 || p >= (int)proj.size()) continue;
                e += proj[p];
            }
            return e;
        };
        auto snapLines = [&](const std::vector<double>& lines, const std::vector<double>& proj) {
            std::vector<double> out;
            int r = (int)(step * 0.35);
            for (double L : lines) {
                int p0 = iRound(L), best = p0;
                double be = -1e18;
                for (int d = -r; d <= r; ++d) {
                    int p = p0 + d;
                    if (p < 0 || p >= (int)proj.size()) continue;
                    if (proj[p] > be) { be = proj[p]; best = p; }
                }
                out.push_back((double)best);
            }
            return out;
        };
        std::vector<double> nx = snapLines(xs, colp), ny = snapLines(ys, rowp);
        double e0 = axisEnergy(xs, colp) + axisEnergy(ys, rowp);
        double e1 = axisEnergy(nx, colp) + axisEnergy(ny, rowp);
        if (e1 > e0 * 1.06 + 1e-9 && gridPlausible(nx, ny) &&
            std::abs(nx.front() - xs.front()) <= step * 0.35 && std::abs(nx.back() - xs.back()) <= step * 0.35 &&
            std::abs(ny.front() - ys.front()) <= step * 0.35 && std::abs(ny.back() - ys.back()) <= step * 0.35) {
            xs = nx;
            ys = ny;
            sx = median(diff(xs));
            sy = median(diff(ys));
            step = (sx + sy) / 2;
            realignNote = "snap";
        }
        auto axisBest = [&](const std::vector<double>& lines, const std::vector<double>& proj,
                            double& bestOff, double& curE, double& bestE) {
            int lo = -(int)std::floor(step * 0.40), hi = (int)std::floor(step * 0.40);
            curE = bestE = 0;
            bestOff = 0;
            for (int d = lo; d <= hi; ++d) {
                double e = 0;
                for (double L : lines) {
                    int p = iRound(L) + d;
                    if (p < 0 || p >= (int)proj.size()) continue;
                    e += proj[p];
                }
                if (d == 0) curE = e;
                if (e > bestE) { bestE = e; bestOff = (double)d; }
            }
        };
        double bdx = 0, bdy = 0, ex0 = 0, exb = 0, ey0 = 0, eyb = 0;
        axisBest(xs, colp, bdx, ex0, exb);
        axisBest(ys, rowp, bdy, ey0, eyb);
        if (exb > ex0 * 1.08 + 1e-9 && std::abs(bdx) >= 1) {
            for (auto& v : xs) v += bdx;
            sx = median(diff(xs));
            realignNote += "x" + std::to_string((int)bdx);
        }
        if (eyb > ey0 * 1.08 + 1e-9 && std::abs(bdy) >= 1) {
            for (auto& v : ys) v += bdy;
            sy = median(diff(ys));
            realignNote += "y" + std::to_string((int)bdy);
        }
        step = (sx + sy) / 2;
    } catch (...) {
    }

    std::vector<int> xsI(xs.size()), ysI(ys.size());
    for (size_t i = 0; i < xs.size(); ++i) xsI[i] = iRound(xs[i]);
    for (size_t i = 0; i < ys.size(); ++i) ysI[i] = iRound(ys[i]);
    int x0i = std::max(0, xsI.front() + (int)(step * 2));
    int x1i = std::min(W, xsI.back() - (int)(step * 2));
    int y0i = std::max(0, ysI.front() + (int)(step * 2));
    int y1i = std::min(H, ysI.back() - (int)(step * 2));
    int bg = 200;
    if (x1i > x0i && y1i > y0i) {
        cv::Mat region = gray(cv::Rect(x0i, y0i, x1i - x0i, y1i - y0i));
        std::vector<double> hist = bincount256(region);
        int best = 0;
        for (int i = 1; i < 256; ++i) if (hist[i] > hist[best]) best = i;
        bg = best;
    }

    auto readFull = [&](const std::vector<double>& xsIn, const std::vector<double>& ysIn,
                        BoardPair& pickedOut, Json& diagOut, long long& totOut) -> bool {
        double stIn = (median(diff(xsIn)) + median(diff(ysIn))) / 2.0;
        int bx0 = std::max(0, iRound(xsIn.front()) + (int)(stIn * 2));
        int bx1 = std::min(W, iRound(xsIn.back()) - (int)(stIn * 2));
        int by0 = std::max(0, iRound(ysIn.front()) + (int)(stIn * 2));
        int by1 = std::min(H, iRound(ysIn.back()) - (int)(stIn * 2));
        int bgIn = 200;
        if (bx1 > bx0 && by1 > by0) {
            cv::Mat region = gray(cv::Rect(bx0, by0, bx1 - bx0, by1 - by0));
            std::vector<double> hist = bincount256(region);
            int best = 0;
            for (int i = 1; i < 256; ++i) if (hist[i] > hist[best]) best = i;
            bgIn = best;
        }
        diagOut = Json::makeObj();
        GeoLike gf;
        gf.size = size;
        gf.x_lines = xsIn;
        gf.y_lines = ysIn;
        gf.spacing = stIn;
        BoardPair pa, pb;
        try {
            cv::Mat conf;
            Json d;
            readStonesAdaptive(img, gf, pa.board, conf, d);
            pa.valid = true;
            diagOut = d;
        } catch (const std::exception& e) {
            diagOut.set("method", Json::makeStr("adaptive"));
            diagOut.set("err", Json::makeStr(e.what()));
        }
        try {
            cv::Mat conf;
            readStonesLegacy(img, [&] {
                Geometry g;
                g.x_lines = xsIn; g.y_lines = ysIn; g.source = source; g.confidence = confidence;
                return g;
            }(), bgIn, pb.board, conf);
            pb.valid = true;
        } catch (const std::exception& e) {
            diagOut.set("legacy_err", Json::makeStr(e.what()));
        }
        std::string who = arbitrate(pa, pb, pickedOut);
        diagOut.set("picked", Json::makeStr(who));
        if (!pickedOut.valid) return false;
        Json invL = invariants(pickedOut.board);
        
        if (invL.find("suspect") && invL.find("suspect")->b) {
            cv::Mat ob;
            if (otsuReadStones(gray, xsIn, ysIn, stIn, ob)) {
                Json inv2 = invariants(ob);
                bool sus2 = inv2.find("suspect") && inv2.find("suspect")->b;
                long long tot2 = inv2.find("total") ? inv2.find("total")->inum : 0;
                if (!sus2 && tot2 > 0) {
                    pickedOut.board = ob;
                    invL = inv2;
                    diagOut.set("method", Json::makeStr("otsu"));
                }
            }
        }
        for (auto& kv : invL.obj) diagOut.set(kv.first, kv.second);
        totOut = invL.find("total") ? invL.find("total")->inum : 0;
        return true;
    };

    Json diag = Json::makeObj();
    BoardPair picked;
    long long totRead = 0;
    if (!readFull(xs, ys, picked, diag, totRead)) {
        Json r = Json::makeObj();
        r.set("ok", Json::makeBool(false));
        const Json* e = diag.find("err");
        r.set("err", Json::makeStr(e && e->type == Json::STR ? e->str : "read failed"));
        return r;
    }

    try {
        long long gate = totRead + std::max<long long>(2, (long long)(totRead * 0.15));
        int bsx = 0, bsy = 0;
        long long bestCnt = totRead;
        for (int sy2 = -1; sy2 <= 1; ++sy2)
            for (int sx2 = -1; sx2 <= 1; ++sx2) {
                if (!sx2 && !sy2) continue;
                std::vector<double> xs2(xs.size()), ys2(ys.size());
                for (size_t i = 0; i < xs.size(); ++i) xs2[i] = xs[i] + sx2 * sx;
                for (size_t i = 0; i < ys.size(); ++i) ys2[i] = ys[i] + sy2 * sy;
                cv::Mat ob;
                if (!otsuReadStones(gray, xs2, ys2, step, ob)) continue;
                long long cnt = 0, nb = 0, nw = 0;
                for (int j = 0; j < ob.rows; ++j)
                    for (int i = 0; i < ob.cols; ++i) {
                        int8_t v = ob.at<int8_t>(j, i);
                        if (!v) continue;
                        ++cnt;
                        if (v == BLACK) ++nb; else ++nw;
                    }
                
                if (cnt > 0 && std::abs(nb - nw) > std::max<long long>(4, (long long)(cnt * 0.2))) continue;
                if (cnt > bestCnt) { bestCnt = cnt; bsx = sx2; bsy = sy2; }
            }
        if ((bsx || bsy) && bestCnt >= gate) {
            std::vector<double> xs2(xs.size()), ys2(ys.size());
            for (size_t i = 0; i < xs.size(); ++i) xs2[i] = xs[i] + bsx * sx;
            for (size_t i = 0; i < ys.size(); ++i) ys2[i] = ys[i] + bsy * sy;
            BoardPair p2;
            Json d2;
            long long t2 = 0;
            bool sus2 = false;
            if (readFull(xs2, ys2, p2, d2, t2) && t2 > totRead) {
                const Json* s2 = d2.find("suspect");
                sus2 = s2 && s2->type == Json::BOOL && s2->b;
                if (!sus2) {                       
                    picked = p2;
                    diag = d2;
                    xs = xs2;
                    ys = ys2;
                    sx = median(diff(xs));
                    sy = median(diff(ys));
                    step = (sx + sy) / 2;
                    xsI.assign(xs.size(), 0);
                    ysI.assign(ys.size(), 0);
                    for (size_t i = 0; i < xs.size(); ++i) xsI[i] = iRound(xs[i]);
                    for (size_t i = 0; i < ys.size(); ++i) ysI[i] = iRound(ys[i]);
                    diag.set("method", Json::makeStr("realigned"));
                }
            }
        }
    } catch (...) {
    }
    if (!realignNote.empty()) diag.set("realign", Json::makeStr(realignNote));

    Json inv = invariants(picked.board);

    Json out = Json::makeObj();
    out.set("ok", Json::makeBool(true));
    out.set("size", Json::makeInt(size));
    out.set("suspect", Json::makeBool(inv.find("suspect")->b));
    if (clippedHint) {
        
        out.set("suspect", Json::makeBool(false));
        out.set("partial", Json::makeBool(true));
    }
    out.set("diag", diag);

    Json gj = Json::makeObj();
    gj.set("x0", Json::makeInt(xsI.front()));
    gj.set("y0", Json::makeInt(ysI.front()));
    gj.set("stepX", Json::makeNum(round2(sx)));
    gj.set("stepY", Json::makeNum(round2(sy)));
    Json xa = Json::makeArr(), ya = Json::makeArr();
    for (int v : xsI) xa.push(Json::makeInt(v));
    for (int v : ysI) ya.push(Json::makeInt(v));
    gj.set("xs", xa);
    gj.set("ys", ya);
    gj.set("source", Json::makeStr(source));
    gj.set("confidence", Json::makeNum(confidence));
    gj.set("bg", Json::makeInt(bg));
    out.set("geometry", gj);

    cv::Mat svc = boardToService(picked.board);
    out.set("black", stonesJson(svc, 1));
    out.set("white", stonesJson(svc, 2));
    return out;
}

namespace {

struct RecogCache {
    static constexpr int CAP = 8;
    struct Ent { uint64_t key = 0; std::string json; uint64_t tick = 0; };
    std::mutex mu;
    std::vector<Ent> v;
    uint64_t tick = 0;
    bool get(uint64_t key, Json& out) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& e : v) {
            if (e.key != key) continue;
            e.tick = ++tick;
            return jsonParse(e.json, out);      
        }
        return false;
    }
    void put(uint64_t key, const Json& j) {
        std::lock_guard<std::mutex> lk(mu);
        std::string s = j.dump();
        if ((int)v.size() < CAP) v.push_back({key, std::move(s), ++tick});
        else {
            auto oldest = std::min_element(v.begin(), v.end(),
                [](const Ent& a, const Ent& b) { return a.tick < b.tick; });
            *oldest = Ent{key, std::move(s), ++tick};
        }
    }
};

inline uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= (uint64_t)c; h *= 1099511628211ull; }
    return h;
}

} 

static Json recognizeUncached(const std::string& imageB64, int size, bool allowSnap);  

Json recognize(const std::string& imageB64, int size, bool allowSnap) {
    static RecogCache g_cache;   
    
    const uint64_t key = fnv1a(imageB64)
                       ^ (uint64_t)size * 0x9E3779B97F4A7C15ull
                       ^ (allowSnap ? 0xA5A5A5A5A5A5A5A5ull : 0ull);
    {
        Json cached;
        if (g_cache.get(key, cached)) return cached;
    }
    Json res = recognizeUncached(imageB64, size, allowSnap);
    if (res.boolOr("ok", false)) g_cache.put(key, res);   
    return res;
}

static Json recognizeUncached(const std::string& imageB64, int size, bool allowSnap) {
    cv::Mat img;
    if (!decodeImage(imageB64, img)) {
        Json r = Json::makeObj();
        r.set("ok", Json::makeBool(false));
        r.set("err", Json::makeStr("decode failed"));
        return r;
    }
    cv::Mat gray;
    cv::cvtColor(img, gray, cv::COLOR_RGB2GRAY);
    Json res = recognizeCore(img, gray, size);
    
    auto stonesEmpty = [](const Json& r) -> bool {
        int n = 0;
        if (const Json* b = r.find("black"); b && b->type == Json::ARR) n += (int)b->arr.size();
        if (const Json* w = r.find("white"); w && w->type == Json::ARR) n += (int)w->arr.size();
        return n == 0;
    };
    if (!res.boolOr("ok", false)) return res;
    if (!res.boolOr("suspect", false) && !stonesEmpty(res)) return res;
    int H = gray.rows, W = gray.cols;

    auto latticeCoverage = [](const Json& r, int w, int h) -> double {
        const Json* geo = r.find("geometry");
        if (!geo || geo->type != Json::OBJ) return 0.0;
        const Json* xs = geo->find("xs");
        const Json* ys = geo->find("ys");
        if (!xs || !ys || xs->type != Json::ARR || ys->type != Json::ARR ||
            xs->arr.size() < 2 || ys->arr.size() < 2) return 0.0;
        double cx = (double)(xs->arr.back().inum - xs->arr.front().inum) / std::max(1, w);
        double cy = (double)(ys->arr.back().inum - ys->arr.front().inum) / std::max(1, h);
        return std::min(cx, cy);
    };
    double cov = latticeCoverage(res, W, H);
    {
        Json* dg = res.findMutable("diag");
        if (dg) dg->set("coverage", Json::makeNum(round2(cov)));
    }
    if (!allowSnap) {                                  
        Json* dg = res.findMutable("diag");
        if (dg) dg->set("snap", Json::makeStr("off"));
    }
    bool skipCrops = !allowSnap;

    if (!skipCrops) {
    auto tryCrop = [&](int x0, int y0, int x1, int y1, const char* method,
                       bool requireStones = false) -> Json {
        x0 = std::max(0, x0); y0 = std::max(0, y0);
        x1 = std::min(W, x1); y1 = std::min(H, y1);
        if (x1 - x0 < 120 || y1 - y0 < 120) return Json();
        cv::Mat crop = img(cv::Rect(x0, y0, x1 - x0, y1 - y0)).clone();
        cv::Mat cg;
        cv::cvtColor(crop, cg, cv::COLOR_RGB2GRAY);
        Json sub = recognizeCore(crop, cg, size);
        if (sub.boolOr("ok", false) && !sub.boolOr("suspect", false) &&
            !sub.boolOr("partial", false) &&
            (!requireStones || !stonesEmpty(sub))) {
            Json diag;
            if (const Json* d = sub.find("diag"); d && d->type == Json::OBJ) diag = *d;
            else diag = Json::makeObj();
            diag.set("method", Json::makeStr(method));
            Json rect = Json::makeArr();
            rect.push(Json::makeInt(x0)); rect.push(Json::makeInt(y0));
            rect.push(Json::makeInt(x1 - x0)); rect.push(Json::makeInt(y1 - y0));
            diag.set("crop_rect", rect);
            sub.set("diag", diag);
            return sub;
        }
        return Json();
    };

    if (stonesEmpty(res)) {
        try {
            cv::Mat big;
            cv::resize(img, big, cv::Size(W * 2, H * 2), 0, 0, cv::INTER_CUBIC);
            cv::Mat bgray;
            cv::cvtColor(big, bgray, cv::COLOR_RGB2GRAY);
            for (const BoardRectCand& c : snapBoardRects(bgray, size, 30)) {
                double pad = (c.step > 0 ? c.step : 20.0) * 0.6;
                Json sub = tryCrop((int)(c.x0 - pad) / 2, (int)(c.y0 - pad) / 2,
                                   (int)(c.x1 + pad) / 2, (int)(c.y1 + pad) / 2,
                                   "snap_rect_big", true);
                if (sub.type == Json::OBJ) return sub;
            }
        } catch (...) {}
    }

    try {
        for (const BoardRectCand& c : snapBoardRects(gray, size, 3)) {
            
            double pad = (c.step > 0 ? c.step : 20.0) * 0.6;
            Json sub = tryCrop((int)(c.x0 - pad), (int)(c.y0 - pad),
                               (int)(c.x1 + pad), (int)(c.y1 + pad), "snap_rect", stonesEmpty(res));
            if (sub.type == Json::OBJ) return sub;
        }
    } catch (...) {}

    for (const cv::Rect& r : boardCropCandidates(gray)) {
        Json sub = tryCrop(r.x, r.y, r.x + r.width, r.y + r.height, "crop_quad", stonesEmpty(res));
        if (sub.type == Json::OBJ) return sub;
    }
    }   

    Json p;
    if (PartialGridStones(img, size, p)) return p;
    if (PartialStonesFallback(gray, p)) return p;
    return res;
}

Json recognizeWulin(const Json& geo, const Json& win, int size) {
    cv::Mat img;
    int ox = 0, oy = 0;
    if (!captureScreen(img, ox, oy)) {
        Json r = Json::makeObj();
        r.set("ok", Json::makeBool(false));
        r.set("err", Json::makeStr("grab: capture failed"));
        return r;
    }
    int gh = img.rows, gw = img.cols;
    
    double iw = win.numOr("innerWidth", 0);
    double ih = win.numOr("innerHeight", 0);
    if (iw <= 0) iw = gw;
    if (ih <= 0) ih = gh;
    double scaleX = iw > 100 ? (double)gw / iw : 1.0;
    double scaleY = ih > 100 ? (double)gh / ih : scaleX;
    double sxWin = win.numOr("screenX", 0);
    double syWin = win.numOr("screenY", 0);
    double tb = win.numOr("topBorder", 0);
    double gx0 = geo.numOr("x0", 0);
    double gy0 = geo.numOr("y0", 0);
    double gStep = geo.numOr("stepX", 0);
    if (gStep == 0) gStep = geo.numOr("step", 50);
    int grid = geo.numOr("grid", 15) > 0 ? (int)geo.numOr("grid", 15) : 15;

    double x0 = (gx0 + sxWin + 8.0) * scaleX;
    double y0 = (gy0 + syWin + std::max(0.0, tb - 8.0)) * scaleY;
    double sp = gStep * scaleX;
    int n = size > 0 ? size : grid;
    std::vector<double> xs(n), ys(n);
    for (int i = 0; i < n; ++i) { xs[i] = x0 + i * sp; ys[i] = y0 + i * sp; }

    cv::Mat gray;
    cv::cvtColor(img, gray, cv::COLOR_RGB2GRAY);
    int H = gray.rows, W = gray.cols;
    int x0i = std::max(0, (int)xs.front() - (int)(sp * 0.5));
    int x1i = std::min(W, (int)xs.back() + (int)(sp * 0.5));
    int y0i = std::max(0, (int)ys.front() - (int)(sp * 0.5));
    int y1i = std::min(H, (int)ys.back() + (int)(sp * 0.5));
    int bg = 200;
    if (x1i > x0i && y1i > y0i) {
        cv::Mat region = gray(cv::Rect(x0i, y0i, x1i - x0i, y1i - y0i));
        std::vector<double> hist = bincount256(region);
        int best = 0;
        for (int i = 1; i < 256; ++i) if (hist[i] > hist[best]) best = i;
        bg = best;
    }

    GeoLike gf;
    gf.size = n;
    gf.spacing = sp;
    gf.x_lines = xs;
    gf.y_lines = ys;

    BoardPair pa, pb, picked;
    try {
        cv::Mat conf;
        Json d;
        readStonesAdaptive(img, gf, pa.board, conf, d);
        pa.valid = true;
    } catch (...) { pa.valid = false; }
    try {
        Geometry g;
        g.x_lines = xs; g.y_lines = ys; g.source = "wulin"; g.confidence = 1.0;
        cv::Mat conf;
        readStonesLegacy(img, g, bg, pb.board, conf);
        pb.valid = true;
    } catch (...) { pb.valid = false; }
    std::string who = arbitrate(pa, pb, picked);
    (void)who;
    cv::Mat board;
    if (picked.valid) board = picked.board;
    else board = readStonesRobust(gray, xs, ys, bg, sp);

    Json out = Json::makeObj();
    out.set("ok", Json::makeBool(true));
    out.set("size", Json::makeInt(size));
    cv::Mat svc = boardToService(board);
    out.set("black", stonesJson(svc, 1));
    out.set("white", stonesJson(svc, 2));
    return out;
}

}  
