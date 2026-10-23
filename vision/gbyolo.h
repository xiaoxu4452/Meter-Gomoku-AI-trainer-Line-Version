// Meter Gomoku Link — v4.43
/* Vision - boardyolo. */


#pragma once

#include "gb.h"
#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace gbyolo {

struct YoloRead {
    bool attempted = false;   
    bool adopted = false;     
    cv::Mat board;            
    int nStones = 0;          
    int nRaw = 0;             
    int nStripped = 0;        
    std::string note;         
};

std::string modelPath();

bool modelAvailable();

YoloRead readStonesYolo(const cv::Mat& rgb, const std::vector<double>& xs,
                        const std::vector<double>& ys, double spacing);

struct Det {
    float cx = 0, cy = 0, w = 0, h = 0;   
    float conf = 0;                        
    int cls = 0;                           
};

std::vector<Det> decodeYolo(const float* data, int rows, int cols,
                            float confThresh, float iouThresh,
                            float scale, float padX, float padY,
                            int netSize, int imgW, int imgH);

std::vector<Det> nms(std::vector<Det> dets, float iouThresh);

}  
