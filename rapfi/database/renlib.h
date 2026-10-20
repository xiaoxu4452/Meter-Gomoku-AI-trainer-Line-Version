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

#include "../core/types.h"

#include <cstddef>
#include <functional>
#include <iosfwd>
#include <string>

class Board;

namespace Database {
class DBClient;
}

/// Reader/writer for the Renlib opening library file format
/// (https://github.com/gomoku/Renlib). A lib file is a game tree serialized
/// in preorder: each node is a move byte plus a flag byte, followed by
/// optional comment/text strings; sibling and no-child flag bits delimit the
/// tree structure. Moves are stored as two 4-bit coordinates in one byte, so
/// the format supports board sizes up to 15 only.
namespace Renlib {

/// Maximum board size storable in a Renlib move byte (4-bit coordinates).
constexpr int MAX_RENLIB_BOARD_SIZE = 15;

/// Callback invoked once per lib node during traversal, after the node's move
/// has been played on the board. `text`/`comment` are null when the node does
/// not carry them.
using TraverseCallback = void(const Board       &board,
                              bool               hasTag,
                              const std::string *text,
                              const std::string *comment);

/// Parse a lib stream and traverse its game tree, replaying moves on a fresh
/// board of the given size and invoking the callback at each node.
/// @return The number of nodes read.
/// @note Throws std::runtime_error on malformed or truncated input.
size_t traverseLib(std::istream                  &libStream,
                   int                            boardSize,
                   Rule                           rule,
                   std::function<TraverseCallback> callback);

/// Export the database game tree reachable from the given board position
/// to a lib stream.
/// @return The number of nodes written.
/// @note Throws std::runtime_error if the board size is not supported.
size_t exportDatabase(std::ostream       &libStream,
                      Database::DBClient &dbClient,
                      const Board        &board,
                      Rule                rule);

}  // namespace Renlib
