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

namespace Search {

/// SearchConfig groups every search knob read from the "[search]" TOML table
/// (AB knobs first, then MCTS knobs - one struct, mirroring the single table).
/// One mutable global instance (SearchCfg) holds the live configuration:
/// config.cpp's readSearch fills it, consumers read fields directly, and
/// runtime mutation (benchmark save/restore, opengen/selfplay overrides) is
/// plain field assignment. Default member initializers define the startup
/// state; per-key reload semantics live in readSearch.
struct SearchConfig
{
    // Parameters for alpha-beta search

    /// Whether to enable aspiration window.
    bool aspirationWindow = true;
    /// Whether to filter redundant symmetry moves at root.
    bool filterSymmetryRootMoves = true;
    /// Number of iterations after we found a mate.
    int numIterationAfterMate = 6;
    /// Number of iterations after we found a singular root.
    int numIterationAfterSingularRoot = 4;
    /// Max depth to search.
    int maxSearchDepth = 99;

    // Parameters for MCTS search

    /// Expand node (evaluating policy) when first evaluate a node (evaluating value).
    bool expandWhenFirstEvaluate = false;
    /// The maximum number of visits per playout in MCTS search.
    int maxNumVisitsPerPlayout = 100;
    /// How many nodes to print root moves in MCTS search. (Positive number to enable)
    int nodesToPrintMCTSRootmoves = 0;
    /// How much milliseconds to print root moves in MCTS search. (Positive number to enable)
    int timeToPrintMCTSRootmoves = 1000;
    /// Maximum number of non-pv root moves to print in MCTS search.
    int maxNonPVRootmovesToPrint = 10;
    /// Maximum number of search nodes after we found that we are in singular root.
    int numNodesAfterSingularRoot = 100;
    /// The power of two number of shards that the node table has.
    int numNodeTableShardsPowerOfTwo = 10;
    /// The ratio to decrase utility when child draw rate is high.
    float drawUtilityPenalty = 0.35f;
    /// Maximum size of the VCF TT slice carved from the MCTS memory budget, in KiB.
    int mctsVCFTTMaxSizeKB = 8192;
    /// Divisor of the MCTS memory budget offered to the VCF TT (slice = budget / divisor).
    int mctsVCFTTBudgetDivisor = 16;
};

/// TimeConfig groups the time-management knobs from "[search.timectl]".
struct TimeConfig
{
    /// Time reserved for delay in communication between engine and GUI.
    int turnTimeReserved = 30;
    /// Number of moves spared for the rest of game
    float matchSpace = 22.0f;
    /// Minimum number of moves spared for the rest of game
    float matchSpaceMin = 7.0f;
    /// Average branch factor to whether next depth has enough time
    float averageBranchFactor = 1.7f;
    /// Exit search if turn time is used more than this ratio (even given ample match time)
    float advancedStopRatio = 0.9f;
    /// Plan time management at most this many moves ahead
    int moveHorizon = 64;
    /// Bias of time divisor factor to depth
    float timeDivisorBias = 1.25f;
    /// Scale of time divisor factor to depth
    float timeDivisorScale = 0.02f;
    /// Pow to depth in time divisor factor
    float timeDivisorDepthPow = 1.4f;
    /// Scale of score to falling factor
    float fallingFactorScale = 0.0032f;
    /// Offset of score to falling factor
    float fallingFactorBias = 0.544f;
    /// Scale of best move stable ply to reduction factor
    float bestmoveStableReductionScale = 0.0125f;
    /// Power of previous time reduction factor to get current factor
    float bestmoveStablePrevReductionPow = 0.528f;
};

/// The global live search/time configuration.
extern SearchConfig SearchCfg;
extern TimeConfig   TimeCfg;

}  // namespace Search
