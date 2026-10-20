/*
 *  Rapfi, a Gomoku/Renju playing engine supporting piskvork protocol.
 *  Copyright (C) 2022  Rapfi developers
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "dataentry.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

/// The single home of the .bin / .binpack wire formats. The byte layouts here are
/// shared with external producers/consumers - the Python Trainer and c-gomoku-cli -
/// so any change to them is a cross-repo format change, not a local refactor.
/// Readers and writers on the Rapfi side must encode/decode exclusively through
/// these functions so the two directions cannot drift apart.
namespace Tuning {

/// Error thrown when dataset input is corrupted or violates the wire format.
/// Dataset readers add file/record context before propagating it to tooling.
struct DatasetError : ::std::runtime_error
{
    using ::std::runtime_error::runtime_error;
};

// ------------------------------------------------------------------------
// Field encodings shared by both formats

/// The rule value stored on the wire is the Gomocup protocol rule number, NOT
/// the internal Rule enum: 0=freestyle, 1=standard, 4=renju.
constexpr uint32_t encodeWireRule(Rule rule)
{
    return rule == RENJU ? 4 : uint32_t(rule);
}

/// Decode a wire rule number back to the internal Rule enum.
/// @return The decoded rule, or RULE_NB if the wire value is not a valid encoding
///     (callers should validate and reject such input).
constexpr Rule decodeWireRule(uint32_t wireRule)
{
    if (wireRule == 4)
        return RENJU;
    return wireRule < 2 ? Rule(wireRule) : RULE_NB;
}

/// A move is stored as a 16-bit unsigned integer, `(x << 5) | y` in the lower
/// 10 bits. Pos::NONE and Pos::PASS have no encoding (0xFFFF is written as a
/// should-not-happen marker); pass moves are representable only by records
/// that carry an explicit isPass flag.
uint16_t encodeU16Move(Pos move);
Pos      decodeU16Move(uint16_t move);

// ------------------------------------------------------------------------
// Simple binary format (.bin), one position entry per record

/// Read and decode the next .bin entry from a stream.
/// @param is Input stream positioned at a record boundary.
/// @param entry Decoded output; pass nullptr to skip the record body cheaply.
/// @param scratch Reused scratch storage for streaming loops.
/// @return False on a clean end-of-stream, otherwise true.
/// @throws DatasetError when the record is corrupted.
struct BinDecodeScratch
{
    /// Occupancy mask of the padded 32x32 coordinate space, used for duplicate
    /// stone detection without per-entry hashing or allocation.
    uint64_t seen[16];
};
bool readBinEntry(std::istream &is, DataEntry *entry, BinDecodeScratch &scratch);

/// Encode and write one .bin entry (the payload is not representable and ignored).
void writeBinEntry(std::ostream &os, const DataEntry &entry);

// ------------------------------------------------------------------------
// Packed binary format (.binpack), one game per record

/// Read and decode the next .binpack game record from a stream.
/// @param is Input stream positioned at a record boundary.
/// @param game Decoded output game (result converted to the white pov of
///     GameEntry; extra multi-pv moves land in each move's payload).
/// @param scratch Reused scratch storage for streaming loops.
/// @return False on a clean end-of-stream, otherwise true.
/// @throws DatasetError when the record is corrupted.
struct PackedDecodeScratch
{
    std::vector<char> buffer;  ///< Bulk read buffer for the move sequence.
};
bool readPackedGame(std::istream        &is,
                    GameEntry           &game,
                    PackedDecodeScratch &scratch,
                    size_t               maxRecordBytes = std::numeric_limits<size_t>::max(),
                    bool                 retainExtraPVs = true);

/// Encode and write one .binpack game record. Only extra-PV payloads are
/// representable on the wire; dense policy payloads are silently dropped
/// (callers that care should warn - see PackedBinaryDataWriter).
/// @throws DatasetError when the game's initial position contains a pass,
///     which the wire cannot represent.
void writePackedGame(std::ostream &os, const GameEntry &game);

}  // namespace Tuning
