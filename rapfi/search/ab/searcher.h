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
#ifdef GB_YBWC_ROOT
    #include "searchstack.h"
#endif

#include <atomic>
#ifdef GB_YBWC_ROOT
    #include <condition_variable>
    #include <mutex>
    #include <vector>
#endif

namespace Search::AB {

#ifdef GB_YBWC_ROOT
/// Shared descriptor for the experimental root-split search (YBWC prototype).
/// The main thread owns one instance per searcher; worker threads wait for a
/// depth ticket, claim root moves from the shared queue and publish results.
/// This is a build-flagged experiment only; the default search path is Lazy SMP.
struct RootSplitShared
{
    std::mutex              mutex;
    std::condition_variable ticketCv;  ///< wakes workers on new depth / siblings open / stop
    std::condition_variable doneCv;    ///< wakes main when every claimed move finished
    std::atomic<uint32_t>   ticket {0};  ///< incremented once per dispatched depth
    std::atomic<uint32_t>   nextMove {0};      ///< next root move index to claim
    std::atomic<uint32_t>   remaining {0};     ///< claimed but unfinished moves
    std::atomic<bool>       siblingsOpen {false};
    std::atomic<int>        bestValue {VALUE_NONE};  ///< shared alpha across siblings
    bool                    stop = false;            ///< guarded by mutex
    bool                    splitPhaseOver = false;  ///< shallow split phase finished
    int                     depth = 0;               ///< guarded by mutex
    uint32_t                moveCount = 0;           ///< guarded by mutex
    Value                   prevValue = VALUE_NONE;  ///< guarded by mutex (no-gate mode)
    std::vector<Pos>        moves;                   ///< guarded by mutex at dispatch only
    std::vector<Value>      values;
    std::vector<std::vector<Pos>> pvs;
};
#endif

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
#ifdef GB_YBWC_ROOT
    uint32_t ybwcTicket = 0;  /// Last root-split ticket served by this thread
#endif

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

#ifdef GB_YBWC_ROOT
    /// Whether the current search session runs the cooperative root split.
    /// Set once per search in searchMain() before workers are dispatched.
    bool ybwcThisSearch = false;

    /// Shared root-split work descriptor (guarded by its own mutex).
    RootSplitShared ybwcWork;

    /// Worker-side loop: wait for a depth ticket, claim root moves and search
    /// them. Returns when the main thread stops the session.
    void assistRootSplit(SearchThread &th);

    /// Serve one shallow cooperative depth: wait for the current ticket, join
    /// the sibling claims and return. Used only in the hybrid split phase.
    void assistRootSplitDepth(SearchThread &th, StackArray &stackArray);

    /// Signal that the shallow split phase ended; waiting workers must fall
    /// back to their own Lazy SMP iteration.
    void notifyRootSplitPhaseOver();

    /// Claim and search root moves of one depth ticket. Used by both the main
    /// thread (after opening the siblings) and the helpers.
    void runRootSplitClaims(SearchThread &th,
                            StackArray   &stackArray,
                            Rule          rule,
                            uint32_t      ticket,
                            int           depth,
                            uint32_t      moveCount);

    /// Main-side depth body: search the previous PV move alone, then release
    /// the remaining root moves to all threads and commit the sorted result.
    void cooperativeRootSearch(SearchThread &th, ABSearchData &sd, Depth depth);

    /// Wake up workers and let them leave the current task.
    void stopRootSplitWorkers();
#endif
};

}  // namespace Search::AB
