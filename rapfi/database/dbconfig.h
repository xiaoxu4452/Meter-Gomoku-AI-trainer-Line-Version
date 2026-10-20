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

#include "dbstorage.h"
#include "dbtypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace Database {

/// DatabaseConfig groups every knob read from the "[database]" TOML table and
/// its sub-tables. One mutable global instance (DatabaseCfg) holds the live
/// configuration: config.cpp's readDatabase fills it (per-key reload
/// semantics are deliberately mixed - some keys only apply on restart),
/// consumers read fields directly, and runtime mutation (the gomocup
/// DATABASE_READONLY toggle, YXSETDATABASE) is plain field assignment.
/// Search reads go through DatabaseSearchParams::captureFromConfig(), which
/// snapshots the search-gating knobs once per search (the overwrite biases
/// are read live inside DBClient - see checkOverwrite's default overload).
struct DatabaseConfig
{
    // [database]

    /// Whether to enable database by default
    bool defaultEnabled = false;
    /// Legacy code page to use for early database files and imported library files.
    uint16_t legacyFileCodePage = 0;
    /// The type of database storage
    std::string type;
    /// The URL of database storage (in utf-8 encoding)
    std::string url;
    /// Database client cache sizes
    size_t cacheSize       = 4096;
    size_t recordCacheSize = 32768;

    // [database.yixindb] - was function-locals captured into DatabaseMaker
    struct Yixindb
    {
        bool compressedSave   = true;
        bool saveOnClose      = true;
        int  numBackupsOnSave = 1;
        bool ignoreCorrupted  = false;
    } yixindb;

    /// Parse state, not a TOML knob: mirrors the legacy null-DatabaseMaker
    /// sentinel. True only when the last committed "[database]" parse got
    /// through the yixindb options; createDBStorage returns a silent nullptr
    /// while false. loadConfig publishes the parsed table atomically, so the
    /// live value always pairs with a consistently committed type/url/yixindb
    /// (a failed reload keeps the previous values, including this flag).
    bool factoryEnabled = false;

    // [database.libfile]
    struct LibFile
    {
        /// Mapping of marks in library file
        char blackWinMark  = 'a';
        char whiteWinMark  = 'a';
        char blackLoseMark = 'c';
        char whiteLoseMark = 'c';
        /// Ignore all comments in imported library file
        bool ignoreComment = false;
        /// Ignore all board texts in imported library file
        bool ignoreBoardText = false;
    } libfile;

    // [database.search]
    struct Search
    {
        /// Whether to write/update the database in search
        bool readonlyMode = false;
        /// Whether to always write parent node if any of the children are written
        bool mandatoryParentWrite = true;

        /// Search before this ply is required to query the database
        int queryPly = 3;
        /// How many iteration needed to increase one database query ply
        int queryPVIterPerPlyIncrement = 1;
        /// How many iteration needed to increase one database query ply
        int queryNonPVIterPerPlyIncrement = 2;

        /// PV node before this ply is required to write the database
        int pvWritePly = 1;
        /// How many depth needed to add a new record in PV node
        int pvWriteMinDepth = 25;
        /// NonPV node before this ply is required to write the database
        int nonPVWritePly = 0;
        /// How many depth needed to add a new record in NonPV node
        int nonPVWriteMinDepth = 25;
        /// The range of value allowed to write in PV/NonPV node
        int writeValueRange = 800;
        /// Mate node before this ply is required to write the database
        int mateWritePly = 2;
        /// How many depth needed to add a new record in exact Mate node
        int mateWriteMinDepthExact = 20;
        /// How many depth needed to add a new record in non-exact Mate node
        int mateWriteMinDepthNonExact = 40;
        /// For mate longer than this step, we will try to write the record
        int mateWriteMinStep = 10;

        /// For record found less then this ply, it will try to overwrite it with exact record
        int exactOverwritePly = 100;
        /// For record found less then this ply, it will try to overwrite it with non-exact record
        int nonExactOverwritePly = 0;
        /// The overwrite rule to write the database
        OverwriteRule overwriteRule = OverwriteRule::BetterValueDepthBound;
        /// The bias added to the exact bound when comparing
        int overwriteExactBias = 3;
        /// The bias added to the old depth bound when comparing
        int overwriteDepthBoundBias = -1;
        /// The bias added to the queried depth bound when comparing
        int queryResultDepthBoundBias = 0;
    } search;
};

/// The global live database configuration.
extern DatabaseConfig DatabaseCfg;

/// Open a database storage from config - the throwing core. Does NOT log.
/// @param cfg Database configuration (type + backend options).
/// @param utf8URL URL of the database (utf-8); empty to use cfg.url.
/// @return The storage instance (never nullptr).
/// @throws std::invalid_argument for an unknown (or empty) cfg.type;
///     backend construction failures (e.g. DBStorageError) propagate.
std::unique_ptr<DBStorage> openDBStorage(const DatabaseConfig &cfg, std::string utf8URL = "");

/// Create a database storage from config - the protocol-facing wrapper.
/// Returns nullptr silently unless cfg.factoryEnabled (the old
/// null-DatabaseMaker sentinel: empty type, unknown-type leftovers from a
/// failed parse, or a parse that threw mid-[database]); otherwise logs the
/// opening/loaded messages and converts any failure into ERRORL + nullptr.
std::unique_ptr<DBStorage> createDBStorage(const DatabaseConfig &cfg, std::string utf8URL = "");

}  // namespace Database
