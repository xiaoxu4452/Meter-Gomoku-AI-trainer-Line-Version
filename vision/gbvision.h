// Meter Gomoku Link — v4.43
/* Vision - boardvision. */


#pragma once

#include "gb.h"

namespace gb {

struct GeoLike {
    int size = 15;
    double spacing = 0.0;
    std::vector<double> x_lines;
    std::vector<double> y_lines;
};

// 同步算法识别参数（照抄 synchronization/lizzieyzy-next-readboard 的
// RecognitionThresholds 默认值；由宿主连线窗口「识别参数」浮层配置）。
struct SyncRecogParams {
    int grayOffset = 50;
    int blackOffset = 96;
    int whiteOffset = 96;
    int blackPercent = 33;
    int whitePercent = 33;
};

// 单格统计（百分比 + trueWhite 证据）
struct SyncCell {
    int blackPercent = 0, whitePercent = 0;
    int pureWhitePercent = 0, almostWhitePercent = 0;
    bool trueWhite = false;
};
SyncCell syncCellInfo(const cv::Mat& rgb, int sx, int sy, int w, int h,
                      const SyncRecogParams& p);

// 同步算法棋盘定位：返回交点线(xLines/yLines)与平均格距；失败返回 false（上层回退自适应找盘）
bool locateBoardSync(const cv::Mat& rgb, int size,
                     std::vector<double>& xLines, std::vector<double>& yLines,
                     double& spacing, Json& diag);

// 同步算法逐格读子 → int8_t 板矩阵（EMPTY=0 / BLACK=1 / WHITE=-1，同 gb.h 常量）
// ★ 2026-10-19：allowRelax —— 整盘一颗子都没判出来时，用「本帧实测峰值 ×0.6」当临时阈值
//   再判一次（小棋子截图专用：棋子直径 ≈0.3 格距时占格比例只有 ~28% < 默认 33%）。
//   只在识图单帧（forceSync）路径开启；连线稳态=false，行为与原来完全一致。
void readStonesSync(const cv::Mat& crop, const GeoLike& g, const SyncRecogParams& p,
                    cv::Mat& board, Json& diag, bool allowRelax = false);

// ★★ 2026-10-19：**局部对比读子**（第三套判别器，专治「换主题就瞎」）。
//   同步算法用的是**绝对**阈值（黑子 ≤96、白子 ≥255-whiteOffset），天然只能适配木色棋盘：
//   浅色棋盘上整格背景就 ≥159 ⇒ 空格全判白；深色棋盘上黑子只比底色暗一点 ⇒ 黑子全丢。
//   这里改成「以每个交点自己的局部底色为基准」：中心 4 个小块 vs 对角 4 个小块的亮度差，
//   用全盘 |差| 的 p90 自适应定阈 —— 木盘/浅盘/深盘/冷色盘一视同仁。
//   只在识图单帧（forceSync）里、且同步结果**看起来不可信**时才用它兜底。
void readStonesLocal(const cv::Mat& crop, const GeoLike& g, cv::Mat& board, Json& diag);

struct BgEstimate {
    cv::Mat field;      
    int bg0 = 200;      
    double sigma = 0.0; 
};

double noiseSigma(const cv::Mat& diff16);

BgEstimate estimateBackground(const cv::Mat& gray, double spacing);

bool plateauLikeMed(double outMed, double ref);
bool quadsPlateauVals(const std::vector<double>& quadMeds, double ref);
bool plateauGateVeto(bool outPlateau, double ref, double edge);

void readStonesAdaptive(const cv::Mat& rgb, const GeoLike& geo,
                        cv::Mat& board, cv::Mat& conf, Json& diag);

int stripBorderArtifacts(cv::Mat& board, cv::Mat* conf, double minRatio = 0.80);
Json invariants(const cv::Mat& board);

cv::Mat autoCanny(const cv::Mat& gray, double sigma = 0.33);

bool locateGridHough(const cv::Mat& gray, int lineCount,
                     std::vector<double>& xs, std::vector<double>& ys, double& residual);

struct DetectionError {
    std::string msg;
    explicit DetectionError(const std::string& m) : msg(m) {}
};

Geometry locateBoard(const cv::Mat& rgb, int lineCount);
Geometry geometryFromRect(const cv::Mat& rgb, int x1, int y1, int x2, int y2,
                          int lineCount, const std::string& source, bool lock);
double gridLiveness(const cv::Mat& rgb, const Geometry& geo);
void readStonesLegacy(const cv::Mat& rgb, const Geometry& geo, int bgLevel,
                      cv::Mat& board, cv::Mat& conf);

struct ScanResult {
    bool ok = true;
    bool found = false;
    double ox = 0, oy = 0;             
    Json geometry;                     
    Json boardRect;                    
    std::vector<std::pair<int, int>> black, white;   
    bool suspect = false;
    int multiBoard = 0;
    Json diag;
    cv::Mat crop;                      
};

bool captureScreen(cv::Mat& rgb, int& ox, int& oy);

// 只抓指定矩形（CPU/GDI）：连线稳态每拍只看已框定区域，免去整屏抓取+全屏转换的固定开销。
// 越界或抓取失败返回 false，调用方回退整屏 captureScreen。
bool captureScreenRect(cv::Mat& rgb, int& ox, int& oy, int x, int y, int w, int h);

struct ScanHint { bool has = false; double x = 0, y = 0, w = 0, h = 0; };
struct ExcludeRect { int x = 0, y = 0, w = 0, h = 0; };

struct LockedGeo {
    bool has = false;
    std::vector<double> xLines, yLines;    
    double spacing = 0;
};

// ★ 2026-10-19：forceSync —— 单帧也要用同步算法读子。
//   连线窗是「连续帧」：第一帧自家定位+自家读子，第二帧起（几何复用）才切 sync（见 gbscreen.cpp
//   finishWithGeometry 的两级融合说明）。而识图窗每次都是一张独立图片（单帧），若沿用旧口径就永远
//   进不了 sync 分支 ⇒ 拖动那 5 项阈值毫无效果。forceSync=true 时无视 liveness 闸门直接走 sync
//   （sync 把整盘判成同色即 swamped 时，里面仍有退回自制读子的兜底）。
ScanResult scanOnce(const cv::Mat* rgbIn, int size, const ScanHint& hint,
                    const std::vector<ExcludeRect>& exclude, bool probe,
                    const std::vector<int>* region,
                    const LockedGeo* locked = nullptr,
                    const SyncRecogParams* sync = nullptr,
                    bool forceSync = false);

struct BoardRectCand {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    double step = 0;
    double score = 0;
};
std::vector<BoardRectCand> snapBoardRects(const cv::Mat& gray, int lineCount, int topn);

Json recognize(const std::string& imageB64, int size, bool allowSnap = true);

Json recognizeWulin(const Json& geo, const Json& win, int size);

}  
