// Meter Gomoku Link — v4.43
/* Vision - boardloader. */


#pragma once

#include <string>

namespace gb {

bool LoadModelDll(bool quietLog = false);

bool ModelDllReady();

bool ModelScan(const std::string& reqBody, std::string& resp);

bool ModelScanImage(const unsigned char* img, int len, bool probe, std::string& resp);

bool ModelRecognizeJson(const std::string& reqBody, std::string& resp);

}  
