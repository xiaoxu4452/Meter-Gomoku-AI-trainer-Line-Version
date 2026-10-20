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

#include "dbclient.h"

#include "../game/board.h"
#include "../game/scopedmove.h"
#include "dbconfig.h"
#include "parallelwalk.h"

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <set>

namespace {

using namespace Database;

/// Compare two label to see which one is more valuable
/// @return 1 if lhs label is more valuable, -1 if rhs label is more valuable.
inline int labelCompare(DBLabel lhs, DBLabel rhs)
{
    if (lhs != rhs) {
        if (isDeterminedLabel(lhs))
            return 1;
        if (isDeterminedLabel(rhs))
            return -1;
    }
    return 0;
}

/// Get the rank of the depth-bound of a record
inline int
depthBoundRank(const DBRecord &record, int overwriteExactBias, int overwriteDepthBoundBias)
{
    return record.depth()
           + (overwriteExactBias - std::min(overwriteDepthBoundBias, 0))
                 * (record.bound() == BOUND_EXACT);
};

/// Get the rank of a mate (determined) record
inline int mateRank(const DBRecord &record)
{
    return std::abs((int)record.value) * 2 + (record.bound() == BOUND_EXACT);
}

/// @brief Iterate all parent keys of a DBKey.
/// @tparam F the function to receive each parent key, the excluded stone pos and the
///     child to parent transform. Returns true to terminate the iteration in advance.
///     Signature is `bool(const DBKey&, const StonePos&, TransformType)`.
template <typename F = void>
void iterateParentKeys(DBStorage &storage, const DBKey &key, const F &f)
{
    DBKey parentKey;
    parentKey.rule           = key.rule;
    parentKey.boardWidth     = key.boardWidth;
    parentKey.boardHeight    = key.boardHeight;
    parentKey.sideToMove     = ~key.sideToMove;
    parentKey.numBlackStones = key.numBlackStones;
    parentKey.numWhiteStones = key.numWhiteStones;

    const StonePos *excludesBegin, *excludesEnd;
    if (parentKey.sideToMove == BLACK) {
        excludesBegin = key.blackStonesBegin();
        excludesEnd   = key.blackStonesEnd();
        if (parentKey.numBlackStones == 0)
            return;
        parentKey.numBlackStones--;
    }
    else {
        excludesBegin = key.whiteStonesBegin();
        excludesEnd   = key.whiteStonesEnd();
        if (parentKey.numWhiteStones == 0)
            return;
        parentKey.numWhiteStones--;
    }

    TransformType parentTransform;
    for (const StonePos *excludePos = excludesBegin; excludePos < excludesEnd; excludePos++) {
        auto it = std::copy(key.blackStonesBegin(), excludePos, parentKey.stones);
        std::copy(excludePos + 1, key.whiteStonesEnd(), it);

        // Make sure we get the smallest parent key
        toSmallestDBKey(parentKey, &parentTransform);

        if (f(parentKey, *excludePos, parentTransform))
            return;
    }
}

/// Check if this key is referenced by any other key.
template <typename IsDeletedParentKeyPredicate>
bool isKeyReferenced(DBStorage &storage, const DBKey &key, IsDeletedParentKeyPredicate pred)
{
    bool result = false;

    iterateParentKeys(storage,
                      key,
                      [&](const DBKey &parentKey, const StonePos &, TransformType) -> bool {
                          if (pred(parentKey))
                              return false;

                          DBRecord record;
                          if (storage.get(parentKey, record, RECORD_MASK_NONE)) {
                              result = true;
                              return true;  // terminate iteration now
                          }

                          return false;
                      });

    return result;
}

/// Shared coordination state for one delChildren operation.
///
/// Concurrency model of delChildren: every worker thread walks the same game
/// tree, each with its own board clone and a thread-specific permutation of the
/// move order so threads spread over different subtrees. Work is claimed through
/// the storage itself (a successful get on a not-yet-deleted key); the sets here
/// only let threads cooperate on subtrees another thread has already claimed:
///   - deletingKeys[ply] holds keys whose subtree deletion is in progress, so a
///     thread finding its key already deleted can help delete the children
///     instead of skipping them.
///   - checkingKeys holds keys whose NoDeleteRecursive subtree check is in
///     progress at the split boundary, so other threads skip the redundant walk.
/// Both are cooperation hints, not correctness requirements: the claiming thread
/// always finishes its own subtree. Tracking stops below MaxSplitPly since deep
/// collisions are rare and set contention would outweigh the help.
struct DeleteChildrenContext
{
    /// The deepest ply where deleting/checking subtrees are tracked for sharing.
    static constexpr int MaxSplitPly = 4;

    std::set<DBKey> deletingKeys[MaxSplitPly];
    std::set<DBKey> checkingKeys;
    std::mutex      mutex[MaxSplitPly + 1];  // [ply] guards deletingKeys[ply],
                                             // [MaxSplitPly] guards checkingKeys
};

/// Delete all children of a board position from the database storage.
template <bool ParentDeleted>
void recursiveDeleteChildren(DeleteChildrenContext                              &ctx,
                             DBStorage                                          &storage,
                             Board                                              &board,
                             Rule                                                rule,
                             const std::function<DBClient::DelType(DBRecord &)> &deleteFilter,
                             int                                                 threadId,
                             int                                                 ply)
{
    if (board.movesLeft() == 0)
        return;

    constexpr int MaxSplitPly  = DeleteChildrenContext::MaxSplitPly;
    auto &[deletingKeys, checkingKeys, mutex] = ctx;

    DBKey thisKey;
    if constexpr (ParentDeleted)
        thisKey = constructDBKey(board, rule);

    // Find all potential children of this board position, spread over subtrees
    std::vector<Pos> toDeletePos;
    toDeletePos.reserve(board.movesLeft());
    FOR_EVERY_EMPTY_POS(&board, pos)
    {
        toDeletePos.push_back(pos);
    }
    permuteForThread(toDeletePos, threadId);

    // Delete all children
    DBKey    key;
    DBRecord record;
    for (auto pos : toDeletePos) {
        board.move(rule, pos);

        key = constructDBKey(board, rule);

        if (storage.get(key, record, deleteFilter ? RECORD_MASK_LVDB : RECORD_MASK_NONE)) {
            switch (deleteFilter ? deleteFilter(record) : DBClient::DelType::DeleteRecursive) {
            default: break;
            case Database::DBClient::DelType::NoDeleteRecursive: {
                bool recursiveCheck = true;
                if (ply == MaxSplitPly) {
                    std::lock_guard lock(mutex[ply]);
                    if (checkingKeys.find(key) != checkingKeys.end())
                        recursiveCheck = false;
                }

                if (recursiveCheck) {
                    std::pair<std::set<DBKey>::iterator, bool> insertedResult;
                    if (ply == MaxSplitPly) {
                        std::lock_guard lock(mutex[ply]);
                        insertedResult = checkingKeys.insert(key);
                    }

                    // Recursive check all children of this key
                    recursiveDeleteChildren<true>(ctx,
                                                  storage,
                                                  board,
                                                  rule,
                                                  deleteFilter,
                                                  threadId,
                                                  ply + 1);

                    if (ply == MaxSplitPly && insertedResult.second) {
                        std::lock_guard lock(mutex[ply]);
                        checkingKeys.erase(insertedResult.first);
                    }
                }

            } break;
            case Database::DBClient::DelType::DeleteRecursive: {
                bool shouldDelete;
                if constexpr (ParentDeleted)
                    shouldDelete = !isKeyReferenced(storage, key, [&thisKey](const DBKey &key) {
                        return key == thisKey;
                    });
                else
                    shouldDelete =
                        !isKeyReferenced(storage, key, [](const DBKey &key) { return false; });

                if (shouldDelete) {
                    // Delete this key first so we can delete its children
                    storage.del(key);

                    std::pair<std::set<DBKey>::iterator, bool> insertedResult;
                    if (ply < MaxSplitPly) {
                        std::lock_guard lock(mutex[ply]);
                        insertedResult = deletingKeys[ply].insert(key);
                    }

                    // Find all children of this key and delete them
                    recursiveDeleteChildren<false>(ctx,
                                                   storage,
                                                   board,
                                                   rule,
                                                   nullptr,
                                                   threadId,
                                                   ply + 1);

                    if (ply < MaxSplitPly && insertedResult.second) {
                        std::lock_guard lock(mutex[ply]);
                        deletingKeys[ply].erase(insertedResult.first);
                    }
                }
            } break;
            }
        }
        // Help other threads to delete current key if they are deleting it but not finished yet
        else if (ply < MaxSplitPly) {
            bool shouldDelete = false;
            {
                std::lock_guard lock(mutex[ply]);
                if (deletingKeys[ply].find(key) != deletingKeys[ply].end())
                    shouldDelete = true;
            }

            if (shouldDelete)
                // Find all children of this key and delete them
                recursiveDeleteChildren<false>(ctx, storage, board, rule, nullptr, threadId, ply + 1);
        }

        board.undo(rule);
    }
}

/// Check if a DBKey is symmetry under the given transform.
bool isDBKeySymmetry(const DBKey &key, TransformType transform)
{
    if (key.boardWidth != key.boardHeight && !isRectangleTransform(transform))
        return false;

    std::map<StonePos, Color> stones;
    for (auto s = key.blackStonesBegin(); s < key.blackStonesEnd(); s++)
        stones[*s] = BLACK;
    for (auto s = key.whiteStonesBegin(); s < key.whiteStonesEnd(); s++)
        stones[*s] = WHITE;

    for (const auto &[stone, color] : stones) {
        Pos      pos       = {stone.x, stone.y};
        Pos      tPos      = applyTransform(pos, key.boardWidth, key.boardHeight, transform);
        StonePos tStonePos = {tPos.x(), tPos.y()};

        auto it = stones.find(tStonePos);
        if (it == stones.end() || it->second != color)
            return false;
    }

    return true;
}

/// Maps board positions to the canonical position that serves as their board-text
/// slot in records of a key: apply the key's canonicalizing transform, then fold
/// with min over every transform the key is symmetric under (a symmetric key makes
/// several positions equivalent, and the smallest one is the storage slot).
/// Symmetry flags are precomputed at construction since isDBKeySymmetry is not
/// cheap and one mapper is typically applied to many positions.
class CanonicalPosMapper
{
public:
    CanonicalPosMapper(const DBKey &key, int boardSize, TransformType keyTransform)
        : boardSize(boardSize)
        , keyTransform(keyTransform)
    {
        for (int t = IDENTITY + 1; t < TRANS_NB; t++)
            isKeySymmetry[t] = isDBKeySymmetry(key, (TransformType)t);
    }

    Pos operator()(Pos pos) const
    {
        Pos canonicalPos = applyTransform(pos, boardSize, keyTransform);
        for (int t = IDENTITY + 1; t < TRANS_NB; t++) {
            if (isKeySymmetry[t]) {
                Pos transformedPos = applyTransform(canonicalPos, boardSize, (TransformType)t);
                canonicalPos       = std::min(canonicalPos, transformedPos);
            }
        }
        return canonicalPos;
    }

private:
    int           boardSize;
    TransformType keyTransform;
    bool          isKeySymmetry[TRANS_NB];
};

}  // namespace

namespace Database {

DBKey constructDBKey(const Board &board, Rule rule, TransformType *transType)
{
    // Build the identity-transform key from the board's move history, then
    // reduce it to the smallest key across all symmetries.
    DBKey    key;
    StonePos whiteStones[MAX_MOVES];
    key.rule           = rule;
    key.boardWidth     = board.size();
    key.boardHeight    = board.size();
    key.sideToMove     = board.sideToMove();
    key.numBlackStones = 0;
    key.numWhiteStones = 0;

    for (int ply = 0; ply < board.ply(); ply++) {
        Pos move = board.getHistoryMove(ply);
        if (move == Pos::PASS)
            continue;

        Color c = board.get(move);
        if (c == BLACK)
            key.stones[key.numBlackStones++] = {move.x(), move.y()};
        else if (c == WHITE)
            whiteStones[key.numWhiteStones++] = {move.x(), move.y()};
    }

    std::sort(key.stones, key.stones + key.numBlackStones, std::less<StonePos>());
    std::copy(whiteStones, whiteStones + key.numWhiteStones, key.stones + key.numBlackStones);
    std::sort(key.stones + key.numBlackStones,
              key.stones + key.numBlackStones + key.numWhiteStones,
              std::less<StonePos>());

    toSmallestDBKey(key, transType);
    return key;
}

void toSmallestDBKey(DBKey &key, TransformType *transType)
{
    DBKey  transformedKeys[TRANS_NB - 1];
    int    smallestIndex = 0;
    DBKey *smallestKey   = &key;

    // Construct 8 symmetry database keys, and find the smallest one
    for (int trans = IDENTITY + 1; trans < TRANS_NB; trans++) {
        DBKey &transKey         = transformedKeys[trans - 1];
        transKey.rule           = key.rule;
        transKey.boardWidth     = key.boardWidth;
        transKey.boardHeight    = key.boardHeight;
        transKey.sideToMove     = key.sideToMove;
        transKey.numBlackStones = key.numBlackStones;
        transKey.numWhiteStones = key.numWhiteStones;

        size_t numStones = 0;
        for (const StonePos *pos = key.blackStonesBegin(); pos < key.whiteStonesEnd(); pos++) {
            Pos originPos {pos->x, pos->y};
            Pos transformedPos = applyTransform(originPos, key.boardWidth, (TransformType)trans);
            transKey.stones[numStones++] = {transformedPos.x(), transformedPos.y()};
        }

        std::sort(transKey.stones,
                  transKey.stones + transKey.numBlackStones,
                  std::less<StonePos>());
        std::sort(transKey.stones + transKey.numBlackStones,
                  transKey.stones + transKey.numBlackStones + transKey.numWhiteStones,
                  std::less<StonePos>());

        if (transKey < *smallestKey)
            smallestIndex = trans, smallestKey = &transKey;
    }

    // Copy smallest index back to the key
    if (smallestIndex > 0)
        key = *smallestKey;

    if (transType)
        *transType = static_cast<TransformType>(smallestIndex);
}

bool checkOverwrite(const DBRecord &oldRecord,
                    const DBRecord &newRecord,
                    OverwriteRule   owRule,
                    int             exactBias,
                    int             depthBoundBias)
{
    // Always overwrite null record (default constructed)
    if (oldRecord.isNull())
        return true;

    switch (owRule) {
    default:
    case OverwriteRule::Disabled: return false;
    case OverwriteRule::Always: return true;
    case OverwriteRule::BetterLabel: return labelCompare(newRecord.label, oldRecord.label) > 0;
    case OverwriteRule::BetterValue: {
        int labelResult = labelCompare(newRecord.label, oldRecord.label);
        return labelResult > 0
               || labelResult == 0 && isDeterminedLabel(oldRecord.label)
                      && mateRank(newRecord) > mateRank(oldRecord);
    }
    case OverwriteRule::BetterDepthBound: {
        int labelResult = labelCompare(newRecord.label, oldRecord.label);
        return labelResult > 0
               || labelResult == 0 && !isDeterminedLabel(oldRecord.label)
                      && depthBoundRank(newRecord, exactBias, depthBoundBias)
                             >= depthBoundRank(oldRecord, exactBias, depthBoundBias)
                                    + depthBoundBias;
    }
    case OverwriteRule::BetterValueDepthBound: {
        int labelResult = labelCompare(newRecord.label, oldRecord.label);
        return labelResult > 0
               || labelResult == 0
                      && (isDeterminedLabel(oldRecord.label)
                              ? mateRank(newRecord) > mateRank(oldRecord)
                              : depthBoundRank(newRecord, exactBias, depthBoundBias)
                                    >= depthBoundRank(oldRecord, exactBias, depthBoundBias)
                                           + depthBoundBias);
    }
    }
}

bool checkOverwrite(const DBRecord &oldRecord, const DBRecord &newRecord, OverwriteRule owRule)
{
    return checkOverwrite(oldRecord,
                          newRecord,
                          owRule,
                          DatabaseCfg.search.overwriteExactBias,
                          DatabaseCfg.search.overwriteDepthBoundBias);
}

DBClient::DBClient(DBStorage   &storage,
                   DBRecordMask recordMask,
                   size_t       dbCacheSize,
                   size_t       dbRecordCacheSize)
    : storage(storage)
    , mask(recordMask)
    , dbCache(std::max<size_t>(dbCacheSize, 1))
    , dbRecordCache(std::max<size_t>(dbRecordCacheSize, 1))
{}

DBClient::~DBClient()
{
    for (auto &[hashKey, entryCache] : dbCache)
        writeBackIfDirty(entryCache);
}

bool DBClient::query(const Board &board, Rule rule, DBRecord &record)
{
    HashKey hashKey = board.zobristKey();

    // Try find this record in dbRecordCache
    auto &[cachedHashKey, cachedRecord] = dbRecordCache[hashKey];
    if (cachedHashKey == hashKey)
        return record = cachedRecord, true;

    // Try find this database entry in dbCache
    auto entryCache = dbCache.get(hashKey);
    if (entryCache)
        return record = entryCache->record, true;

    // Read record from storage and save it in record cache
    DBKey dbKey = constructDBKey(board, rule);
    if (storage.get(dbKey, record, mask)) {
        // Save a new entry cache in dbCache
        dbCache.put(hashKey, EntryCache {dbKey, record, false}, evictedEntryCollector());

        // Save a new record cache in dbRecordCache
        dbRecordCache[hashKey] = std::make_pair(hashKey, record);

        return true;
    }

    return false;
}

std::vector<std::pair<Pos, DBRecord>> DBClient::queryChildren(const Board &board, Rule rule)
{
    DBRecord record;

    std::vector<std::pair<Pos, DBRecord>> childRecords;
    FOR_EVERY_EMPTY_POS(&board, pos)
    {
        if (rule == RENJU && board.sideToMove() == BLACK && board.checkForbiddenPoint(pos))
            continue;

        ScopedRuleMove probe(board, rule, pos);
        if (query(board, rule, record))
            childRecords.emplace_back(pos, record);
    }
    return childRecords;
}

std::vector<std::pair<Pos, std::string>> DBClient::queryBoardTexts(const Board &board, Rule rule)
{
    DBRecord record;
    if (!query(board, rule, record))
        return {};

    // Get the list of all canonical positions with board texts
    auto canonicalPosAndTexts = record.getAllBoardTexts();
    if (canonicalPosAndTexts.empty())
        return {};

    // Sort the list by canonical pos in ascending order
    std::sort(canonicalPosAndTexts.begin(),
              canonicalPosAndTexts.end(),
              [](const auto &lhs, const auto &rhs) { return int(lhs.first) < int(rhs.first); });

    // Get the list of all empty positions
    std::vector<Pos> emptyPosList;
    FOR_EVERY_EMPTY_POS(&board, pos)
    {
        if (rule == RENJU && board.sideToMove() == BLACK && board.checkForbiddenPoint(pos))
            continue;
        emptyPosList.push_back(pos);
    }

    // Find the smallest canonical pos considering symmetries to query board text
    TransformType parentTrans;
    DBKey         parentKey = constructDBKey(board, rule, &parentTrans);
    CanonicalPosMapper mapToCanonicalPos(parentKey, board.size(), parentTrans);

    // Map all empty pos into canonical pos, and get their board text
    std::vector<std::pair<Pos, std::string>> childBoardTexts;
    for (Pos pos : emptyPosList) {
        Pos canonicalPos = mapToCanonicalPos(pos);

        // Find the board text of this canonical pos
        auto itBegin = std::lower_bound(
            canonicalPosAndTexts.begin(),
            canonicalPosAndTexts.end(),
            canonicalPos,
            [](const auto &lhs, const Pos &rhs) { return int(lhs.first) < int(rhs); });
        auto itEnd = std::upper_bound(
            canonicalPosAndTexts.begin(),
            canonicalPosAndTexts.end(),
            canonicalPos,
            [](const Pos &lhs, const auto &rhs) { return int(lhs) < int(rhs.first); });
        if (itBegin < itEnd)
            childBoardTexts.emplace_back(pos, std::string {itBegin->second});
    }

    return childBoardTexts;
}

void DBClient::setBoardText(const Board &board, Rule rule, Pos pos, std::string text)
{
    if (!board.isLegal(pos))
        return;

    DBRecord record, childRecord;
    if (!query(board, rule, record))
        record = DBRecord {LABEL_NONE};

    {
        ScopedRuleMove probe(board, rule, pos);
        // Add new child record for board text
        if (!text.empty() && !query(board, rule, childRecord))
            save(board, rule, DBRecord {LABEL_NONE}, OverwriteRule::Always);
    }

    // Find the smallest canonical pos considering symmetries to set board text
    TransformType parentTrans;
    DBKey         parentKey    = constructDBKey(board, rule, &parentTrans);
    Pos           canonicalPos = CanonicalPosMapper(parentKey, board.size(), parentTrans)(pos);

    record.setBoardText(canonicalPos, text);
    save(board, rule, record, OverwriteRule::Always);
}

bool DBClient::save(const Board &board, Rule rule, const DBRecord &record, OverwriteRule owRule)
{
    DBKey   dbKey;
    HashKey hashKey   = board.zobristKey();
    bool    overwrite = owRule == OverwriteRule::Always
                     || owRule != OverwriteRule::Disabled && !(mask & RECORD_MASK_LVDB);

    // Try find this database entry in dbCache
    if (auto entryCache = dbCache.get(hashKey); entryCache) {
        if (overwrite || checkOverwrite(entryCache->record, record, owRule)) {
            entryCache->record = record;
            entryCache->dirty  = true;

            dbRecordCache[hashKey] = std::make_pair(hashKey, record);
            return true;
        }
        else
            return false;
    }

    // Skip record fetch if we already know that we are going to overwrite it
    if (overwrite) {
        dbKey = constructDBKey(board, rule);
    }
    // Get record from dbRecordCache or dbStorage, as we need to check overwrite condition
    else {
        auto &[cachedHashKey, cachedRecord] = dbRecordCache[hashKey];
        if (cachedHashKey == hashKey) {
            overwrite = checkOverwrite(cachedRecord, record, owRule);
            if (overwrite)
                dbKey = constructDBKey(board, rule);
        }
        else {
            dbKey = constructDBKey(board, rule);
            DBRecord oldRecord;

            // Query record from dbStorage first
            if (storage.get(dbKey, oldRecord))
                overwrite = checkOverwrite(oldRecord, record, owRule);
            else  // Always overwrite if record does not exist
                overwrite = true;
        }
    }

    if (overwrite) {
        dbCache.put(hashKey, EntryCache {dbKey, record, true}, evictedEntryCollector());
        dbRecordCache[hashKey] = std::make_pair(hashKey, record);
        return true;
    }
    else
        return false;
}

void DBClient::del(const Board &board, Rule rule)
{
    HashKey hashKey = board.zobristKey();

    // First remove record cache from dbRecordCache if exists
    auto &[cachedHashKey, cachedRecord] = dbRecordCache[hashKey];
    if (cachedHashKey == hashKey)
        cachedHashKey = DBRecordCache::NullKey;

    // Iterate all parent keys of the deleted key, to remove its board text
    auto deleteParentBoardText = [&](const DBKey    &parentKey,
                                     const StonePos &stonePos,
                                     TransformType   parentTransform) -> bool {
        DBRecord parentRecord;
        if (storage.get(parentKey, parentRecord, RECORD_MASK_TEXT)) {
            Pos pos          = Pos {stonePos.x, stonePos.y};
            Pos canonicalPos = CanonicalPosMapper(parentKey, board.size(), parentTransform)(pos);

            // Delete parent record's board text if found
            if (!parentRecord.boardText(canonicalPos).empty()) {
                parentRecord.setBoardText(canonicalPos, {});
                storage.set(parentKey, parentRecord, RECORD_MASK_TEXT);
            }
        }
        return false;
    };

    // Try find this database entry in dbCache
    if (auto entryCache = dbCache.get(hashKey); entryCache) {
        storage.del(entryCache->key);
        iterateParentKeys(storage, entryCache->key, deleteParentBoardText);
        dbCache.remove(hashKey);
    }
    else {
        DBKey dbKey = constructDBKey(board, rule);
        storage.del(dbKey);
        iterateParentKeys(storage, dbKey, deleteParentBoardText);
    }
}

void DBClient::delChildren(const Board                       &board,
                           Rule                               rule,
                           std::function<DelType(DBRecord &)> deleteFilter)
{
    sync();  // clear all cached records first

    DeleteChildrenContext ctx;
    parallelWalkOnBoardClones(board, [this, rule, &deleteFilter, &ctx](Board &board, int id) {
        recursiveDeleteChildren<true>(ctx, storage, board, rule, deleteFilter, id, 0);
    });

    DBRecord parentRecord;
    if (query(board, rule, parentRecord)) {
        parentRecord.clearAllBoardText();
        save(board, rule, parentRecord, OverwriteRule::Always);
    }
}

void DBClient::sync(bool clearCache)
{
    for (auto &[hashKey, entryCache] : dbCache) {
        writeBackIfDirty(entryCache);
        entryCache.dirty = false;
    }

    if (clearCache) {
        // Clear all cache as records in dbStorage might be newer
        dbCache.clear();
        dbRecordCache.clear();
    }
}

}  // namespace Database
