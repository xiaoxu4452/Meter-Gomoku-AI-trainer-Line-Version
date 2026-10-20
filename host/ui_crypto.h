

#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iterator>

static const char* GB_CALC_UI_KEY = "";  // placeholder: set at build time to the 32-char key used by the release packaging tool

static int gbCalcB64Idx(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c == '0' || (c >= '1' && c <= '9')) return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
static bool gbCalcBase64Decode(const std::string& in, std::vector<uint8_t>& out) {
  out.clear();
  int buf = 0, bits = 0;
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    int v = gbCalcB64Idx(c);
    if (v < 0) return false;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((uint8_t)((buf >> bits) & 0xff));
    }
  }
  return true;
}

static bool gbCalcDecryptBuf(std::vector<uint8_t>& buf, uint32_t seed2, int shift) {
  const char* K = GB_CALC_UI_KEY;
  uint32_t klen = (uint32_t)strlen(K);
  if (klen != 32) return false;                 

  int s = shift % 8;
  if (s != 0) {
    int amt = 8 - s;
    for (size_t i = 0; i < buf.size(); ++i)
      buf[i] = (uint8_t)(((buf[i] << amt) | (buf[i] >> (8 - amt))) & 0xff);
  }

  uint32_t s1 = 0x811c9dc5u;                       
  for (uint32_t i = 0; K[i]; ++i) {
    s1 ^= (uint8_t)K[i];
    s1 = (uint32_t)((uint64_t)s1 * 0x01000193u) & 0xffffffffu;
  }
  uint32_t s2 = seed2;
  for (size_t i = 0; i < buf.size(); ++i) {
    uint8_t mix;
    if ((i & 1) == 0) {
      s1 = (uint32_t)((uint64_t)s1 * 1103515245u + 12345u) & 0xffffffffu;
      mix = (uint8_t)(s1 >> 24);
    } else {
      s2 = (uint32_t)((uint64_t)s2 * 1103515245u + 12345u) & 0xffffffffu;
      mix = (uint8_t)(s2 >> 24);
    }
    buf[i] = (uint8_t)((buf[i] ^ (uint8_t)K[i % klen] ^ mix) & 0xff);
  }
  return true;
}

static bool gbCalcDecrypt(const std::string& b64, uint32_t seed2, int shift, std::vector<uint8_t>& out) {
  std::vector<uint8_t> buf;
  if (!gbCalcBase64Decode(b64, buf)) return false;
  if (!gbCalcDecryptBuf(buf, seed2, shift)) return false;
  out = std::move(buf);
  return true;
}

static bool gbCalcReadEncFile(const std::wstring& wpath, std::vector<uint8_t>& out) {
  std::ifstream f(wpath, std::ios::binary);
  if (!f) return false;
  std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  uint32_t seed2 = 0;
  int shift = 0;
  std::string b64;
  std::istringstream iss(all);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.compare(0, 6, "SEED2=") == 0) seed2 = (uint32_t)std::strtoul(line.c_str() + 6, nullptr, 10);
    else if (line.compare(0, 6, "SHIFT=") == 0) shift = (int)std::strtol(line.c_str() + 6, nullptr, 10);
    else if (line.compare(0, 4, "B64=") == 0) b64 = line.substr(4);
  }
  if (b64.empty()) return false;
  return gbCalcDecrypt(b64, seed2, shift, out);
}

static uint16_t gbCalcRdU16(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t gbCalcRdU32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t gbCalcFnv1a(const uint8_t* p, size_t n) {
  uint32_t h = 0x811c9dc5u;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h = (uint32_t)((uint64_t)h * 0x01000193u) & 0xffffffffu;
  }
  return h;
}

static bool gbCalcReadUiPak(const std::wstring& wpath, const std::string& name, std::vector<uint8_t>& out) {
  std::ifstream f(wpath, std::ios::binary);
  if (!f) return false;
  std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (all.size() < 8) return false;
  const uint8_t* b = (const uint8_t*)all.data();
  if (memcmp(b, "MGPK", 4) != 0) return false;
  const uint16_t ver = gbCalcRdU16(b + 4);
  const uint16_t cnt = gbCalcRdU16(b + 6);
  if (ver != 2 || cnt == 0 || cnt > 64) return false;
  const size_t need = 8 + (size_t)cnt * 40;
  if (all.size() < need) return false;
  for (uint16_t i = 0; i < cnt; ++i) {
    const uint8_t* e = b + 8 + (size_t)i * 40;
    char nm[17] = { 0 };
    memcpy(nm, e, 16);
    if (name != nm) continue;
    const uint32_t off = gbCalcRdU32(e + 16);
    const uint32_t len = gbCalcRdU32(e + 20);
    const uint32_t seed2 = gbCalcRdU32(e + 24);
    const uint32_t sum = gbCalcRdU32(e + 28);
    const int shift = (int)e[32];
    if (len == 0 || len > 8u * 1024 * 1024) return false;
    if ((uint64_t)off + len > (uint64_t)all.size()) return false;
    std::vector<uint8_t> buf(b + off, b + off + len);
    if (gbCalcFnv1a(buf.data(), buf.size()) != sum) return false;
    if (!gbCalcDecryptBuf(buf, seed2, shift)) return false;
    out = std::move(buf);
    return true;
  }
  return false;
}
