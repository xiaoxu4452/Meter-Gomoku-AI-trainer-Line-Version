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

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

class Board;

namespace Tuning {

/// Controls how move payloads are converted into policy targets.
/// A zero multi-pv temperature preserves the best-move one-hot fallback.
struct PolicyTargetConfig
{
    float multiPVTemperature = 0.0f;
    float evalScalingFactor  = 200.0f;

    bool useMultiPV() const { return multiPVTemperature > 0.0f; }
};

/// Encodes one training position (a board state plus its move's targets) into the
/// numpy training-plane layout. This is the seam where neural-network input design
/// lives: new input features belong here, not in the npz writer, which only owns
/// batching and serialization.
///
/// The destination pointers address ONE entry's slice of the caller's batch arrays
/// (the caller applies the per-entry stride). Layout notes:
/// - Planes are flattened with the entry's own board size (posIdx = y * size + x),
///   zero-padded up to the batch-wide numCells.
/// - binaryPacked packs three bit-planes (in-board, self stones, opponent stones)
///   big-endian bitwise, each zero-padded to a round byte.
/// - sparseU8/sparseU16 may be null to skip the sparse pattern features.
struct EntryFeatureDst
{
    uint8_t  *binaryPacked;   ///< [3][numBytes] packed binary planes
    uint8_t  *sparseU8;       ///< [10][numCells] pattern/pattern4 features, nullable
    uint16_t *sparseU16;      ///< [2][numCells] pattern-code features, nullable
    float    *globalInput;    ///< [1] side to move (black=-1, white=1)
    float    *globalTargets;  ///< [3] win-loss-draw probability, side-to-move pov
    uint16_t *policyTarget;   ///< [numCells+1] policy quantized to uint16 (pass at the end)
};

/// Reused per-entry plane scratch, allocated once per batch.
struct FeatureEncodeScratch
{
    std::vector<uint8_t> inBoardPlane, selfPlane, oppoPlane;
    std::vector<float>   policyWeights;
};

/// Encode one policy target into the quantized numpy layout.
/// Dense policy payloads take precedence. Otherwise, when enabled and every
/// multi-pv evaluation is usable, policy is a temperature softmax over win rates;
/// all other inputs fall back to a one-hot target on the best move.
void encodePolicyTarget(Pos                       bestMove,
                        Eval                      bestEval,
                        const MovePayload        &payload,
                        int                       boardsize,
                        size_t                    numCells,
                        const PolicyTargetConfig &config,
                        uint16_t                 *dst,
                        FeatureEncodeScratch     &scratch);

/// Encode one entry from the current `board` state.
/// @param board Board holding the entry's position (side to move = the entry's side).
/// @param bestMove The move output for this position (policy fallback target).
/// @param bestEval The evaluation of bestMove (side-to-move pov).
/// @param payload The move's payload (dense policy target or extra PVs).
/// @param policyConfig Policy-target conversion settings snapshotted by the writer.
/// @param result Game result from the side to move's pov (used without soft target).
/// @param softValueTarget Optional soft (win,loss,draw) value target override.
/// @param numCells Batch-wide cell count (maxBoardSize^2); planes are padded to it.
/// @param dst Destination slice of the batch arrays for this entry.
/// @param scratch Reused scratch buffers.
void encodeEntryFeatures(const Board                               &board,
                         Pos                                        bestMove,
                         Eval                                       bestEval,
                         const MovePayload                         &payload,
                         const PolicyTargetConfig                  &policyConfig,
                         Result                                     result,
                         const std::optional<std::array<float, 3>> &softValueTarget,
                         size_t                                     numCells,
                         const EntryFeatureDst                     &dst,
                         FeatureEncodeScratch                      &scratch);

/// Dimension (number of distinct values) of each sparse spatial input channel,
/// in the channel order of EntryFeatureDst::sparseU8 then sparseU16.
const std::vector<uint32_t> &sparseInputDims();

}  // namespace Tuning
