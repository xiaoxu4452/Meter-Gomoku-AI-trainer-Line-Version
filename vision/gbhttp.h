/* Vision - boardhttp. */


#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace gb {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    std::string header(const std::string& k) const;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
    std::vector<std::pair<std::string, std::string>> extraHeaders;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

bool startHttpServer(int port, HttpHandler handler, std::string& err);

}  
