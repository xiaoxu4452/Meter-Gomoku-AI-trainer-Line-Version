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

#include "tunecorpus.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

namespace Tuning {

/// Writes one prepared corpus shard with fixed little-endian fields and
/// per-section CRC32 checksums. The target is published by atomic rename.
void writePreparedShard(const std::filesystem::path &path,
                        const PreparedCorpus        &corpus,
                        const std::string           &fingerprint  = {},
                        uint64_t                     shardOrdinal = 0);

/// Loads and fully validates one prepared corpus shard.
PreparedCorpus readPreparedShard(const std::filesystem::path &path,
                                 bool                         verifyChecksum = true,
                                 size_t maxAllocationBytes = std::numeric_limits<size_t>::max(),
                                 const std::string &expectedFingerprint  = {},
                                 uint64_t           expectedShardOrdinal = 0);

}  // namespace Tuning
