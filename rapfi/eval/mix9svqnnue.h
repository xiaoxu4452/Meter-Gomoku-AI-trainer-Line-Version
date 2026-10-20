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

#include "../core/platform.h"
#include "evaluator.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace Evaluation::mixnet {
struct MoveCache;  // defined in mixcommon.h
}

namespace Evaluation::mix9svq {

// Weight layout and the incremental accumulator are implementation details
// (see mix9svqnnue.cpp); only the Evaluator plugin surface is public.
struct Weight;
class Accumulator;

class Evaluator : public Evaluation::Evaluator
{
public:
    Evaluator(int                   boardSize,
              Rule                  rule,
              Numa::NumaNodeId      numaNodeId,
              std::filesystem::path blackWeightPath,
              std::filesystem::path whiteWeightPath);
    ~Evaluator();

    void initEmptyBoard();
    void beforeMove(const Board &board, Pos pos);
    void afterUndo(const Board &board, Pos pos);

    ValueType evaluateValue(const Board &board, AccLevel level);
    void      evaluatePolicy(const Board &board, PolicyBuffer &policyBuffer, AccLevel level);

private:
    /// Apply all queued board changes, syncing the accumulator with the board state.
    void flushMoveCache(Color side);
    /// Record new board action, but not update accumulator instantly.
    void addCache(Color side, int x, int y, bool isUndo);

    Weight /* non-owning ptr */ *weight[2];
    std::unique_ptr<Accumulator> accumulator[2];
    std::vector<mixnet::MoveCache> moveCache[2];
};

}  // namespace Evaluation::mix9svq
