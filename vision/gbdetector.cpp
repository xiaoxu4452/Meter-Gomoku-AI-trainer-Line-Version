// Meter Gomoku Link — v4.43
/* Vision - boarddetector. */


#include "gbvision.h"

#include <cmath>
#include <limits>

namespace gb {

namespace {

inline double npRound(double v) { return std::nearbyint(v); }
inline int iRound(double v) { return (int)std::nearbyint(v); }

using Rect4 = cv::Rect;

std::vector<int> profilePeaks(const std::vector<double>& prof) {
    std::vector<int> pos;
    if (prof.empty()) return pos;
    double m = *std::max_element(prof.begin(), prof.end());
    if (m < 8) return pos;
    double thr = std::max(8.0, 0.12 * m);
    for (size_t i = 1; i + 1 < prof.size(); ++i)
        if (prof[i] >= thr && prof[i] >= prof[i - 1] && prof[i] >= prof[i + 1])
            pos.push_back((int)i);
    return pos;
}

std::vector<double> cluster1d(const std::vector<int>& positions, double tol = 3.0) {
    std::vector<double> centers;
    if (positions.empty()) return centers;
    std::vector<double> run{(double)positions[0]};
    for (size_t i = 1; i < positions.size(); ++i) {
        double v = (double)positions[i];
        if (v - run.back() <= tol) run.push_back(v);
        else {
            double s = 0;
            for (double x : run) s += x;
            centers.push_back(s / (double)run.size());
            run.assign(1, v);
        }
    }
    double s = 0;
    for (double x : run) s += x;
    centers.push_back(s / (double)run.size());
    return centers;
}

bool axisLattice(const std::vector<double>& prof, int lineCount,
                 double& firstOut, double& lastOut, double& stepOut, int& inlOut) {
    int n = (int)prof.size();
    if (n < lineCount * 4) return false;
    std::vector<double> centers = cluster1d(profilePeaks(prof));
    if ((int)centers.size() < lineCount) return false;

    std::vector<int> keys;
    std::vector<int> counts;
    std::map<int, int> pos2slot;
    double maxStep = n / std::max(4.0, (double)lineCount / 2.0);
    for (size_t i = 0; i + 1 < centers.size(); ++i)
        for (size_t j = i + 1; j < centers.size(); ++j) {
            double d = centers[j] - centers[i];
            if (d >= 8.0 && d <= maxStep) {
                int key = iRound(d);
                auto it = pos2slot.find(key);
                if (it == pos2slot.end()) {
                    pos2slot[key] = (int)keys.size();
                    keys.push_back(key);
                    counts.push_back(1);
                } else counts[it->second]++;
            }
        }
    if (keys.empty()) return false;

    std::vector<int> order(keys.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return counts[a] > counts[b]; });

    bool haveBest = false;
    std::tuple<int, double> bestScore;
    std::vector<double> bestIdx;
    double bestStep = 0;
    int bestInl = 0;

    for (size_t oi = 0; oi < order.size() && oi < 3; ++oi) {
        int base = keys[order[oi]];
        double weight = 0, wsum = 0;
        for (size_t i = 0; i < keys.size(); ++i)
            if (std::abs(keys[i] - base) <= 1) {
                weight += counts[i];
                wsum += (double)keys[i] * counts[i];
            }
        if (weight <= 0) continue;
        double step = wsum / weight;
        if (step < 6) continue;
        double t = std::max(2.0, step * 0.12);
        double span = (lineCount - 1) * step;
        bool haveK = false;
        std::tuple<int, double> kScore;
        std::vector<double> kIdx;
        int kInl = 0;
        for (double c : centers) {
            for (int k = 0; k < lineCount; ++k) {
                double first = c - k * step;
                if (first < 0 || first + span > n - 1) continue;
                std::vector<double> idx = ramp(first, step, lineCount);
                int inl = 0;
                for (double v : idx) {
                    
                    int j = (int)(std::lower_bound(centers.begin(), centers.end(), v) - centers.begin());
                    if (j < 1) j = 1;
                    if (j > (int)centers.size() - 1) j = (int)centers.size() - 1;
                    double dj = std::abs(centers[j] - v);
                    double dj1 = std::abs(centers[j - 1] - v);
                    double chosen = (dj < dj1) ? centers[j] : centers[j - 1];
                    if (std::abs(chosen - v) <= t) inl++;
                }
                if (inl < lineCount - 3) continue;
                std::vector<double> amps;
                amps.reserve(idx.size());
                for (double v : idx) {
                    int ii = iRound(v);
                    if (ii < 0) ii = 0;
                    if (ii > n - 1) ii = n - 1;
                    amps.push_back(prof[ii]);
                }
                std::tuple<int, double> score{inl, median(amps)};
                if (!haveK || score > kScore) {
                    haveK = true; kScore = score; kIdx = idx; kInl = inl;
                }
            }
        }
        if (haveK && (!haveBest || kScore > bestScore)) {
            haveBest = true;
            bestScore = kScore;
            bestIdx = kIdx;
            bestStep = step;
            bestInl = kInl;
        }
    }
    if (!haveBest) return false;
    firstOut = bestIdx.front();
    lastOut = bestIdx.back();
    stepOut = bestStep;
    inlOut = bestInl;
    return true;
}

bool axisLatticeStrip(const std::vector<double>& prof, int lineCount, double stepHint,
                      double& firstOut, double& lastOut, double& stepOut, int& inlOut) {
    int n = (int)prof.size();
    if (n < lineCount * 4 || stepHint < 6) return false;
    
    std::vector<double> ps(n, 0.0);
    for (int i = 0; i < n; ++i) {
        double s = 0;
        for (int j = 0; j < 5; ++j) {
            int k = i + j - 2;
            if (k >= 0 && k < n) s += prof[k];
        }
        ps[i] = s / 5.0;
    }
    bool have = false;
    double bestScore = -1.0, bestFirst = 0, bestStep = 0;
    for (double step : linspace(stepHint * 0.92, stepHint * 1.08, 33)) {
        double span = (lineCount - 1) * step;
        if (span > n - 1) continue;
        double stop = (double)n - 1.0 - span;
        if (stop <= 0) continue;
        int cnt = (int)std::ceil(stop / 0.5);   
        for (int a = 0; a < cnt; ++a) {
            double first = 0.5 * (double)a;
            double sum = 0;
            for (int i = 0; i < lineCount; ++i) {
                double x = first + step * i;
                
                int i0 = (int)std::floor(x);
                double frac = x - i0;
                double v;
                if (i0 < 0) v = ps[0];
                else if (i0 >= n - 1) v = ps[n - 1];
                else v = ps[i0] * (1.0 - frac) + ps[i0 + 1] * frac;
                sum += v;
            }
            double score = sum / (double)lineCount;
            if (score > bestScore) { bestScore = score; bestFirst = first; bestStep = step; have = true; }
        }
    }
    if (!have) return false;
    firstOut = bestFirst;
    lastOut = bestFirst + (lineCount - 1) * bestStep;
    stepOut = bestStep;
    inlOut = lineCount;
    return true;
}

int localMode(const cv::Mat& gray) { return globalMode(gray); }

std::vector<double> axisProjection(const cv::Mat& gray, int axis) {
    int h = gray.rows, w = gray.cols;
    double sigma = std::max(2.0, (double)std::min(h, w) / 120.0);
    cv::Mat blur;
    cv::GaussianBlur(gray, blur, cv::Size(0, 0), sigma);
    cv::Mat detail(h, w, CV_32F);
    for (int r = 0; r < h; ++r) {
        const uint8_t* gp = gray.ptr<uint8_t>(r);
        const uint8_t* bp = blur.ptr<uint8_t>(r);
        float* dp = detail.ptr<float>(r);
        for (int c = 0; c < w; ++c) {
            double d = (double)bp[c] - (double)gp[c];
            dp[c] = (float)(d > 0 ? d : 0.0);
        }
    }
    std::vector<double> out;
    if (axis == 0) {
        out.assign(w, 0.0);
        for (int r = 0; r < h; ++r) {
            const float* dp = detail.ptr<float>(r);
            for (int c = 0; c < w; ++c) out[c] += dp[c];
        }
        for (int c = 0; c < w; ++c) out[c] /= (double)h;
    } else {
        out.assign(h, 0.0);
        for (int r = 0; r < h; ++r) {
            const float* dp = detail.ptr<float>(r);
            double s = 0;
            for (int c = 0; c < w; ++c) s += dp[c];
            out[r] = s / (double)w;
        }
    }
    return out;
}

bool snapToPeaks(const std::vector<double>& projection, const std::vector<double>& positions,
                 int radius, std::vector<double>& adjusted, std::vector<double>& peaks) {
    int length = (int)projection.size();
    adjusted.clear(); peaks.clear();
    for (double position : positions) {
        int center = iRound(position);
        int lo = std::max(0, center - radius), hi = std::min(length, center + radius + 1);
        if (hi <= lo) return false;
        int off = 0;
        double bv = projection[lo];
        for (int i = lo + 1; i < hi; ++i)
            if (projection[i] > bv) { bv = projection[i]; off = i - lo; }
        
        double pos = (double)(lo + off);
        if (off > 0 && lo + off < hi - 1) {
            double pa = projection[lo + off - 1], pb = projection[lo + off], pc = projection[lo + off + 1];
            double den = pa - 2.0 * pb + pc;
            if (den < -1e-12) {
                double delta = 0.5 * (pa - pc) / den;
                if (delta >= -0.5 && delta <= 0.5) pos += delta;
            }
        }
        adjusted.push_back(pos);
        peaks.push_back(bv);
    }
    return true;
}

double spacingCv(const std::vector<double>& positions) {
    std::vector<double> sp = diff(positions);
    if (sp.empty()) return 1e9;
    double m = mean(sp);
    if (m <= 0) return 1e9;
    return stddev(sp) / m;
}

std::vector<double> reanchorRegular(const std::vector<double>& projection,
                                    const std::vector<double>& positions, int radius,
                                    double baseline, double spread) {
    int lineCount = (int)positions.size();
    if (lineCount < 3) return positions;
    double step = (positions.back() - positions.front()) / (double)(lineCount - 1);
    if (step <= 0) return positions;
    int length = (int)projection.size();

    struct Cand { double strength; double cv; std::vector<double> snapped; };
    std::vector<Cand> cands;
    for (double k : {0.0, -1.0, 1.0}) {
        std::vector<double> p2(positions.size());
        for (size_t i = 0; i < positions.size(); ++i) p2[i] = positions[i] + k * step;
        std::vector<double> snapped, peaks;
        if (!snapToPeaks(projection, p2, radius, snapped, peaks)) continue;
        if (snapped.front() < 0 || snapped.back() > length - 1) continue;
        bool inc = true;
        for (size_t i = 1; i < snapped.size(); ++i)
            if (snapped[i] - snapped[i - 1] <= 0) { inc = false; break; }
        if (!inc) continue;
        std::vector<double> rel(peaks.size());
        for (size_t i = 0; i < peaks.size(); ++i) rel[i] = (peaks[i] - baseline) / spread;
        cands.push_back({median(rel), spacingCv(snapped), snapped});
    }
    if (cands.empty()) return positions;
    double bestStrength = -1e300;
    for (auto& c : cands) bestStrength = std::max(bestStrength, c.strength);
    std::vector<Cand> viable;
    for (auto& c : cands) if (c.strength >= 0.70 * bestStrength) viable.push_back(c);
    if (viable.empty()) viable = cands;
    std::stable_sort(viable.begin(), viable.end(),
                     [](const Cand& a, const Cand& b) { return a.cv < b.cv; });
    return viable[0].snapped;
}

std::vector<double> fitAxis(const std::vector<double>& projection, int lineCount,
                            double& scoreOut) {
    int length = (int)projection.size();
    if (length < lineCount * 3) throw DetectionError("候选区域太小，无法拟合棋盘网格");
    bool have = false;
    std::vector<double> bestPositions;
    double bestScore = -1.0;
    int radius = std::max(1, (int)(length / lineCount / 7));
    double baseline = percentile(projection, 55);
    double spread = std::max(percentile(projection, 95) - baseline, 0.5);

    for (double sf : linspace(0.015, 0.13, 10)) {
        for (double ef : linspace(0.87, 0.985, 10)) {
            std::vector<double> positions =
                linspace(sf * (length - 1), ef * (length - 1), lineCount);
            double spacing = positions[1] - positions[0];
            if (spacing < 4) continue;
            std::vector<double> adjusted, peaks;
            if (!snapToPeaks(projection, positions, radius, adjusted, peaks)) continue;
            double devSum = 0;
            for (size_t i = 0; i < adjusted.size(); ++i)
                devSum += std::abs(adjusted[i] - positions[i]);
            double regularity =
                std::max(0.0, 1.0 - (devSum / (double)adjusted.size()) / std::max(radius + 0.5, 1.0));
            std::vector<double> rel(peaks.size());
            for (size_t i = 0; i < peaks.size(); ++i) rel[i] = (peaks[i] - baseline) / spread;
            double peakStrength = median(rel);
            double score = 0.78 * peakStrength + 0.22 * regularity;
            if (score > bestScore) {
                bestScore = score;
                bestPositions = adjusted;
                have = true;
            }
        }
    }
    if (!have) throw DetectionError("没有找到等距网格");
    bestPositions = reanchorRegular(projection, bestPositions, radius, baseline, spread);
    scoreOut = std::min(1.0, std::max(0.0, bestScore));
    return bestPositions;
}

double axisRegularity(const std::vector<double>& lines) {
    std::vector<double> spacings = diff(lines);
    if (spacings.empty()) return 0.0;
    for (double s : spacings) if (s <= 0) return 0.0;
    double meanSpacing = mean(spacings);
    if (meanSpacing <= 0) return 0.0;
    std::vector<double> ideal = linspace(lines.front(), lines.back(), (int)lines.size());
    double sq = 0;
    for (size_t i = 0; i < lines.size(); ++i) sq += (lines[i] - ideal[i]) * (lines[i] - ideal[i]);
    double residual = std::sqrt(sq / (double)lines.size()) / meanSpacing;
    double spacingCvV = stddev(spacings) / meanSpacing;
    return std::min(1.0, std::max(0.0, 1.0 - std::max(residual / 0.20, spacingCvV / 0.28)));
}

void gridMetrics(const std::vector<double>& xLines, const std::vector<double>& yLines,
                 int cw, int ch, double& aspect, double& regularity, double& extent) {
    double xSpan = xLines.back() - xLines.front();
    double ySpan = yLines.back() - yLines.front();
    if (xSpan <= 0 || ySpan <= 0) { aspect = regularity = extent = 0.0; return; }
    aspect = std::min(xSpan / ySpan, ySpan / xSpan);
    regularity = std::min(axisRegularity(xLines), axisRegularity(yLines));
    extent = std::min(xSpan / std::max(cw - 1, 1), ySpan / std::max(ch - 1, 1));
    aspect = std::min(1.0, std::max(0.0, aspect));
    regularity = std::min(1.0, std::max(0.0, regularity));
    extent = std::min(1.0, std::max(0.0, extent));
}

std::vector<double> periodLockAxis(const std::vector<double>& projection, int lineCount,
                                   double& scoreOut) {
    std::vector<double> prof(projection.size());
    double pm = mean(projection);
    for (size_t i = 0; i < projection.size(); ++i) prof[i] = projection[i] - pm;
    int n = (int)prof.size();
    double expected = (double)n / (double)(lineCount - 1);
    double lo = std::max(6.0, expected * 0.72), hi = expected * 1.35;
    double bestScore = -1.0;
    std::vector<double> bestPos = linspace(0, n - 1, lineCount);
    for (double step : linspace(lo, hi, 260)) {
        int k = iRound(step);
        if (k < 6) continue;
        double prox = std::exp(-std::pow((step - expected) / std::max(expected * 0.30, 1.0), 2.0));
        for (int phase = 0; phase < k; ++phase) {
            std::vector<double> idx(lineCount);
            for (int i = 0; i < lineCount; ++i) idx[i] = npRound((double)i * step + phase);
            if (idx.front() < 0 || idx.back() >= n) continue;
            double s = 0;
            for (double v : idx) s += prof[(int)v];
            double score = s * (0.55 + 0.45 * prox);
            if (score > bestScore) { bestScore = score; bestPos = idx; }
        }
    }
    double span = std::max(bestPos.back() - bestPos.front(), 1.0);
    double norm = bestScore / std::max(span, 1.0);
    scoreOut = std::min(1.0, std::max(0.0, norm / std::max(stddev(prof) + 1e-3, 1e-3)));
    return bestPos;
}

Geometry locateStretched(const cv::Mat& rgb, const cv::Rect& rect, int lineCount) {
    int x1 = rect.x, y1 = rect.y, x2 = rect.x + rect.width - 1, y2 = rect.y + rect.height - 1;
    cv::Mat crop = rgb(cv::Rect(x1, y1, x2 - x1 + 1, y2 - y1 + 1));
    cv::Mat gray, blur, dark;
    cv::cvtColor(crop, gray, cv::COLOR_RGB2GRAY);
    cv::GaussianBlur(gray, blur, cv::Size(0, 0), 5);
    cv::Mat diff32;
    cv::subtract(blur, gray, diff32);   
    cv::Mat clipped;
    cv::max(diff32, 0.0, clipped);
    clipped.convertTo(clipped, CV_8U);
    cv::GaussianBlur(clipped, dark, cv::Size(0, 0), 3);

    std::vector<double> rowp = axisProjection(dark, 1);
    std::vector<double> colp = axisProjection(dark, 0);

    double yScore = 0;
    std::vector<double> yPos = periodLockAxis(rowp, lineCount, yScore);
    double yBase = percentile(rowp, 55);
    double ySpread = std::max(percentile(rowp, 95) - yBase, 0.5);
    yPos = reanchorRegular(rowp, yPos, std::max(1, (int)((int)rowp.size() / lineCount / 7)),
                           yBase, ySpread);
    double ystep = (yPos.back() - yPos.front()) / (double)(lineCount - 1);

    std::vector<double> xp(colp.size());
    double cm = mean(colp);
    for (size_t i = 0; i < colp.size(); ++i) xp[i] = colp[i] - cm;
    double expected = ystep;
    double bestScore = -1e9;
    std::vector<double> bestX = linspace(0, (int)colp.size() - 1, lineCount);
    for (double step : linspace(expected * 0.62, expected * 1.18, 160)) {
        int k = iRound(step);
        if (k < 6) continue;
        for (int phase = 0; phase < k; ++phase) {
            std::vector<double> idx(lineCount);
            for (int i = 0; i < lineCount; ++i) idx[i] = npRound((double)i * step + phase);
            if (idx.front() < 0 || idx.back() >= (int)xp.size()) continue;
            std::vector<double> vals;
            vals.reserve(idx.size());
            for (double v : idx) vals.push_back(xp[(int)v]);
            double score = median(vals);
            if (score > bestScore) { bestScore = score; bestX = idx; }
        }
    }
    double xBase = percentile(colp, 55);
    double xSpread = std::max(percentile(colp, 95) - xBase, 0.5);
    bestX = reanchorRegular(colp, bestX, std::max(1, (int)((int)colp.size() / lineCount / 7)),
                            xBase, xSpread);

    Geometry g;
    g.x_lines.resize(bestX.size());
    for (size_t i = 0; i < bestX.size(); ++i) g.x_lines[i] = bestX[i] + x1;
    g.y_lines.resize(yPos.size());
    for (size_t i = 0; i < yPos.size(); ++i) g.y_lines[i] = yPos[i] + y1;
    double cov = std::max(stddev(colp) + 1e-3, 1e-3);
    g.confidence = std::min(1.0, std::max(0.0,
        std::min(yScore, 0.6 + bestScore / cov * 0.1)));
    g.source = "auto";
    return g;
}

std::vector<cv::Rect> dedupeRectangles(const std::vector<cv::Rect>& in) {
    std::vector<cv::Rect> sorted = in;
    std::stable_sort(sorted.begin(), sorted.end(), [](const cv::Rect& a, const cv::Rect& b) {
        return (long long)a.width * a.height > (long long)b.width * b.height;
    });
    std::vector<cv::Rect> result;
    for (const cv::Rect& r : sorted) {
        int x = r.x, y = r.y, w = r.width, h = r.height;
        bool keep = true;
        for (const cv::Rect& o : result) {
            int ix1 = std::max(x, o.x), iy1 = std::max(y, o.y);
            int ix2 = std::min(x + w, o.x + o.width), iy2 = std::min(y + h, o.y + o.height);
            long long inter = (long long)std::max(0, ix2 - ix1) * std::max(0, iy2 - iy1);
            long long uni = (long long)w * h + (long long)o.width * o.height - inter;
            if (uni && (double)inter / (double)uni > 0.82) { keep = false; break; }
        }
        if (keep) result.push_back(r);
        if (result.size() >= 20) break;
    }
    return result;
}

std::vector<cv::Rect> latticeRectangles(const cv::Mat& rgb, int lineCount) {
    int height = rgb.rows, width = rgb.cols;
    cv::Mat gray, small;
    cv::cvtColor(rgb, gray, cv::COLOR_RGB2GRAY);
    double scale = std::min(1.0, 1400.0 / std::max(height, width));
    if (scale < 1.0) {
        cv::resize(gray, small, cv::Size(), scale, scale, cv::INTER_AREA);
    } else small = gray;

    int smin = std::min(small.rows, small.cols);
    int block = std::max(15, iRound((double)smin / 45.0) | 1);
    cv::Mat binary;
    cv::adaptiveThreshold(small, binary, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          cv::THRESH_BINARY_INV, block, 5);
    int lineLen = std::max(12, smin / 65);
    cv::Mat hor, ver;
    cv::morphologyEx(binary, hor, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(lineLen, 1)));
    cv::morphologyEx(binary, ver, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(1, lineLen)));

    std::vector<double> rowProf(small.rows, 0.0), colProf(small.cols, 0.0);
    for (int r = 0; r < small.rows; ++r) {
        const uint8_t* p = hor.ptr<uint8_t>(r);
        double s = 0;
        for (int c = 0; c < small.cols; ++c) if (p[c]) s += 1.0;
        rowProf[r] = s;
    }
    for (int c = 0; c < small.cols; ++c) {
        double s = 0;
        for (int r = 0; r < small.rows; ++r) if (ver.at<uint8_t>(r, c)) s += 1.0;
        colProf[c] = s;
    }

    double ryF = 0, ryL = 0, ryS = 0; int ryI = 0;
    double rxF = 0, rxL = 0, rxS = 0; int rxI = 0;
    bool ryOk = axisLattice(rowProf, lineCount, ryF, ryL, ryS, ryI);
    bool rxOk = axisLattice(colProf, lineCount, rxF, rxL, rxS, rxI);

    auto stepsDisagree = [](bool aOk, double aStep, bool bOk, double bStep) {
        if (!aOk || !bOk) return false;
        return std::abs(aStep - bStep) / std::max(aStep, bStep) > 0.12;
    };
    if (stepsDisagree(ryOk, ryS, rxOk, rxS)) {
        if (rxOk && (!ryOk || rxI >= ryI)) {
            int off = std::max(0, (int)rxF - 2);
            int x1 = (int)rxL + 1;
            if (x1 > small.cols) x1 = small.cols;
            std::vector<double> strip(small.rows, 0.0);
            for (int r = 0; r < small.rows; ++r) {
                double s = 0;
                const uint8_t* p = binary.ptr<uint8_t>(r);
                for (int c = off; c < x1; ++c) if (p[c]) s += 1.0;
                strip[r] = s;
            }
            double f = 0, l = 0, st = 0; int inl = 0;
            if (axisLatticeStrip(strip, lineCount, rxS, f, l, st, inl)) {
                ryOk = true; ryF = f; ryL = l; ryS = st; ryI = inl;
            }
        } else if (ryOk) {
            int off = std::max(0, (int)ryF - 2);
            int y1 = (int)ryL + 1;
            if (y1 > small.rows) y1 = small.rows;
            std::vector<double> strip(small.cols, 0.0);
            for (int c = 0; c < small.cols; ++c) {
                double s = 0;
                for (int r = off; r < y1; ++r) if (binary.at<uint8_t>(r, c)) s += 1.0;
                strip[c] = s;
            }
            double f = 0, l = 0, st = 0; int inl = 0;
            if (axisLatticeStrip(strip, lineCount, ryS, f, l, st, inl)) {
                rxOk = true; rxF = f; rxL = l; rxS = st; rxI = inl;
            }
        }
    } else if (!rxOk && ryOk) {
        int off = std::max(0, (int)ryF - 2);
        int y1 = (int)ryL + 1;
        if (y1 > small.rows) y1 = small.rows;
        std::vector<double> strip(small.cols, 0.0);
        for (int c = 0; c < small.cols; ++c) {
            double s = 0;
            for (int r = off; r < y1; ++r) if (binary.at<uint8_t>(r, c)) s += 1.0;
            strip[c] = s;
        }
        double f = 0, l = 0, st = 0; int inl = 0;
        if (axisLatticeStrip(strip, lineCount, ryS, f, l, st, inl)) {
            rxOk = true; rxF = f; rxL = l; rxS = st; rxI = inl;
        }
    } else if (!ryOk && rxOk) {
        int off = std::max(0, (int)rxF - 2);
        int x1 = (int)rxL + 1;
        if (x1 > small.cols) x1 = small.cols;
        std::vector<double> strip(small.rows, 0.0);
        for (int r = 0; r < small.rows; ++r) {
            double s = 0;
            const uint8_t* p = binary.ptr<uint8_t>(r);
            for (int c = off; c < x1; ++c) if (p[c]) s += 1.0;
            strip[r] = s;
        }
        double f = 0, l = 0, st = 0; int inl = 0;
        if (axisLatticeStrip(strip, lineCount, rxS, f, l, st, inl)) {
            ryOk = true; ryF = f; ryL = l; ryS = st; ryI = inl;
        }
    }
    if (!ryOk || !rxOk) return {};

    double x0 = rxF, x1v = rxL, stepX = rxS;
    double y0 = ryF, y1v = ryL, stepY = ryS;
    double spanX = x1v - x0, spanY = y1v - y0;
    if (std::min(spanX, spanY) <= 0 || std::min(spanX, spanY) / std::max(spanX, spanY) < 0.65)
        return {};
    int margin = iRound(std::max(stepX, stepY) * 0.8) + 6;
    int x0f = (int)std::max(0.0, (x0 - margin) / scale);
    int y0f = (int)std::max(0.0, (y0 - margin) / scale);
    int x1f = (int)std::min((double)(width - 1), (x1v + margin) / scale);
    int y1f = (int)std::min((double)(height - 1), (y1v + margin) / scale);
    if (x1f - x0f < 60 || y1f - y0f < 60) return {};
    return {cv::Rect(x0f, y0f, x1f - x0f + 1, y1f - y0f + 1)};
}

std::vector<cv::Rect> candidateRectangles(const cv::Mat& rgb, int lineCount) {
    const bool trace = getenv("GB_VISION_TRACE") != nullptr;
    int height = rgb.rows, width = rgb.cols;
    double minArea = (double)height * width * 0.012;
    if (trace) fprintf(stderr, "[trace] candRects img=%dx%d minArea=%.0f\n", width, height, minArea);
    cv::Mat bgr, gray, small;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    double scale = std::min(1.0, 1400.0 / std::max(height, width));
    if (scale < 1.0) cv::resize(gray, small, cv::Size(), scale, scale, cv::INTER_AREA);
    else small = gray;

    int smin = std::min(small.rows, small.cols);
    int block = std::max(15, iRound((double)smin / 45.0) | 1);
    cv::Mat binary;
    cv::adaptiveThreshold(small, binary, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          cv::THRESH_BINARY_INV, block, 5);
    int lineLen = std::max(12, smin / 65);
    cv::Mat horizontal, vertical;
    cv::morphologyEx(binary, horizontal, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(lineLen, 1)));
    cv::morphologyEx(binary, vertical, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(1, lineLen)));
    cv::Mat grid, orv;
    cv::bitwise_or(horizontal, vertical, orv);
    cv::dilate(orv, grid, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5)),
                cv::Point(-1, -1), 1);

    std::vector<cv::Rect> rects;
    {
        std::vector<std::vector<cv::Point>> conts;
        std::vector<cv::Vec4i> hier;
        cv::findContours(grid, conts, hier, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        for (auto& c : conts) {
            cv::Rect bb = cv::boundingRect(c);
            int x = iRound(bb.x / scale), y = iRound(bb.y / scale);
            int w = iRound(bb.width / scale), h = iRound(bb.height / scale);
            if ((double)w * h < minArea || !(0.68 <= (double)w / std::max(h, 1) &&
                                             (double)w / std::max(h, 1) <= 1.47)) continue;
            int pad = iRound(std::max(w, h) * 0.07);
            rects.emplace_back(std::max(0, x - pad), std::max(0, y - pad),
                               std::min(width - x + pad, w + 2 * pad),
                               std::min(height - y + pad, h + 2 * pad));
        }
    }

    cv::Mat hsv;
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
    auto scanMask = [&](const cv::Mat& mask, double fillMin, std::vector<cv::Rect>& out) {
        std::vector<std::vector<cv::Point>> conts;
        std::vector<cv::Vec4i> hier;
        cv::findContours(const_cast<cv::Mat&>(mask), conts, hier,
                         cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        for (auto& c : conts) {
            cv::Rect bb = cv::boundingRect(c);
            int x = bb.x, y = bb.y, w = bb.width, h = bb.height;
            double fill = std::abs(cv::contourArea(c)) / std::max(w * h, 1);
            if ((double)w * h >= minArea && fill > fillMin &&
                0.68 <= (double)w / std::max(h, 1) && (double)w / std::max(h, 1) <= 1.47)
                out.emplace_back(x, y, w, h);
        }
    };

    {
        cv::Mat warm;
        cv::inRange(hsv, cv::Scalar(4, 20, 45), cv::Scalar(48, 255, 255), warm);
        int ks = std::max(9, std::min(height, width) / 90);
        cv::morphologyEx(warm, warm, cv::MORPH_CLOSE,
                         cv::getStructuringElement(cv::MORPH_RECT, cv::Size(ks, ks)));
        scanMask(warm, 0.55, rects);
    }
    {
        
        cv::Mat bw;
        cv::inRange(hsv, cv::Scalar(4, 20, 135), cv::Scalar(48, 255, 255), bw);
        int sk = std::max(5, std::min(height, width) / 180);
        cv::morphologyEx(bw, bw, cv::MORPH_CLOSE,
                         cv::getStructuringElement(cv::MORPH_RECT, cv::Size(sk, sk)));
        cv::morphologyEx(bw, bw, cv::MORPH_OPEN,
                         cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));
        scanMask(bw, 0.72, rects);
    }
    {
        
        cv::Mat ls;
        cv::inRange(hsv, cv::Scalar(10, 22, 200), cv::Scalar(32, 100, 255), ls);
        int lk = std::max(5, std::min(height, width) / 200);
        cv::morphologyEx(ls, ls, cv::MORPH_CLOSE,
                         cv::getStructuringElement(cv::MORPH_RECT, cv::Size(lk, lk)));
        scanMask(ls, 0.70, rects);
    }

    try {
        std::vector<cv::Rect> lat = latticeRectangles(rgb, lineCount);
        if (trace) fprintf(stderr, "[trace]   lattice=%d\n", (int)lat.size());
        rects.insert(rects.end(), lat.begin(), lat.end());
    } catch (...) {
        
        if (trace) fprintf(stderr, "[trace]   lattice threw\n");
    }
    if (trace) fprintf(stderr, "[trace]   total rects=%d (after dedupe)\n",
                       (int)dedupeRectangles(rects).size());
    return dedupeRectangles(rects);
}

double axisLiveness(const cv::Mat& gray, const std::vector<double>& lines,
                    int start, int end, double spacing, bool vertical) {
    if (lines.size() < 5 || spacing < 4 || start > end) return 0.0;
    int height = gray.rows, width = gray.cols;
    int half = std::max(0, std::min(1, iRound(spacing * 0.035)));
    int flankOffset = std::max(8, iRound(spacing * 0.42));
    int searchRadius = std::max(1, std::min(6, iRound(spacing * 0.16)));

    std::vector<double> contrasts;
    std::vector<int> offsets;
    for (double line : lines) {
        int expected = iRound(line);
        double bestContrast = -1.0;
        int bestOffset = INT_MIN;
        for (int off = -searchRadius; off <= searchRadius; ++off) {
            int point = expected + off;
            std::vector<double> core, left, right;
            if (vertical) {
                if (point - half < 0 || point + half >= width ||
                    point - flankOffset - half < 0 || point + flankOffset + half >= width)
                    continue;
                for (int y = start; y <= end; ++y) {
                    for (int x = point - half; x <= point + half; ++x)
                        core.push_back(gray.at<uint8_t>(y, x));
                    for (int x = point - flankOffset - half; x <= point - flankOffset + half; ++x)
                        left.push_back(gray.at<uint8_t>(y, x));
                    for (int x = point + flankOffset - half; x <= point + flankOffset + half; ++x)
                        right.push_back(gray.at<uint8_t>(y, x));
                }
            } else {
                if (point - half < 0 || point + half >= height ||
                    point - flankOffset - half < 0 || point + flankOffset + half >= height)
                    continue;
                for (int x = start; x <= end; ++x) {
                    for (int y = point - half; y <= point + half; ++y)
                        core.push_back(gray.at<uint8_t>(y, x));
                    for (int y = point - flankOffset - half; y <= point - flankOffset + half; ++y)
                        left.push_back(gray.at<uint8_t>(y, x));
                    for (int y = point + flankOffset - half; y <= point + flankOffset + half; ++y)
                        right.push_back(gray.at<uint8_t>(y, x));
                }
            }
            std::vector<double> flank = left;
            flank.insert(flank.end(), right.begin(), right.end());
            double contrast = std::abs(median(flank) - median(core));
            
            if (contrast > bestContrast + 1e-6 ||
                (std::abs(contrast - bestContrast) <= 1e-6 &&
                 (bestOffset == INT_MIN || std::abs(off) < std::abs(bestOffset)))) {
                bestContrast = contrast;
                bestOffset = off;
            }
        }
        if (bestOffset != INT_MIN) {
            contrasts.push_back(bestContrast);
            offsets.push_back(bestOffset);
        }
    }
    if ((int)contrasts.size() < std::max(5, iRound((double)lines.size() * 0.8))) return 0.0;

    std::vector<double> offVals;
    if (offsets.size() > 4) {
        for (size_t i = 1; i + 1 < offsets.size(); ++i) offVals.push_back((double)offsets[i]);
    } else {
        for (int v : offsets) offVals.push_back((double)v);
    }
    std::vector<double> absOff(offVals.size());
    for (size_t i = 0; i < offVals.size(); ++i) absOff[i] = std::abs((double)offVals[i]);

    double strength = std::min(1.0, std::max(0.0, (median(contrasts) - 8.0) / 24.0));
    int hitCnt = 0;
    for (double c : contrasts) if (c >= 12.0) hitCnt++;
    double hitRatio = (double)hitCnt / (double)contrasts.size();
    double align = std::min(1.0, std::max(0.0,
        1.0 - std::max(median(absOff) / 4.0, percentile(absOff, 90) / 6.0)));
    return std::min(1.0, std::max(0.0, (0.70 * strength + 0.30 * hitRatio) * align));
}

}  

double gridLiveness(const cv::Mat& rgb, const Geometry& geo) {
    if (rgb.empty() || rgb.channels() != 3) return 0.0;
    if (geo.x_lines.size() != geo.y_lines.size() || geo.x_lines.size() < 5) return 0.0;
    double spacing = geo.spacing();
    if (!std::isfinite(spacing)) return 0.0;
    int height = rgb.rows, width = rgb.cols;
    int xStart = iRound(geo.x_lines.front()), xEnd = iRound(geo.x_lines.back());
    int yStart = iRound(geo.y_lines.front()), yEnd = iRound(geo.y_lines.back());
    if (!(0 <= xStart && xStart < xEnd && xEnd < width &&
          0 <= yStart && yStart < yEnd && yEnd < height)) return 0.0;
    cv::Mat gray;
    cv::cvtColor(rgb, gray, cv::COLOR_RGB2GRAY);
    double xs = axisLiveness(gray, geo.x_lines, yStart, yEnd, spacing, true);
    double ys = axisLiveness(gray, geo.y_lines, xStart, xEnd, spacing, false);
    return std::min(xs, ys);
}

Geometry geometryFromRect(const cv::Mat& rgb, int x1, int y1, int x2, int y2,
                          int lineCount, const std::string& source, bool lock) {
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    x1 = std::max(0, x1); x2 = std::min(rgb.cols - 1, x2);
    y1 = std::max(0, y1); y2 = std::min(rgb.rows - 1, y2);
    if (x2 - x1 < lineCount * 4 || y2 - y1 < lineCount * 4)
        throw DetectionError("框选区域过小");
    cv::Mat crop = rgb(cv::Rect(x1, y1, x2 - x1 + 1, y2 - y1 + 1));
    cv::Mat gray;
    cv::cvtColor(crop, gray, cv::COLOR_RGB2GRAY);
    std::vector<double> xs, ys;
    double xScore = 0, yScore = 0;
    if (lock) {
        xs = periodLockAxis(axisProjection(gray, 0), lineCount, xScore);
        ys = periodLockAxis(axisProjection(gray, 1), lineCount, yScore);
    } else {
        xs = fitAxis(axisProjection(gray, 0), lineCount, xScore);
        ys = fitAxis(axisProjection(gray, 1), lineCount, yScore);
    }
    int cw = x2 - x1 + 1, ch = y2 - y1 + 1;
    double rectAspect = std::min((double)cw / std::max(ch, 1), (double)ch / std::max(cw, 1));
    double gridAspect, regularity, extent;
    gridMetrics(xs, ys, cw, ch, gridAspect, regularity, extent);
    double confidence = std::min(1.0, std::max(0.0,
        0.50 * std::min(xScore, yScore) + 0.25 * gridAspect +
        0.15 * rectAspect + 0.10 * regularity));
    Geometry g;
    g.x_lines.resize(xs.size());
    for (size_t i = 0; i < xs.size(); ++i) g.x_lines[i] = xs[i] + x1;
    g.y_lines.resize(ys.size());
    for (size_t i = 0; i < ys.size(); ++i) g.y_lines[i] = ys[i] + y1;
    g.confidence = confidence;
    g.source = source;
    return g;
}

Geometry locateBoard(const cv::Mat& rgb, int lineCount) {
    if (lineCount != 15 && lineCount != 19)
        throw DetectionError("目前仅支持 15 路或 19 路棋盘");
    const bool trace = getenv("GB_VISION_TRACE") != nullptr;
    std::vector<cv::Rect> candidates = candidateRectangles(rgb, lineCount);
    if (trace) fprintf(stderr, "[trace] locateBoard candidates=%d (img %dx%d)\n",
                       (int)candidates.size(), rgb.cols, rgb.rows);
    if (candidates.empty())
        throw DetectionError("屏幕中没有找到近似方形的棋盘网格");

    bool have = false;
    Geometry best;
    double bestScore = -1.0;

    auto evaluate = [&](const Geometry& g, int x, int y, int w, int h) {
        std::vector<double> xl(g.x_lines.size()), yl(g.y_lines.size());
        for (size_t i = 0; i < g.x_lines.size(); ++i) xl[i] = g.x_lines[i] - x;
        for (size_t i = 0; i < g.y_lines.size(); ++i) yl[i] = g.y_lines[i] - y;
        double aspect, regularity, extent;
        gridMetrics(xl, yl, w, h, aspect, regularity, extent);
        if (aspect < 0.84 || regularity < 0.42) return;
        double score = 0.70 * g.confidence + 0.20 * regularity + 0.10 * extent;
        if (!have || score > bestScore) {
            have = true;
            bestScore = score;
            best = g;
            best.confidence = std::min(1.0, std::max(0.0, score));
        }
    };

    for (const cv::Rect& r : candidates) {
        try {
            Geometry g = geometryFromRect(rgb, r.x, r.y, r.x + r.width - 1,
                                          r.y + r.height - 1, lineCount, "auto", false);
            evaluate(g, r.x, r.y, r.width, r.height);
        } catch (const DetectionError&) {
            continue;
        }
    }

    if (!have) {
        std::vector<cv::Rect> ranked = candidates;
        std::stable_sort(ranked.begin(), ranked.end(), [](const cv::Rect& a, const cv::Rect& b) {
            return (long long)a.width * a.height > (long long)b.width * b.height;
        });
        for (const cv::Rect& r : ranked) {
            Geometry g;
            try {
                g = locateStretched(rgb, r, lineCount);
            } catch (...) { continue; }
            std::vector<double> xl(g.x_lines.size()), yl(g.y_lines.size());
            for (size_t i = 0; i < g.x_lines.size(); ++i) xl[i] = g.x_lines[i] - r.x;
            for (size_t i = 0; i < g.y_lines.size(); ++i) yl[i] = g.y_lines[i] - r.y;
            double aspect, regularity, extent;
            gridMetrics(xl, yl, r.width, r.height, aspect, regularity, extent);
            if (regularity < 0.55) continue;
            double score = 0.60 * g.confidence + 0.25 * regularity + 0.15 * extent;
            if (!have || score > bestScore) {
                have = true;
                bestScore = score;
                best = g;
                best.confidence = std::min(1.0, std::max(0.0, score));
            }
        }
    }
    if (!have || bestScore < 0.62) {
        if (trace) fprintf(stderr, "[trace] locateBoard FAIL have=%d bestScore=%.3f\n",
                           (int)have, have ? bestScore : -1.0);
        throw DetectionError("发现了候选区域，但网格间距不稳定");
    }
    if (trace) fprintf(stderr, "[trace] locateBoard OK score=%.3f x=%d y=%d\n",
                       bestScore, (int)best.x_lines.size(), (int)best.y_lines.size());
    return best;
}

void readStonesLegacy(const cv::Mat& rgb, const Geometry& geo, int bgLevel,
                      cv::Mat& board, cv::Mat& conf) {
    cv::Mat hsv, gray;
    cv::cvtColor(rgb, hsv, cv::COLOR_RGB2HSV);
    cv::cvtColor(rgb, gray, cv::COLOR_RGB2GRAY);
    int n = geo.size();
    board = cv::Mat(n, n, CV_8S, cv::Scalar(EMPTY));
    conf = cv::Mat(n, n, CV_32F, cv::Scalar(0.f));

    double brightThr = 215.0;
    if (bgLevel >= 0) brightThr = std::max(215.0, (double)bgLevel + 12.0);
    double spacing = geo.spacing();
    int radius = std::max(3, iRound(spacing * 0.30));
    int outerRadius = std::max(radius + 2, iRound(spacing * 0.47));
    int rr = outerRadius;
    int side = 2 * rr + 1;

    std::vector<uint8_t> innerMask(side * side, 0), ringMask(side * side, 0);
    for (int r = 0; r < side; ++r)
        for (int c = 0; c < side; ++c) {
            double xx = (double)(c - rr), yy = (double)(r - rr);
            double d = std::hypot(xx, yy);
            if (d <= radius) innerMask[r * side + c] = 1;
            if (d >= radius * 1.16 && d <= outerRadius) ringMask[r * side + c] = 1;
        }

    for (int row = 0; row < (int)geo.y_lines.size() && row < n; ++row) {
        for (int col = 0; col < (int)geo.x_lines.size() && col < n; ++col) {
            int x = iRound(geo.x_lines[col]), y = iRound(geo.y_lines[row]);
            int x1 = x - rr, y1 = y - rr;
            
            if (x1 >= gray.cols || y1 >= gray.rows || x1 + side <= 0 || y1 + side <= 0) continue;
            std::vector<double> inner, ring;
            double darkCnt = 0, whiteCnt = 0, redCnt = 0;
            int innerTot = 0;
            for (int r = 0; r < side; ++r) {
                for (int c = 0; c < side; ++c) {
                    int yy = y1 + r, xx = x1 + c;
                    if (yy < 0 || yy >= gray.rows || xx < 0 || xx >= gray.cols) continue;
                    double gv = (double)gray.at<uint8_t>(yy, xx);
                    const cv::Vec3b& hp = hsv.at<cv::Vec3b>(yy, xx);
                    int k = r * side + c;
                    if (innerMask[k]) {
                        innerTot++;
                        inner.push_back(gv);
                        if (gv < 92) darkCnt++;
                        if (gv > brightThr && hp[1] < 70) whiteCnt++;
                        bool red = ((hp[0] <= 8 || hp[0] >= 170) && hp[1] > 100 && hp[2] > 105);
                        if (red) redCnt++;
                    }
                    if (ringMask[k]) ring.push_back(gv);
                }
            }
            if (inner.empty() || ring.empty()) continue;
            
            double innerMean = median(inner), ringMean = mean(ring);
            double darkRatio = double(darkCnt) / double(innerTot);
            double whiteRatio = double(whiteCnt) / double(innerTot);
            double redRatio = double(redCnt) / double(innerTot);

            double blackStrength = std::max((ringMean - innerMean - 25) / 75,
                                            (darkRatio - 0.26) / 0.58);
            double whiteStrength = std::max((innerMean - ringMean - 18) / 62,
                                            (whiteRatio - 0.36) / 0.58);
            if (innerMean - ringMean >= 20 && whiteRatio >= 0.45 && darkRatio < 0.12)
                whiteStrength = std::max(whiteStrength,
                    0.58 + std::min(0.38, (innerMean - ringMean - 20) / 55));
            if (redRatio >= 0.075 && innerMean - ringMean >= 5 &&
                whiteRatio >= 0.22 && darkRatio < 0.12)
                whiteStrength = std::max(whiteStrength,
                    0.54 + std::min(0.40, (redRatio - 0.075) * 2.5));

            if (blackStrength >= 0.43 && blackStrength > whiteStrength + 0.08) {
                board.at<int8_t>(row, col) = BLACK;
                conf.at<float>(row, col) = (float)std::min(1.0, std::max(0.0, blackStrength));
            } else if (whiteStrength >= 0.43 && whiteStrength > blackStrength + 0.08) {
                board.at<int8_t>(row, col) = WHITE;
                conf.at<float>(row, col) = (float)std::min(1.0, std::max(0.0, whiteStrength));
            } else {
                conf.at<float>(row, col) =
                    (float)std::min(1.0, std::max(0.0, 1.0 - std::max(blackStrength, whiteStrength)));
            }
        }
    }
}

}  
