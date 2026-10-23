// Meter Gomoku Link — v4.43
/* Vision - board. */


#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/geometry/2d.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace gb {

constexpr int EMPTY = 0;
constexpr int BLACK = 1;
constexpr int WHITE = -1;

struct Geometry;

inline double medianDiff(const std::vector<double>& v);

struct Geometry {
    std::vector<double> x_lines;
    std::vector<double> y_lines;
    double confidence = 0.0;
    std::string source = "auto";
    int screen_left = 0;
    int screen_top = 0;

    int size() const { return (int)x_lines.size(); }

    double spacing() const {
        return (medianDiff(x_lines) + medianDiff(y_lines)) / 2.0;
    }
    
    void bounds(int& x0, int& y0, int& x1, int& y1) const {
        x0 = (int)std::nearbyint(x_lines.front());
        y0 = (int)std::nearbyint(y_lines.front());
        x1 = (int)std::nearbyint(x_lines.back());
        y1 = (int)std::nearbyint(y_lines.back());
    }
};

inline double medianSorted(std::vector<double>& v) {
    if (v.empty()) return 0.0;
    size_t n = v.size();
    
    if (n % 2 == 1) {
        std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)(n / 2), v.end());
        return v[n / 2];
    }
    std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)(n / 2), v.end());
    double hi = v[n / 2];
    double lo = *std::max_element(v.begin(), v.begin() + (std::ptrdiff_t)(n / 2));
    return (lo + hi) / 2.0;
}

inline double median(std::vector<double> v) { return medianSorted(v); }

inline double median(const std::vector<int16_t>& src) {
    
    std::vector<int16_t> v = src;
    size_t n = v.size();
    if (n == 0) return 0.0;
    if (n % 2 == 1) {
        std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)(n / 2), v.end());
        return v[n / 2];
    }
    std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)(n / 2), v.end());
    double hi = v[n / 2];
    double lo = *std::max_element(v.begin(), v.begin() + (std::ptrdiff_t)(n / 2));
    return (lo + hi) / 2.0;
}

inline double median(const std::vector<uint8_t>& src) {
    
    std::vector<uint8_t> v = src;
    size_t n = v.size();
    if (n == 0) return 0.0;
    if (n % 2 == 1) {
        std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)(n / 2), v.end());
        return v[n / 2];
    }
    std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)(n / 2), v.end());
    double hi = v[n / 2];
    double lo = *std::max_element(v.begin(), v.begin() + (std::ptrdiff_t)(n / 2));
    return (lo + hi) / 2.0;
}

inline double median(const std::vector<float>& src) {
    std::vector<double> v(src.begin(), src.end());
    return medianSorted(v);
}

inline double percentile(std::vector<double> v, double q) {
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    if (n == 1) return v[0];
    double idx = (q / 100.0) * double(n - 1);
    double lo = std::floor(idx), hi = std::ceil(idx);
    size_t ilo = (size_t)std::max(0.0, std::min(lo, double(n - 1)));
    size_t ihi = (size_t)std::max(0.0, std::min(hi, double(n - 1)));
    if (ilo == ihi) return v[ilo];
    double frac = idx - lo;
    return v[ilo] + (v[ihi] - v[ilo]) * frac;
}

inline double percentileF(std::vector<float>& v, double q) {
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    size_t n = v.size();
    if (n == 1) return v[0];
    double idx = (q / 100.0) * double(n - 1);
    double lo = std::floor(idx), hi = std::ceil(idx);
    size_t ilo = (size_t)std::max(0.0, std::min(lo, double(n - 1)));
    size_t ihi = (size_t)std::max(0.0, std::min(hi, double(n - 1)));
    std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)ilo, v.end());
    if (ilo == ihi) return v[ilo];
    std::nth_element(v.begin() + (std::ptrdiff_t)ilo + 1,
                     v.begin() + (std::ptrdiff_t)ihi, v.end());
    double frac = idx - lo;
    return (double)v[ilo] + ((double)v[ihi] - (double)v[ilo]) * frac;
}
inline double medianAbsDev(std::vector<double> v) {
    if (v.empty()) return 0.0;
    double med = medianSorted(v);
    std::vector<double> dev(v.size());
    for (size_t i = 0; i < v.size(); ++i) dev[i] = std::abs(v[i] - med);
    return 1.4826 * medianSorted(dev);
}

inline double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double s = 0.0;
    for (double x : v) s += x;
    return s / double(v.size());
}

inline double stddev(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double m = mean(v), s = 0.0;
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt(s / double(v.size()));
}

inline std::vector<double> diff(const std::vector<double>& v) {
    std::vector<double> out;
    if (v.size() < 2) return out;
    out.resize(v.size() - 1);
    for (size_t i = 1; i < v.size(); ++i) out[i - 1] = v[i] - v[i - 1];
    return out;
}

inline double medianDiff(const std::vector<double>& v) { return median(diff(v)); }

inline std::vector<double> linspace(double start, double stop, int num) {
    std::vector<double> out;
    if (num <= 0) return out;
    out.resize(num);
    if (num == 1) { out[0] = start; return out; }
    double step = (stop - start) / double(num - 1);
    for (int i = 0; i < num; ++i) out[i] = start + step * i;
    out[num - 1] = stop;   
    return out;
}

inline std::vector<double> ramp(double o, double d, int count) {
    std::vector<double> out(count);
    for (int i = 0; i < count; ++i) out[i] = o + d * i;
    return out;
}

inline void polyfit1(const std::vector<double>& x, const std::vector<double>& y,
                     double& slope, double& intercept) {
    size_t n = std::min(x.size(), y.size());
    if (n == 0) { slope = 0.0; intercept = 0.0; return; }
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i) {
        sx += x[i]; sy += y[i]; sxx += x[i] * x[i]; sxy += x[i] * y[i];
    }
    double dn = double(n);
    double den = dn * sxx - sx * sx;
    if (std::abs(den) < 1e-12) { slope = 0.0; intercept = sy / dn; return; }
    slope = (dn * sxy - sx * sy) / den;
    intercept = (sy - slope * sx) / dn;
}

inline std::vector<int> argsort(const std::vector<double>& v) {
    std::vector<int> idx(v.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::stable_sort(idx.begin(), idx.end(),
                     [&v](int a, int b) { return v[a] < v[b]; });
    return idx;
}

inline int globalMode(const cv::Mat& gray) {
    double hist[256] = {0};
    int total = gray.rows * gray.cols;
    const uint8_t* p = gray.ptr<uint8_t>(0);
    for (int i = 0; i < total; ++i) hist[p[i]] += 1.0;
    
    double sm[256];
    for (int i = 0; i < 256; ++i) {
        double s = 0.0;
        for (int j = 0; j < 5; ++j) {
            int k = i + j - 2;
            if (k >= 0 && k < 256) s += hist[k];
        }
        sm[i] = s / 5.0;
    }
    int best = 0;
    for (int i = 1; i < 256; ++i)
        if (sm[i] > sm[best]) best = i;   
    return best;
}

inline std::vector<double> bincount256(const cv::Mat& gray) {
    std::vector<double> hist(256, 0.0);
    int total = gray.rows * gray.cols;
    const uint8_t* p = gray.ptr<uint8_t>(0);
    for (int i = 0; i < total; ++i) hist[p[i]] += 1.0;
    return hist;
}

inline cv::Mat boardToService(const cv::Mat& board) {
    cv::Mat out = board.clone();
    int total = out.rows * out.cols;
    int8_t* p = out.ptr<int8_t>(0);
    for (int i = 0; i < total; ++i)
        if (p[i] == WHITE) p[i] = 2;
    return out;
}

inline std::string base64Encode(const std::vector<uint8_t>& in) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out.push_back(T[(v >> 18) & 63]); out.push_back(T[(v >> 12) & 63]);
        out.push_back(T[(v >> 6) & 63]);  out.push_back(T[v & 63]);
    }
    if (i < in.size()) {
        uint32_t v = in[i] << 16;
        bool two = (i + 1 < in.size());
        if (two) v |= in[i + 1] << 8;
        out.push_back(T[(v >> 18) & 63]); out.push_back(T[(v >> 12) & 63]);
        out.push_back(two ? T[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

inline bool base64Decode(const std::string& in, std::vector<uint8_t>& out) {
    static int8_t LUT[256];
    static bool init = false;
    if (!init) {
        std::memset(LUT, -1, sizeof(LUT));
        const char* T =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) LUT[(uint8_t)T[i]] = (int8_t)i;
        init = true;
    }
    out.clear();
    out.reserve(in.size() / 4 * 3 + 3);
    uint32_t acc = 0;
    int nbits = 0;
    for (char c : in) {
        int8_t d = LUT[(uint8_t)c];
        if (d < 0) continue;                  
        acc = (acc << 6) | (uint32_t)d;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out.push_back((uint8_t)((acc >> nbits) & 0xFF));
        }
    }
    return true;
}

class Json {
public:
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };

    Type type = NUL;
    bool b = false;
    double num = 0.0;
    bool isInt = false;
    long long inum = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    Json() {}
    static Json makeNull() { return Json(); }
    static Json makeBool(bool v) { Json j; j.type = BOOL; j.b = v; return j; }
    static Json makeInt(long long v) {
        Json j; j.type = NUM; j.isInt = true; j.inum = v; j.num = (double)v; return j;
    }
    static Json makeNum(double v) { Json j; j.type = NUM; j.num = v; return j; }
    static Json makeStr(const std::string& v) { Json j; j.type = STR; j.str = v; return j; }
    static Json makeArr() { Json j; j.type = ARR; return j; }
    static Json makeObj() { Json j; j.type = OBJ; return j; }

    void set(const std::string& k, Json v) { obj.emplace_back(k, std::move(v)); }
    void push(Json v) { arr.push_back(std::move(v)); }

    const Json* find(const std::string& k) const {
        for (const auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    Json* findMutable(const std::string& k) {
        for (auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    double numOr(const std::string& k, double d) const {
        const Json* p = find(k);
        return (p && p->type == NUM) ? p->num : d;
    }
    bool boolOr(const std::string& k, bool d) const {
        const Json* p = find(k);
        if (!p) return d;
        if (p->type == BOOL) return p->b;
        if (p->type == NUM) return p->num != 0.0;
        return d;
    }
    std::string strOr(const std::string& k, const std::string& d) const {
        const Json* p = find(k);
        return (p && p->type == STR) ? p->str : d;
    }

    std::string dump() const;
};

bool jsonParse(const std::string& text, Json& out);

std::string fmtDouble(double v);

void setDpiAware();

void TouchTraffic();

}  
