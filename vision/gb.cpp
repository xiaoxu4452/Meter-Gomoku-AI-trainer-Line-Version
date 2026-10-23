/* Vision - board. */


#include "gb.h"

#include <windows.h>

#include <charconv>

namespace gb {

std::string fmtDouble(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
    if (v == 0.0) return std::signbit(v) ? "-0.0" : "0.0";
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), v);   
    std::string s(buf, res.ptr);
    
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos && s.find("inf") == std::string::npos &&
        s.find("nan") == std::string::npos) {
        s += ".0";
    }
    return s;
}

static void dumpString(const std::string& s, std::string& out) {
    out.push_back('"');
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') { out += "\\\""; ++i; }
        else if (c == '\\') { out += "\\\\"; ++i; }
        else if (c == '\n') { out += "\\n"; ++i; }
        else if (c == '\r') { out += "\\r"; ++i; }
        else if (c == '\t') { out += "\\t"; ++i; }
        else if (c == '\b') { out += "\\b"; ++i; }
        else if (c == '\f') { out += "\\f"; ++i; }
        else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", c);
            out += b; ++i;
        } else if (c < 0x80) { out.push_back((char)c); ++i; }
        else {
            
            unsigned cp = 0;
            int extra = 0;
            if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
            else { cp = c; extra = 0; }
            size_t j = i + 1;
            for (int k = 0; k < extra && j < s.size(); ++k, ++j)
                cp = (cp << 6) | ((unsigned char)s[j] & 0x3F);
            i = j;
            char b[16];
            if (cp >= 0x10000) {
                unsigned v = cp - 0x10000;
                snprintf(b, sizeof(b), "\\u%04x\\u%04x",
                         0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
            } else {
                snprintf(b, sizeof(b), "\\u%04x", cp);
            }
            out += b;
        }
    }
    out.push_back('"');
}

static void dumpValue(const Json& j, std::string& out) {
    switch (j.type) {
        case Json::NUL: out += "null"; break;
        case Json::BOOL: out += j.b ? "true" : "false"; break;
        case Json::NUM:
            if (j.isInt) out += std::to_string(j.inum);
            else out += fmtDouble(j.num);
            break;
        case Json::STR: dumpString(j.str, out); break;
        case Json::ARR: {
            out.push_back('[');
            for (size_t i = 0; i < j.arr.size(); ++i) {
                if (i) out += ", ";
                dumpValue(j.arr[i], out);
            }
            out.push_back(']');
            break;
        }
        case Json::OBJ: {
            out.push_back('{');
            for (size_t i = 0; i < j.obj.size(); ++i) {
                if (i) out += ", ";
                dumpString(j.obj[i].first, out);
                out += ": ";
                dumpValue(j.obj[i].second, out);
            }
            out.push_back('}');
            break;
        }
    }
}

std::string Json::dump() const {
    std::string out;
    out.reserve(512);
    dumpValue(*this, out);
    return out;
}

namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    explicit Parser(const std::string& t) : s(t) {}

    void skip() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool eof() { skip(); return i >= s.size(); }

    bool parseValue(Json& out) {
        skip();
        if (i >= s.size()) return false;
        char c = s[i];
        if (c == '{') return parseObj(out);
        if (c == '[') return parseArr(out);
        if (c == '"') {
            out.type = Json::STR;
            return parseString(out.str);
        }
        if (c == 't') { if (s.compare(i, 4, "true") == 0) { i += 4; out = Json::makeBool(true); return true; } return false; }
        if (c == 'f') { if (s.compare(i, 5, "false") == 0) { i += 5; out = Json::makeBool(false); return true; } return false; }
        if (c == 'n') { if (s.compare(i, 4, "null") == 0) { i += 4; out = Json::makeNull(); return true; } return false; }
        return parseNumber(out);
    }

    bool parseNumber(Json& out) {
        skip();
        size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        bool isFloat = false;
        while (i < s.size()) {
            char c = s[i];
            if (c >= '0' && c <= '9') { ++i; }
            else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                if (c == '.' || c == 'e' || c == 'E') isFloat = true;
                ++i;
            } else break;
        }
        if (i == start) return false;
        std::string tok = s.substr(start, i - start);
        try {
            if (!isFloat) {
                out = Json::makeInt(std::stoll(tok));
            } else {
                out = Json::makeNum(std::stod(tok));
            }
        } catch (...) { return false; }
        return true;
    }

    bool parseString(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = s[i + k];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else return false;
                    }
                    i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() &&
                        s[i] == '\\' && s[i + 1] == 'u') {
                        unsigned lo = 0;
                        bool ok = true;
                        for (int k = 0; k < 4; ++k) {
                            char h = s[i + 2 + k];
                            lo <<= 4;
                            if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                            else { ok = false; break; }
                        }
                        if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                            i += 6;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                    }
                    
                    if (cp < 0x80) out.push_back((char)cp);
                    else if (cp < 0x800) {
                        out.push_back((char)(0xC0 | (cp >> 6)));
                        out.push_back((char)(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        out.push_back((char)(0xE0 | (cp >> 12)));
                        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back((char)(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back((char)(0xF0 | (cp >> 18)));
                        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back((char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool parseArr(Json& out) {
        ++i;   
        out = Json::makeArr();
        skip();
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        while (true) {
            Json v;
            if (!parseValue(v)) return false;
            out.arr.push_back(std::move(v));
            skip();
            if (i >= s.size()) return false;
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == ']') { ++i; return true; }
            return false;
        }
    }

    bool parseObj(Json& out) {
        ++i;   
        out = Json::makeObj();
        skip();
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            skip();
            std::string key;
            if (!parseString(key)) return false;
            skip();
            if (i >= s.size() || s[i] != ':') return false;
            ++i;
            Json v;
            if (!parseValue(v)) return false;
            out.obj.emplace_back(std::move(key), std::move(v));
            skip();
            if (i >= s.size()) return false;
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == '}') { ++i; return true; }
            return false;
        }
    }
};

}  

bool jsonParse(const std::string& text, Json& out) {
    Parser p(text);
    if (!p.parseValue(out)) return false;
    return true;
}

void setDpiAware() {
    static bool done = false;
    if (done) return;
    done = true;
    typedef BOOL(WINAPI * SetCtxFn)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto fn = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn && fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
        typedef BOOL(WINAPI * SetAwareFn)();
        auto fn2 = (SetAwareFn)GetProcAddress(user32, "SetProcessDPIAware");
        if (fn2 && fn2()) return;
    }
    typedef HRESULT(WINAPI * SetShcoreFn)(int);
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        auto fn = (SetShcoreFn)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (fn) fn(2);   
    }
}

}  
