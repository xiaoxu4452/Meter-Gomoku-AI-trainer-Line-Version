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

#include <memory>
#include <string>

namespace Search {

class SearchThread;  // forward declaration
class SearchEngine;    // forward declaration
class TimeControl;   // forward declaration
struct RootMove;     // forward declaration

struct SearchData
{
    virtual ~SearchData() = default;

    /// Clear the states of search data for a new search.
    virtual void clearData(SearchThread &th) = 0;
};

/// Searcher is the base class for implementation of all search algorithms.
class Searcher
{
public:
    /// Preserves the current output asymmetry of the two searchers on the
    /// instant-win path: whether the session driver prints a bestmove-without-
    /// search message when an immediate A_FIVE win is found (AB does, MCTS
    /// does not). Pending unification as a separate behavior-changing commit.
    const bool printsTrivialBestmove;

    explicit Searcher(bool printsTrivialBestmove) : printsTrivialBestmove(printsTrivialBestmove)
    {}
    virtual ~Searcher() = default;

    /// Creates a instance of search data for one search thread.
    virtual std::unique_ptr<SearchData> makeSearchData(SearchThread &th) = 0;

    /// Set the memory size limit of the search.
    /// @param memorySizeKB Maximum memory size in KiB. Should be greater than zero.
    virtual void setMemoryLimit(size_t memorySizeKB) = 0;
    /// Get the current memory size limit of the search.
    /// @return Maximum memory size in KiB.
    virtual size_t getMemoryLimit() const = 0;

    /// Clear all searcher states between different games.
    /// @param engine The search engine that holds all the search threads.
    /// @param clearAllMemory Whether to clear all memory (TT, etc.)
    /// @note All threads in the engine are guaranteed to be created by this searcher.
    virtual void clear(SearchEngine &engine, bool clearAllMemory) = 0;

    /// Main-thread search entry point: the algorithm middle of one search
    /// session, called by the session driver after the trivial cases have been
    /// handled and the clock started. It is responsible for starting all other
    /// threads.
    /// @return The root move carrying the final choice (the driver then applies
    ///     the common result finalization), or nullptr if the search concluded
    ///     internally (early-out paths keep their exact semantics and skip the
    ///     common finalization).
    virtual const RootMove *searchMain(SearchThread &th) = 0;
    /// Worker-thread search entry point. This function is called by each worker thread.
    virtual void search(SearchThread &th) = 0;

    /// Checks if a search reaches timeup condition. The timeup policy differs
    /// per algorithm (AB stops at maximum; MCTS stops at optimum).
    /// @param timectl The time controller of the current search context.
    /// @return True if time is up, otherwise false.
    virtual bool checkTimeupCondition(const TimeControl &timectl) = 0;
};

/// Create a searcher instance by name ("alphabeta" or "mcts", case-insensitive).
/// @param searcherName The name of the searcher. Empty string to use default.
/// @return The pointer to the searcher. Can not be nullptr (unknown names fall
///     back to the default alpha-beta searcher with an error message).
std::unique_ptr<Searcher> createSearcher(std::string searcherName = "");

}  // namespace Search
