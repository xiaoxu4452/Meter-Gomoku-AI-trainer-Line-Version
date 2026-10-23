// Meter Gomoku Link — v4.43
/* Vision - boardhttp. */


#include "gbhttp.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstring>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace gb {

std::string HttpRequest::header(const std::string& k) const {
    for (auto& kv : headers) {
        if (kv.first.size() != k.size()) continue;
        bool same = true;
        for (size_t i = 0; i < k.size(); ++i)
            if (std::tolower((unsigned char)kv.first[i]) != std::tolower((unsigned char)k[i])) {
                same = false;
                break;
            }
        if (same) return kv.second;
    }
    return {};
}

namespace {

std::atomic<bool> wsaReady{false};

bool ensureWinsock() {
    if (wsaReady.load()) return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    wsaReady.store(true);
    return true;
}

bool recvUntil(SOCKET s, std::string& buf, const std::string& delim) {
    char tmp[4096];
    while (buf.find(delim) == std::string::npos) {
        int n = ::recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, (size_t)n);
        if (buf.size() > 64u * 1024u * 1024u) return false;   
    }
    return true;
}

bool sendAll(SOCKET s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = ::send(s, data.data() + sent, (int)std::min<size_t>(data.size() - sent, 1 << 20), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

const char* statusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

void handleConn(SOCKET s, HttpHandler handler) {
    std::string buf;
    int served = 0;
    while (served < 64) {
        
        if (!recvUntil(s, buf, "\r\n\r\n")) break;
        size_t headEnd = buf.find("\r\n\r\n");
        std::string head = buf.substr(0, headEnd);
        buf.erase(0, headEnd + 4);

        HttpRequest req;
        size_t lineEnd = head.find("\r\n");
        std::string line = head.substr(0, lineEnd == std::string::npos ? head.size() : lineEnd);
        size_t sp1 = line.find(' ');
        size_t sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
        if (sp1 == std::string::npos) break;
        req.method = line.substr(0, sp1);
        req.path = (sp2 == std::string::npos) ? line.substr(sp1 + 1)
                                              : line.substr(sp1 + 1, sp2 - sp1 - 1);

        size_t p = (lineEnd == std::string::npos) ? head.size() : lineEnd + 2;
        while (p < head.size()) {
            size_t e = head.find("\r\n", p);
            if (e == std::string::npos) e = head.size();
            std::string hl = head.substr(p, e - p);
            size_t colon = hl.find(':');
            if (colon != std::string::npos) {
                std::string k = hl.substr(0, colon);
                std::string v = hl.substr(colon + 1);
                while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
                while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
                req.headers.emplace_back(k, v);
            }
            p = e + 2;
        }

        long long clen = 0;
        std::string cl = req.header("Content-Length");
        if (!cl.empty()) clen = std::stoll(cl);
        if (clen > 0) {
            while ((long long)buf.size() < clen) {
                char tmp[65536];
                int n = ::recv(s, tmp, sizeof(tmp), 0);
                if (n <= 0) { clen = (long long)buf.size(); break; }
                buf.append(tmp, (size_t)n);
            }
            if ((long long)buf.size() < clen) break;
            req.body = buf.substr(0, (size_t)clen);
            buf.erase(0, (size_t)clen);
        }

        bool wantClose = false;
        {
            std::string cn = req.header("Connection");
            for (auto& c : cn) c = (char)std::tolower((unsigned char)c);
            if (cn.find("close") != std::string::npos) wantClose = true;
        }

        HttpResponse res;
        try {
            res = handler(req);
        } catch (const std::exception& e) {
            res.status = 500;
            res.body = std::string("{\"ok\": false, \"err\": \"") + e.what() + "\"}";
        }

        std::string out;
        out.reserve(res.body.size() + 512);
        out += "HTTP/1.1 ";
        out += std::to_string(res.status);
        out += " ";
        out += statusText(res.status);
        out += "\r\nContent-Type: ";
        out += res.contentType;
        out += "\r\nContent-Length: ";
        out += std::to_string(res.body.size());
        out += "\r\nConnection: ";
        out += wantClose ? "close" : "keep-alive";
        out += "\r\n";
        for (auto& kv : res.extraHeaders) {
            out += kv.first;
            out += ": ";
            out += kv.second;
            out += "\r\n";
        }
        out += "\r\n";
        out += res.body;
        if (!sendAll(s, out)) break;
        served++;
        if (wantClose) break;
    }
    ::shutdown(s, SD_BOTH);
    ::closesocket(s);
}

}  

bool startHttpServer(int port, HttpHandler handler, std::string& err) {
    if (!ensureWinsock()) {
        err = "WSAStartup failed";
        return false;
    }
    SOCKET srv = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        err = "socket() failed";
        return false;
    }
    BOOL yes = TRUE;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        err = "bind failed (port " + std::to_string(port) + ")";
        ::closesocket(srv);
        return false;
    }
    if (::listen(srv, 32) == SOCKET_ERROR) {
        err = "listen failed";
        ::closesocket(srv);
        return false;
    }

    std::thread([srv, handler] {
        while (true) {
            SOCKET c = ::accept(srv, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            
            BOOL nodelay = TRUE;
            ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
            std::thread(handleConn, c, handler).detach();
        }
    }).detach();
    return true;
}

}  
