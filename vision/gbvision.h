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

struct SyncRecogParams {
    int grayOffset = 50;
    int blackOffset = 96;
    int whiteOffset = 96;
    int blackPercent = 33;
    int whitePercent = 33;
};

struct SyncCell {
    int blackPercent = 0, whitePercent = 0;
    int pureWhitePercent = 0, almostWhitePercent = 0;
    bool trueWhite = false;
};
SyncCell syncCellInfo(const cv::Mat& rgb, int sx, int sy, int w, int h,
                      const SyncRecogParams& p);

bool locateBoardSync(const cv::Mat& rgb, int size,
                     std::vector<double>& xLines, std::vector<double>& yLines,
                     double& spacing, Json& diag);

void readStonesSync(const cv::Mat& crop, const GeoLike& g, const SyncRecogParams& p,
                    cv::Mat& board, Json& diag, bool allowRelax = false);

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

bool captureScreenRect(cv::Mat& rgb, int& ox, int& oy, int x, int y, int w, int h);

struct ScanHint { bool has = false; double x = 0, y = 0, w = 0, h = 0; };
struct ExcludeRect { int x = 0, y = 0, w = 0, h = 0; };

struct LockedGeo {
    bool has = false;
    std::vector<double> xLines, yLines;    
    double spacing = 0;
};

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
