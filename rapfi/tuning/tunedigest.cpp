/*
 *  Rapfi, a Gomoku/Renju playing engine supporting piskvork protocol.
 *  Copyright (C) 2022  Rapfi developers
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "tunedigest.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace Tuning {
namespace {

    constexpr std::array<uint32_t, 64> RoundConstants = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U,
    };

    uint32_t rotateRight(uint32_t value, unsigned shift)
    {
        return (value >> shift) | (value << (32 - shift));
    }

}  // namespace

void Sha256::transform(const uint8_t *block)
{
    uint32_t words[64];
    for (size_t i = 0; i < 16; i++)
        words[i] = uint32_t(block[i * 4]) << 24 | uint32_t(block[i * 4 + 1]) << 16
                   | uint32_t(block[i * 4 + 2]) << 8 | uint32_t(block[i * 4 + 3]);
    for (size_t i = 16; i < 64; i++) {
        uint32_t s0 =
            rotateRight(words[i - 15], 7) ^ rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
        uint32_t s1 =
            rotateRight(words[i - 2], 17) ^ rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (size_t i = 0; i < 64; i++) {
        uint32_t sum1     = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
        uint32_t choice   = (e & f) ^ (~e & g);
        uint32_t temp1    = h + sum1 + choice + RoundConstants[i] + words[i];
        uint32_t sum0     = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2    = sum0 + majority;
        h                 = g;
        g                 = f;
        f                 = e;
        e                 = d + temp1;
        d                 = c;
        c                 = b;
        b                 = a;
        a                 = temp1 + temp2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const void *data, size_t size)
{
    if (finished_)
        throw std::logic_error("SHA-256 update after finish");
    if (size > std::numeric_limits<uint64_t>::max() - totalBytes_)
        throw std::length_error("SHA-256 input exceeds 64-bit length");
    totalBytes_ += size;

    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    while (size != 0) {
        size_t copied = std::min(size, buffer_.size() - buffered_);
        std::copy_n(bytes, copied, buffer_.data() + buffered_);
        bytes += copied;
        size -= copied;
        buffered_ += copied;
        if (buffered_ == buffer_.size()) {
            transform(buffer_.data());
            buffered_ = 0;
        }
    }
}

Sha256Digest Sha256::finish()
{
    if (finished_)
        throw std::logic_error("SHA-256 finish called more than once");
    finished_          = true;
    uint64_t bitLength = totalBytes_ * 8;

    buffer_[buffered_++] = 0x80;
    if (buffered_ > 56) {
        std::fill(buffer_.begin() + buffered_, buffer_.end(), uint8_t(0));
        transform(buffer_.data());
        buffered_ = 0;
    }
    std::fill(buffer_.begin() + buffered_, buffer_.begin() + 56, uint8_t(0));
    for (size_t i = 0; i < 8; i++)
        buffer_[56 + i] = static_cast<uint8_t>(bitLength >> (56 - 8 * i));
    transform(buffer_.data());

    Sha256Digest digest;
    for (size_t i = 0; i < state_.size(); i++) {
        digest[i * 4]     = static_cast<uint8_t>(state_[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
    }
    return digest;
}

Sha256Digest sha256File(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("unable to open file for SHA-256: " + path.string());

    Sha256                  hasher;
    std::array<char, 65536> buffer;
    while (input) {
        input.read(buffer.data(), buffer.size());
        std::streamsize count = input.gcount();
        if (count > 0)
            hasher.update(buffer.data(), static_cast<size_t>(count));
    }
    if (!input.eof())
        throw std::runtime_error("failed to read file for SHA-256: " + path.string());
    return hasher.finish();
}

std::string sha256Hex(const Sha256Digest &digest)
{
    constexpr char Hex[] = "0123456789abcdef";
    std::string    result(digest.size() * 2, '0');
    for (size_t i = 0; i < digest.size(); i++) {
        result[i * 2]     = Hex[digest[i] >> 4];
        result[i * 2 + 1] = Hex[digest[i] & 0x0f];
    }
    return result;
}

}  // namespace Tuning
