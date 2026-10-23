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

#include "datawriter.h"

#include "../command/argutils.h"
#include "../core/compressor.h"
#include "../core/iohelper.h"
#include "dataformat.h"

#include <cassert>
#include <fstream>
#include <stdexcept>

namespace Tuning {

void DataWriter::writeGame(const GameEntry &gameEntry)
{
    writeEntriesInGame(gameEntry);
}

void DataWriter::writeEntriesInGame(const GameEntry                       &gameEntry,
                                    std::function<bool(const DataEntry &)> filter)
{
    Color     startSide = gameEntry.initPosition.size() % 2 == 0 ? BLACK : WHITE;
    DataEntry dataEntry {
        gameEntry.initPosition,
        gameEntry.boardsize,
        gameEntry.rule,
        // GameEntry::result is white pov; the entry wants it from its side to move.
        startSide == WHITE ? gameEntry.result : flipResult(gameEntry.result),
    };

    for (auto &moveData : gameEntry.moveSequence) {
        dataEntry.move    = moveData.move;
        dataEntry.eval    = moveData.eval;
        dataEntry.payload = moveData.payload;

        if (!filter || filter(dataEntry))
            writeEntry(dataEntry);

        // Every move (a pass included) yields the turn, so the result pov flips.
        dataEntry.position.push_back(moveData.move);
        dataEntry.result = flipResult(dataEntry.result);
    }
}

// ==============================================

class PlainTextDataWriter::DataStream
{
public:
    DataStream(std::ofstream ofs) : file(std::move(ofs)) {}
    std::ostream &getStream() { return file; }

private:
    std::ofstream file;
};

PlainTextDataWriter::PlainTextDataWriter(std::string filename)
{
    std::ofstream file(filename);
    if (!file.is_open())
        throw std::runtime_error("can not open output file: " + filename);

    dataStream = std::make_unique<DataStream>(std::move(file));
}

PlainTextDataWriter::~PlainTextDataWriter() {}

void PlainTextDataWriter::writeEntry(const DataEntry &entry)
{
    std::ostream &dst = dataStream->getStream();
    dst << int(entry.boardsize) << ',' << entry.rule << ',' << MovesText {entry.position, false}
        << ',' << int(entry.result) << ',' << entry.move;
    if (entry.eval != VALUE_NONE)
        dst << '(' << entry.eval << ')';

    if (const ExtraPVArray *pvs = extraPVs(entry.payload)) {
        for (const PVMove &pv : *pvs) {
            dst << '|' << pv.move;
            if (pv.eval != VALUE_NONE)
                dst << '(' << pv.eval << ')';
        }
    }

    dst << '\n';
}

// ==============================================

class SimpleBinaryDataWriter::DataStream
{
public:
    DataStream(std::ofstream ofs, bool compress)
        : file(std::move(ofs))
        , compressor(file, compress ? Compressor::Type::LZ4_DEFAULT : Compressor::Type::NO_COMPRESS)
        , ostream(compressor.openOutputStream())
    {
        if (!ostream)
            throw std::runtime_error("failed to open output stream");
    }
    std::ostream &getStream() { return *ostream; }

private:
    std::ofstream file;
    Compressor    compressor;
    std::ostream *ostream;
};

SimpleBinaryDataWriter::SimpleBinaryDataWriter(std::string filename, bool compress)
{
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("can not open output file: " + filename);

    dataStream = std::make_unique<DataStream>(std::move(file), compress);
}

SimpleBinaryDataWriter::~SimpleBinaryDataWriter() {}

void SimpleBinaryDataWriter::writeEntry(const DataEntry &entry)
{
    writeBinEntry(dataStream->getStream(), entry);
}

// ==============================================

class PackedBinaryDataWriter::DataStream
{
public:
    DataStream(std::ofstream ofs, bool compress)
        : file(std::move(ofs))
        , compressor(file, compress ? Compressor::Type::LZ4_DEFAULT : Compressor::Type::NO_COMPRESS)
        , ostream(compressor.openOutputStream())
    {
        if (!ostream)
            throw std::runtime_error("failed to open output stream");
    }

    ~DataStream()
    {
        // Flush previous game entry if any
        flushGameEntry(getStream());
    }

    std::ostream &getStream() { return *ostream; }

    void addDataEntry(const DataEntry &dataEntry)
    {
        warnIfPolicyDropped(dataEntry.payload);

        // Setup game entry for the first data entry
        if (gameEntry.moveSequence.empty() || !checkDataEntryMatched(dataEntry)) {
            // Flush previous game entry if any
            flushGameEntry(getStream());

            // Setup temp state. Position parity gives the side to move even when the
            // position contains passes, since a pass yields the turn like any move.
            curResult     = dataEntry.result;
            curSideToMove = dataEntry.sideToMove();

            // Setup new game entry
            gameEntry.boardsize = dataEntry.boardsize;
            gameEntry.rule      = dataEntry.rule;
            gameEntry.result =
                curSideToMove == WHITE ? dataEntry.result : flipResult(dataEntry.result);
            gameEntry.initPosition = {dataEntry.position.begin(), dataEntry.position.end()};
        }

        // Add the move, its eval and a copy of its optional payload
        gameEntry.moveSequence.push_back({dataEntry.move, dataEntry.eval, dataEntry.payload});

        // Every move yields the turn to the opponent - a pass too (it changes the
        // side to move without placing a stone). The previous non-flipping pass
        // handling disagreed with the readers on both sides of the pipeline and
        // made checkDataEntryMatched split games at every pass.
        curResult     = flipResult(curResult);
        curSideToMove = ~curSideToMove;
    }

    void addGameEntry(const GameEntry &gameEntry)
    {
        // Flush previous game entry if any
        flushGameEntry(getStream());

        // Write game entry
        this->gameEntry.boardsize    = gameEntry.boardsize;
        this->gameEntry.rule         = gameEntry.rule;
        this->gameEntry.result       = gameEntry.result;
        this->gameEntry.initPosition = gameEntry.initPosition;
        for (auto &m : gameEntry.moveSequence) {
            warnIfPolicyDropped(m.payload);
            this->gameEntry.moveSequence.push_back({m.move, m.eval, m.payload});
        }

        // Flush current game entry as it is completed
        flushGameEntry(getStream());
    }

private:
    std::ofstream file;
    Compressor    compressor;
    std::ostream *ostream;

    GameEntry gameEntry;      // GameEntry represents a full game in the dataset.
    Result    curResult;      // current result of the game
    Color     curSideToMove;  // current side to move
    bool      warnedPolicyDrop = false;

    /// The .binpack wire can only store extra multi-pv moves; dense policy
    /// arrays are silently dropped by writePackedGame. Tell the user once
    /// instead of losing their training targets without a trace.
    void warnIfPolicyDropped(const MovePayload &payload)
    {
        if (warnedPolicyDrop || !hasPayload(payload) || extraPVs(payload))
            return;
        MESSAGEL("Warning: the packed binary format cannot store dense policy targets; "
                 "they are dropped (only multi-pv moves are kept). Use the numpy output "
                 "format to keep them.");
        warnedPolicyDrop = true;
    }

    bool checkDataEntryMatched(const DataEntry &dataEntry) const
    {
        if (gameEntry.boardsize != dataEntry.boardsize)
            return false;
        if (gameEntry.rule != dataEntry.rule)
            return false;
        if (curResult != dataEntry.result)
            return false;
        if (curSideToMove != dataEntry.sideToMove())
            return false;
        // A chained entry's position must be exactly the moves recorded so far
        // (passes included on both sides). This also guards the prefix walk below
        // against indexing past the end of a shorter position.
        if (dataEntry.position.size()
            != gameEntry.initPosition.size() + gameEntry.moveSequence.size())
            return false;

        size_t i = 0;
        for (; i < gameEntry.initPosition.size(); i++) {
            if (gameEntry.initPosition[i] != dataEntry.position[i])
                return false;
        }
        for (auto &moveData : gameEntry.moveSequence) {
            if (moveData.move != dataEntry.position[i])
                return false;
            i++;
        }

        return true;
    }

    void flushGameEntry(std::ostream &os)
    {
        if (gameEntry.moveSequence.empty())
            return;

        writePackedGame(os, gameEntry);

        // Mark as flushed, and reset the game entry
        gameEntry.initPosition.clear();
        gameEntry.moveSequence.clear();
    }
};

PackedBinaryDataWriter::PackedBinaryDataWriter(std::string filename, bool compress)
{
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("can not open output file: " + filename);

    dataStream = std::make_unique<DataStream>(std::move(file), compress);
}

PackedBinaryDataWriter::~PackedBinaryDataWriter() {}

void PackedBinaryDataWriter::writeEntry(const DataEntry &entry)
{
    dataStream->addDataEntry(entry);
}

void PackedBinaryDataWriter::writeGame(const GameEntry &gameEntry)
{
    dataStream->addGameEntry(gameEntry);
}

// ==============================================
// NumpyDataWriter lives in numpywriter.cpp (batching + npz serialization),
// with the training-plane layout itself in featureencoder.{h,cpp}.

std::unique_ptr<DataWriter> makeDataWriter(Command::DataWriterType dataWriterType,
                                           const std::string      &outputPath)
{
    using Command::DataWriterType;
    switch (dataWriterType) {
    case DataWriterType::PlainText: return std::make_unique<PlainTextDataWriter>(outputPath);
    case DataWriterType::SimpleBinary:
        return std::make_unique<SimpleBinaryDataWriter>(outputPath, false);
    case DataWriterType::SimpleBinaryLZ4:
        return std::make_unique<SimpleBinaryDataWriter>(outputPath, true);
    case DataWriterType::PackedBinary:
        return std::make_unique<PackedBinaryDataWriter>(outputPath, false);
    case DataWriterType::PackedBinaryLZ4:
        return std::make_unique<PackedBinaryDataWriter>(outputPath, true);
    default: assert(false && "Numpy writer is constructed at its call sites"); return nullptr;
    }
}

}  // namespace Tuning
