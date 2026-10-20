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

#include "../core/pos.h"  // Pos (parseLegalCoord)

#include <filesystem>
#include <iostream>  // std::cin default argument
#include <optional>

class Board;  // forward declaration
namespace Search {
struct SearchOptions;  // forward declaration (searchcommon.h)
}
namespace Database {
class DBStorage;  // forward declaration
}

namespace Command {

// --- Protocol-input helpers shared by gomocup.cpp and the DB handlers ---
// (moved from gomocup.cpp's anonymous namespace; gomocup.cpp keeps calling
// them unqualified via using-declarations)

/// Reads a file path from one input line (used by protocol commands that take
/// a path argument).
std::filesystem::path readPathFromInput(std::istream &in = std::cin);

/// Detects an illegal consecutive pass: returns true (after ERRORL) if the
/// last move was already a pass, false otherwise. (The name reads oddly - true
/// means "reject"; parseLegalCoord treats a true result as failure. Moved
/// verbatim; parseLegalCoord depends on it.)
bool checkLastMoveIsNotPass(Board &board);

/// Parses one coordinate from the stream and validates it against the board
/// (moved verbatim; used by TURN/BOARD in gomocup.cpp and by the DB position
/// handlers here).
std::optional<Pos> parseLegalCoord(std::istream &is, Board &board);

/// Everything a gomocup database protocol handler needs from the protocol
/// loop. Built fresh per dispatch by gomocup.cpp.
struct DBCommandContext
{
    /// Protocol argument stream (gomocup: std::cin).
    std::istream &in;
    /// Current game board - NULLABLE: null before the first board-allocating
    /// command. Handlers dispatched behind CheckBoardOK may assume non-null;
    /// libToDatabase deliberately handles null (board ? board->size() : 15).
    Board *board;
    /// Current search options (rule etc.).
    Search::SearchOptions &options;
    /// The engine-attached storage at dispatch time - may be null; every
    /// handler checks before use, as before.
    Database::DBStorage *storage;
};

// The gomocup database protocol handlers (moved verbatim from gomocup.cpp;
// protocol output is unchanged). setDatabase stays in gomocup.cpp - it
// installs a new storage into the engine, which this context does not expose.
void saveDatabase(DBCommandContext &ctx);
void databaseToTxt(DBCommandContext &ctx, bool currentBoardSizeAndRule);
void libToDatabase(DBCommandContext &ctx);
void databaseToLib(DBCommandContext &ctx);
void getDatabasePosition(DBCommandContext &ctx);
void queryDatabaseAll(DBCommandContext &ctx, bool getPosition);
void queryDatabaseOne(DBCommandContext &ctx, bool getPosition);
void queryDatabaseText(DBCommandContext &ctx, bool getPosition);
void editDatabaseTVD(DBCommandContext &ctx);
void editDatabaseText(DBCommandContext &ctx);
void editDatabaseBoardLabel(DBCommandContext &ctx);
void deleteDatabaseAll(DBCommandContext &ctx, bool getPosition);
void deleteDatabaseOne(DBCommandContext &ctx, bool getPosition);
void splitDatabase(DBCommandContext &ctx);
void mergeDatabase(DBCommandContext &ctx);

}  // namespace Command
