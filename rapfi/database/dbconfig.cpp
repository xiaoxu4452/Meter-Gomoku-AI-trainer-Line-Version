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

#include "dbconfig.h"

#include "../core/iohelper.h"
#include "../core/utils.h"
#include "yxdbstorage.h"

#include <filesystem>

namespace Database {

DatabaseConfig DatabaseCfg;

std::unique_ptr<DBStorage> openDBStorage(const DatabaseConfig &cfg, std::string utf8URL)
{
    if (cfg.type == "yixindb") {
        auto dbPath = std::filesystem::u8path(utf8URL.empty() ? cfg.url : utf8URL);
        return std::make_unique<YXDBStorage>(dbPath,
                                             cfg.yixindb.compressedSave,
                                             cfg.yixindb.saveOnClose,
                                             cfg.yixindb.numBackupsOnSave,
                                             cfg.yixindb.ignoreCorrupted);
    }
    throw std::invalid_argument("unknown database type " + cfg.type);
}

std::unique_ptr<DBStorage> createDBStorage(const DatabaseConfig &cfg, std::string utf8URL)
{
    // Silent nullptr reproduces the old null-DatabaseMaker sentinel: a maker
    // exists iff the last committed config load got through the yixindb
    // options. loadConfig publishes the parsed [database] table atomically,
    // so a true factoryEnabled always pairs with type == "yixindb" - no
    // half-parsed state can reach this factory.
    if (!cfg.factoryEnabled)
        return nullptr;

    try {
        auto dbPath    = std::filesystem::u8path(utf8URL.empty() ? cfg.url : utf8URL);
        bool existing  = std::filesystem::exists(dbPath);
        auto startTime = now();
        MESSAGEL("Opening yixin database at " << pathToConsoleString(dbPath) << " ...");
        auto dbStorage = openDBStorage(cfg, std::move(utf8URL));
        if (existing)
            MESSAGEL("Loaded Yixin database (" << dbStorage->size() << " records) using "
                                               << (now() - startTime) << " ms.");
        return dbStorage;
    }
    catch (const std::exception &e) {
        ERRORL("Failed to create yixin database: " << e.what());
        return nullptr;
    }
}

}  // namespace Database
