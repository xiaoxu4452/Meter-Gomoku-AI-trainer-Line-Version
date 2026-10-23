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

#include "../game/board.h"

#include <memory>
#include <utility>
#include <vector>
#ifdef MULTI_THREADING
    #include <thread>
#endif

namespace Database {

/// Permute a child traversal order based on the walker's thread id, so that
/// concurrent tree walkers start from different subtrees instead of contending
/// on the same one. Thread 0 keeps the natural order; work is still claimed
/// through the storage itself, the permutation only reduces collisions.
inline void permuteForThread(std::vector<Pos> &order, int threadId)
{
    if (threadId > 0) {
        for (size_t i = 0; i < order.size(); i += (threadId + 1) / 2) {
            size_t swapIndex = (i * (threadId + 1)) % order.size();
            std::swap(order[i], order[swapIndex]);
        }
    }
}

/// Run a database tree walk on every hardware thread, each walker getting its
/// own clone of the board and its thread id (for permuteForThread). Blocks
/// until all walkers finish. Runs a single walker inline without threading
/// support.
/// @param walk Callable with signature `void(Board &, int threadId)`.
template <typename F>
void parallelWalkOnBoardClones(const Board &board, F &&walk)
{
#ifdef MULTI_THREADING
    const int                numThreads = (int)std::thread::hardware_concurrency();
    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    for (int i = 0; i < numThreads; i++) {
        threads.emplace_back(
            [&walk, id = i](std::unique_ptr<Board> board) { walk(*board, id); },
            std::make_unique<Board>(board, nullptr));
    }

    for (auto &th : threads)
        th.join();
#else
    auto boardClone = std::make_unique<Board>(board, nullptr);
    walk(*boardClone, 0);
#endif
}

}  // namespace Database
