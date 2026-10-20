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

#include "searchthread.h"

#include "../core/platform.h"
#include "../database/dbclient.h"
#include "../database/dbconfig.h"
#include "../eval/evaluator.h"
#include "../game/board.h"
#include "searcher.h"

namespace Search {

SearchThread::SearchThread(SearchEngine &searchEngine, uint32_t id)
    : numaId(Numa::DefaultNumaNodeId)
    , running(false)
    , exit(false)
    , id(id)
    , engine(searchEngine)
{}

void SearchThread::init(bool bindGroup)
{
#ifdef MULTI_THREADING
    thread = std::thread(&SearchThread::threadLoop, this);
#endif

    runTask([bindGroup](SearchThread &th) {
        // Create search data for this thread
        th.searchData = th.engine.searcher()->makeSearchData(th);

        // Set thread affinity to a specific group if needed
        if (bindGroup) {
            // If OS already scheduled us on a different group than 0 then don't overwrite
            // the choice, eventually we are one of many one-threaded processes running on
            // some Windows NUMA hardware, for instance in fishtest. To make it simple,
            // just check if running threads are below a threshold, in this case all this
            // NUMA machinery is not needed. We also store this thread's numa ID for the
            // later NUMA-aware loading of evaluator weights.
            th.numaId = Numa::bindThisThread(th.id);
        }
    });
}

SearchThread::~SearchThread()
{
    exit = true;

#ifdef MULTI_THREADING
    runTask(nullptr);
    thread.join();
#endif
}

void SearchThread::runTask(std::function<void(SearchThread &)> task)
{
#ifdef MULTI_THREADING
    if (std::this_thread::get_id() == thread.get_id()) {
        // We *are* the worker => enqueue "tail task" without waiting.
        std::lock_guard<std::mutex> lock(mutex);
        // at this point running is still true, we simply replace the functor
        taskFunc = std::move(task);
    }
    else {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return !running; });
        taskFunc = std::move(task);
        running  = true;
        lock.unlock();
        cv.notify_one();
    }
#else
    if (task)
        task(*this);
#endif
}

void SearchThread::waitForIdle()
{
#ifdef MULTI_THREADING
    // Check deadlock if we are already in the worker thread
    assert(std::this_thread::get_id() != thread.get_id());

    if (!running)
        return;

    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return !running; });
#endif
}

#ifdef MULTI_THREADING
void SearchThread::threadLoop()
{
    while (true) {
        std::function<void(SearchThread &)> task;

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!taskFunc) {
                running = false;
                cv.notify_all();
                cv.wait(lock, [&] { return running || exit; });
            }

            if (exit)
                return;

            std::swap(task, taskFunc);
        }

        if (task)
            task(*this);
    }
}
#endif

void SearchThread::clear()
{
    if (searchData)
        searchData->clearData(*this);
    rootMoves.clear();
    balance2Moves.clear();
    numNodes = 0;
    selDepth = 0;

    // Setup dbClient for each thread
    if (engine.dbStorage() && (!dbClient || &dbClient->getStorage() != engine.dbStorage())) {
        dbClient = std::make_unique<Database::DBClient>(*engine.dbStorage(),
                                                        Database::RECORD_MASK_LVDB,
                                                        Database::DatabaseCfg.cacheSize,
                                                        Database::DatabaseCfg.recordCacheSize);
    }
}

void SearchThread::setBoardAndEvaluator(const Board &board)
{
    // Reset board instance in this thread to be null
    this->board.reset();

    // Setup evaluator in this thread
    if (!engine.evaluatorMaker)
        evaluator.reset();
    else {
        const int  boardSize = board.size();
        const Rule rule      = engine.ctx.options.rule;

        // Clear loaded evaluator that does not match
        if (evaluator && (evaluator->boardSize != boardSize || evaluator->rule != rule))
            evaluator.reset();

        if (!evaluator)
            evaluator = engine.evaluatorMaker(boardSize, rule, numaId);
    }

    // Clone the board (this will also sync the evaluator to the board state)
    this->board = std::make_unique<Board>(board, this);
}

}  // namespace Search
