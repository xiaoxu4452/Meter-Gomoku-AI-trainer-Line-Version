// Meter Gomoku Link — v4.43
/* Vision - boardscanapi. */


#pragma once

#include <string>

#include "gbvision.h"

namespace gb {

Json ScanResultJson(const ScanResult& r);

std::string ScanDirect(const std::string& body);

}  
