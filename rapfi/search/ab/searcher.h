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

#include "../../core/pos.h"
#include "../../core/types.h"
#include "../searcher.h"
#include "../searchoutput.h"
#include "../searchthread.h"
#include "../timecontrol.h"
#include "history.h"

#include <atomic>

namespace Search::AB {

struct ABSearchData : SearchData
{
    uint32_t         multiPv;          /// Current number of multi pv
    uint32_t         pvIdx;            /// Current searched pv index
    int              rootDepth;        /// Current searched depth
    Value            rootDelta;        /// Current window size of the root node
    Value            rootAlpha;        /// Current alpha value of the root node
    bool             singularRoot;     /// Is there only a single response at root?
    std::atomic<int> completedDepth;   /// Previously completed depth
    std::atomic<int> bestMoveChanges;  /// How many time best move has changed in this search

    MainHistory        mainHistory;         /// Heuristic history table
    CounterMoveHistory counterMoveHistory;  /// Counter move history table

    ~ABSearchData() = default;

    /// Clear all search states between two search.
    void clearData(SearchThread &th) override;
};

class ABSearcher : public Searcher
{
public:
    // Time management information
    float previousTimeReduction;  // (keep for one game)
    Value previousBestValue;      // (keep for one game)

    /// Lookup tables used for reduction/pruning, where index is depth or moveCount.
    std::array<Depth, MAX_MOVES + 1> reductions[RULE_NB];

    ABSearcher() : Searcher(true) {}
    ~ABSearcher() = default;
    std::unique_ptr<SearchData> makeSearchData(SearchThread &th) override
    {
        return std::make_unique<ABSearchData>();
    }

    /// Set the memory size limit of the search.
    void setMemoryLimit(size_t memorySizeKB) override;

    /// Get the current memory size limit of the search.
    size_t getMemoryLimit() const override;

    /// Clear main thread states (and TT) between different games.
    void clear(SearchEngine &pool, bool clearAllMemory) override;

    /// The algorithm middle of one search session, called by the session
    /// driver on the main search thread. Launches the worker threads and
    /// returns the best thread's first root move (or nullptr on early-outs).
    const RootMove *searchMain(SearchThread &th) override;

    /// The main iterative deepening search loop. It calls search() repeatedly with increasing depth
    /// until the stop condition is reached. Results are updated to thread bounded with the board.
    void search(SearchThread &th) override;

    /// Checks if current search reaches timeup condition.
    bool checkTimeupCondition(const TimeControl &timectl) override;

private:
    /// Choose the next search depth by checking completed depth of all other
    /// threads and selecting the next depth with least working threads to
    /// avoid repeated searching.
    int pickNextDepth(SearchEngine &threads, uint32_t thisId, int lastDepth) const;

    /// Pick thread with the best result according to eval and completed depth.
    SearchThread *pickBestThread(SearchEngine &threads) const;
};

}  // namespace Search::AB
