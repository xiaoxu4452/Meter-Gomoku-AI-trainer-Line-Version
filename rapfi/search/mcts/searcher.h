/*
 *  Rapfi, a Gomoku/Renju playing engine supporting piskvork protocol.
 *  Copyright (C) 2024  Rapfi developers
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
#include "../searchoutput.h"
#include "../searchthread.h"
#include "../timecontrol.h"
#include "node.h"
#include "nodetable.h"

#include <atomic>

namespace Search::MCTS {

class MCTSSearcher : public Searcher
{
public:
    /// The node table for storing and finding all transposition nodes.
    /// Per-game memory: fully cleared only by clear(clearAllMemory=true).
    std::unique_ptr<NodeTable> nodeTable;

    /// Cross-search graph-reuse state. The node table is transposition-aware,
    /// so this is a Monte-Carlo search *graph*, not a tree.
    /// Lifecycle (preserved semantics): clear() nulls root only; globalNodeAge
    /// resets only with clearAllMemory; previousPosition is never reset - a
    /// stale one can skip recycleOldNodes on the first search of a new game
    /// (documented quirk, queued as a behavior-change candidate).
    struct GraphState
    {
        /// The root node of the MCTS search graph
        Node *root = nullptr;
        /// The searched position of last root node
        std::vector<Pos> previousPosition;
        /// The global node age to synchronize the node table
        uint32_t globalNodeAge = 0;
    } graph;

    /// Per-search scratch, reset at the top of searchMain.
    struct SearchScratch
    {
        /// The number of selectable root moves, set by updateRootMovesData().
        uint32_t numSelectableRootMoves;
        /// The last number of nodes that we have printed search outputs
        uint64_t lastOutputNodes;
        /// The last time that we have printed search outputs
        Time lastOutputTime;
    } scratch;

    /// Total requested MCTS search-memory budget in KiB, as passed to
    /// setMemoryLimit (0 = no budget set: graph unbounded, VCF TT at its
    /// default maximum). Written only between searches, like TT resizing.
    size_t memoryLimitKB = 0;

    /// Graph memory budget in bytes, derived from memoryLimitKB by
    /// setMemoryLimit. May legitimately be zero (tiny budgets); "unbounded"
    /// is signalled by memoryLimitKB == 0, never by this field.
    size_t graphMemoryLimit = 0;

    /// Estimated bytes of live graph memory (nodes + edge arrays +
    /// per-allocation overhead constants). Updated at node insertion, edge
    /// creation, and the recycle sweep; survives clear(false) together with
    /// the node table contents; zeroed by clear(true).
    std::atomic<size_t> graphMemoryUsed {0};

    MCTSSearcher();
    ~MCTSSearcher() = default;

    std::unique_ptr<SearchData> makeSearchData(SearchThread &th) override { return nullptr; }

    /// Set the memory size limit of the search.
    void setMemoryLimit(size_t memorySizeKB) override;

    /// Get the current memory size limit of the search.
    size_t getMemoryLimit() const override;

    /// Clear the state of the searcher between two different games
    void clear(SearchEngine &pool, bool clearAllMemory) override;

    /// The algorithm middle of one search session, called by the session
    /// driver on the main search thread. Launches the worker threads and
    /// returns the top-ranked root move (or nullptr on early-outs).
    const RootMove *searchMain(SearchThread &th) override;

    /// The main best first search loop. It calls search() repeatedly until the stop
    /// condition is reached. Results are updated to thread bounded with the board.
    void search(SearchThread &th) override;

    /// Checks if current search reaches timeup condition.
    bool checkTimeupCondition(const TimeControl &timectl) override;

private:
    /// Setup root node for the search
    void setupRootNode(SearchThread &th);

    /// Garbage collect all old nodes in the node table
    void recycleOldNodes(SearchThread &th);

    /// Rank the root moves and update PV, then print all root moves
    void updateRootMovesData(SearchThread &th);
};

}  // namespace Search::MCTS
