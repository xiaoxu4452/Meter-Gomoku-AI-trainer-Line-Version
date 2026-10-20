/*
 *  Rapfi, a Gomoku/Renju playing engine supporting piskvork protocol.
 *  Copyright (C) 2022  Rapfi developers
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Tuning {

using Sha256Digest = std::array<uint8_t, 32>;

class Sha256
{
public:
    void         update(const void *data, size_t size);
    Sha256Digest finish();

private:
    void transform(const uint8_t *block);

    std::array<uint32_t, 8> state_ = {0x6a09e667U,
                                      0xbb67ae85U,
                                      0x3c6ef372U,
                                      0xa54ff53aU,
                                      0x510e527fU,
                                      0x9b05688cU,
                                      0x1f83d9abU,
                                      0x5be0cd19U};
    std::array<uint8_t, 64> buffer_ {};
    uint64_t                totalBytes_ = 0;
    size_t                  buffered_   = 0;
    bool                    finished_   = false;
};

Sha256Digest sha256File(const std::filesystem::path &path);
std::string  sha256Hex(const Sha256Digest &digest);

}  // namespace Tuning
