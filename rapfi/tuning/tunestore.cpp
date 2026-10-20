/*
 *  Rapfi, a Gomoku/Renju playing engine supporting piskvork protocol.
 *  Copyright (C) 2022  Rapfi developers
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "tunestore.h"

#include "tunedigest.h"
#include "tuneshard.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <system_error>
#include <unordered_set>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace Tuning {
namespace {

    constexpr uint32_t    ManifestVersion         = 3;
    constexpr uint32_t    GenerationMarkerVersion = 1;
    constexpr const char *GenerationMarkerName    = ".rftune-generation";
    constexpr size_t      MaxManifestBytes        = 8 * 1024 * 1024;

    class ReadOnlyMemoryBuffer : public std::streambuf
    {
    public:
        explicit ReadOnlyMemoryBuffer(std::string &contents)
        {
            if (contents.empty())
                setg(nullptr, nullptr, nullptr);
            else {
                char *begin = &contents[0];
                setg(begin, begin, begin + contents.size());
            }
        }
    };

    std::filesystem::path makeGenerationDirectory(const std::filesystem::path &root,
                                                  const std::string           &label)
    {
        static std::atomic<uint64_t> sequence {0};
        uint64_t                     timestamp = static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());

        for (uint64_t attempt = 0; attempt < 1024; attempt++) {
            std::ostringstream name;
            name << label << '-' << std::hex << timestamp << '-'
                 << (sequence.fetch_add(1, std::memory_order_relaxed) + attempt);
            std::filesystem::path candidate = root / name.str();
            std::error_code       error;
            if (std::filesystem::create_directories(candidate, error))
                return candidate;
            if (error && error != std::errc::file_exists)
                throw std::runtime_error("unable to create prepared corpus directory: "
                                         + error.message());
        }
        throw std::runtime_error("unable to allocate a unique prepared corpus generation");
    }

    std::string shardFilename(size_t shardIndex)
    {
        std::ostringstream filename;
        filename << "shard-" << std::setfill('0') << std::setw(8) << shardIndex << ".rftune";
        return filename.str();
    }

    bool isSafeGenerationName(const std::string &name, const std::string &label)
    {
        std::filesystem::path path(name);
        std::string           prefix = label + '-';
        if (name.empty() || path != path.filename() || name.compare(0, prefix.size(), prefix) != 0)
            return false;
        size_t separator = name.find('-', prefix.size());
        if (separator == std::string::npos || separator == prefix.size()
            || separator + 1 == name.size() || name.find('-', separator + 1) != std::string::npos)
            return false;
        auto isHexRange = [&](size_t begin, size_t end) {
            return std::all_of(name.begin() + begin, name.begin() + end, [](char digit) {
                return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
            });
        };
        return isHexRange(prefix.size(), separator) && isHexRange(separator + 1, name.size());
    }

    bool isDirectOwnedDirectory(const std::filesystem::path &root,
                                const std::filesystem::path &directory)
    {
        std::error_code error;
        auto            status = std::filesystem::symlink_status(directory, error);
        if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
            return false;
        std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(root, error);
        if (error)
            return false;
        std::filesystem::path canonicalDirectory =
            std::filesystem::weakly_canonical(directory, error);
        return !error && canonicalDirectory.parent_path() == canonicalRoot;
    }

    size_t checkedSize(uint64_t value, const char *field)
    {
        if (value > std::numeric_limits<size_t>::max())
            throw std::runtime_error(std::string("prepared manifest ") + field + " exceeds size_t");
        return static_cast<size_t>(value);
    }

    void expectToken(std::istream &input, const char *expected)
    {
        std::string token;
        if (!(input >> token) || token != expected)
            throw std::runtime_error(std::string("prepared manifest expected '") + expected + "'");
    }

    void writeGenerationMarker(const std::filesystem::path &directory,
                               const std::string           &label,
                               const std::string           &fingerprint)
    {
        std::ofstream output(directory / GenerationMarkerName, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("unable to create prepared generation marker");
        output << "RFTUNE_GENERATION " << GenerationMarkerVersion << '\n';
        output << "label " << std::quoted(label) << '\n';
        output << "generation " << std::quoted(directory.filename().string()) << '\n';
        output << "fingerprint " << fingerprint << '\n';
        output << "end\n";
        output.close();
        if (!output)
            throw std::runtime_error("failed to publish prepared generation marker");
    }

    bool hasMatchingGenerationMarker(const std::filesystem::path &root,
                                     const std::filesystem::path &directory,
                                     const std::string           &label,
                                     const std::string           &fingerprint)
    {
        if (!isSafeGenerationName(directory.filename().string(), label)
            || !isDirectOwnedDirectory(root, directory))
            return false;
        std::ifstream input(directory / GenerationMarkerName, std::ios::binary);
        if (!input)
            return false;
        try {
            expectToken(input, "RFTUNE_GENERATION");
            uint32_t version;
            if (!(input >> version) || version != GenerationMarkerVersion)
                return false;
            expectToken(input, "label");
            std::string actualLabel;
            if (!(input >> std::quoted(actualLabel)) || actualLabel != label)
                return false;
            expectToken(input, "generation");
            std::string generation;
            if (!(input >> std::quoted(generation)) || generation != directory.filename().string())
                return false;
            expectToken(input, "fingerprint");
            std::string actualFingerprint;
            if (!(input >> actualFingerprint) || actualFingerprint != fingerprint)
                return false;
            expectToken(input, "end");
            std::string trailing;
            return !(input >> trailing);
        }
        catch (...) {
            return false;
        }
    }

    void validateCurrentSource(const PreparedSourceInfo &expected)
    {
        std::filesystem::path configured = std::filesystem::u8path(expected.configuredPath);
        std::error_code       error;
        std::filesystem::path path = std::filesystem::weakly_canonical(configured, error);
        if (error || path.generic_u8string() != expected.canonicalPath)
            throw std::runtime_error(
                "tuning dataset alias target changed before cache publication: "
                + configured.string());
        uintmax_t size = std::filesystem::file_size(path, error);
        if (error || size != expected.size)
            throw std::runtime_error("tuning dataset source size changed before cache publication: "
                                     + path.string());
        auto modified = std::filesystem::last_write_time(path, error);
        if (error)
            throw std::runtime_error("unable to inspect tuning dataset source before publication: "
                                     + path.string());
        std::string digest = sha256Hex(sha256File(path));
        error.clear();
        uintmax_t verifiedSize = std::filesystem::file_size(path, error);
        if (error)
            throw std::runtime_error("unable to recheck tuning dataset source before publication: "
                                     + path.string());
        auto verifiedModified = std::filesystem::last_write_time(path, error);
        if (error || verifiedSize != size || verifiedModified != modified)
            throw std::runtime_error("tuning dataset source changed during cache publication: "
                                     + path.string());
        if (digest != expected.sha256)
            throw std::runtime_error(
                "tuning dataset source content changed before cache publication: " + path.string());
    }

    std::string readManifest(const std::filesystem::path &path)
    {
        std::error_code error;
        uintmax_t       fileSize = std::filesystem::file_size(path, error);
        if (error)
            throw std::runtime_error("unable to inspect prepared manifest: " + error.message());
        if (fileSize > MaxManifestBytes)
            throw std::runtime_error("prepared manifest exceeds its memory allowance");

        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::runtime_error("unable to open prepared manifest");
        std::string contents(static_cast<size_t>(fileSize), '\0');
        if (!contents.empty())
            input.read(&contents[0], static_cast<std::streamsize>(contents.size()));
        if (input.gcount() != static_cast<std::streamsize>(contents.size())
            || input.peek() != std::ios::traits_type::eof())
            throw std::runtime_error("prepared manifest changed while being read");
        return contents;
    }

    void atomicReplace(const std::filesystem::path &source, const std::filesystem::path &target)
    {
#ifdef _WIN32
        DWORD lastError = ERROR_SUCCESS;
        for (int attempt = 0; attempt < 1000; attempt++) {
            if (MoveFileExW(source.c_str(),
                            target.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return;
            lastError = GetLastError();
            if (lastError != ERROR_ACCESS_DENIED && lastError != ERROR_SHARING_VIOLATION)
                break;
            Sleep(5);
        }
        std::error_code error(static_cast<int>(lastError), std::system_category());
        throw std::runtime_error("failed to publish prepared manifest: " + error.message());
#else
        std::error_code error;
        std::filesystem::rename(source, target, error);
        if (error)
            throw std::runtime_error("failed to publish prepared manifest: " + error.message());
#endif
    }

}  // namespace

FileBackedCorpus::FileBackedCorpus(const std::filesystem::path &root,
                                   const char                  *label,
                                   size_t                       maxShardBytes,
                                   PreparedCacheKey             cacheKey,
                                   bool                         rebuild)
    : root_(root)
    , label_(label)
    , manifestPath_(root_ / (label_ + ".manifest"))
    , maxShardBytes_(maxShardBytes)
    , cacheKey_(std::move(cacheKey))
{
    if (cacheKey_.fingerprint.size() != 64)
        throw std::invalid_argument("prepared cache fingerprint must contain 64 hex digits");
    std::filesystem::create_directories(root_);

    if (!rebuild && tryReuse())
        return;
    if (rebuild)
        cacheStatus_ = "forced prepared-cache rebuild";
    directory_ = makeGenerationDirectory(root_, label_);
    try {
        writeGenerationMarker(directory_, label_, cacheKey_.fingerprint);
    }
    catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
        directory_.clear();
        throw;
    }
}

FileBackedCorpus::~FileBackedCorpus()
{
    if (!reused_ && !published_ && !directory_.empty()
        && hasMatchingGenerationMarker(root_, directory_, label_, cacheKey_.fingerprint)) {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }
}

bool FileBackedCorpus::tryReuse()
{
    std::ifstream manifestProbe(manifestPath_, std::ios::binary);
    if (!manifestProbe) {
        cacheStatus_ = "prepared-cache miss: manifest not found";
        return false;
    }
    manifestProbe.close();

    try {
        std::string          manifestContents = readManifest(manifestPath_);
        ReadOnlyMemoryBuffer manifestBuffer(manifestContents);
        std::istream         input(&manifestBuffer);
        expectToken(input, "RFTUNE_MANIFEST");
        uint32_t version;
        if (!(input >> version) || version != ManifestVersion)
            throw std::runtime_error("prepared manifest version is unsupported");
        expectToken(input, "fingerprint");
        std::string fingerprint;
        if (!(input >> fingerprint))
            throw std::runtime_error("prepared manifest fingerprint is missing");
        expectToken(input, "generation");
        std::string generation;
        if (!(input >> std::quoted(generation)) || !isSafeGenerationName(generation, label_))
            throw std::runtime_error("prepared manifest generation is invalid");
        if (fingerprint != cacheKey_.fingerprint) {
            cacheStatus_ = "prepared-cache miss: strict fingerprint changed";
            return false;
        }
        std::filesystem::path generationDirectory = root_ / generation;
        if (!hasMatchingGenerationMarker(root_, generationDirectory, label_, cacheKey_.fingerprint))
            throw std::runtime_error("prepared generation ownership marker is invalid");

        expectToken(input, "sources");
        uint64_t sourceCount;
        if (!(input >> sourceCount) || sourceCount != cacheKey_.sources.size())
            throw std::runtime_error("prepared manifest source count is invalid");
        for (const PreparedSourceInfo &expected : cacheKey_.sources) {
            expectToken(input, "source");
            PreparedSourceInfo actual;
            uint64_t           size;
            if (!(input >> std::quoted(actual.configuredPath) >> std::quoted(actual.canonicalPath)
                  >> size >> actual.modifiedTicks >> actual.sha256))
                throw std::runtime_error("prepared manifest source record is incomplete");
            actual.size = size;
            if (actual.configuredPath != expected.configuredPath
                || actual.canonicalPath != expected.canonicalPath || actual.size != expected.size
                || actual.sha256 != expected.sha256)
                throw std::runtime_error("prepared manifest source record does not match");
        }

        expectToken(input, "shards");
        uint64_t declaredShardCount;
        if (!(input >> declaredShardCount))
            throw std::runtime_error("prepared manifest shard count is missing");
        expectToken(input, "samples");
        uint64_t declaredSamples;
        if (!(input >> declaredSamples))
            throw std::runtime_error("prepared manifest sample count is missing");
        expectToken(input, "disk_bytes");
        uint64_t declaredDiskBytes;
        if (!(input >> declaredDiskBytes))
            throw std::runtime_error("prepared manifest disk size is missing");
        expectToken(input, "max_shard_storage");
        uint64_t declaredMaxStorage;
        if (!(input >> declaredMaxStorage))
            throw std::runtime_error("prepared manifest maximum shard size is missing");

        directory_                 = std::move(generationDirectory);
        size_t validatedShards     = checkedSize(declaredShardCount, "shard count");
        size_t validatedSamples    = 0;
        size_t validatedDiskBytes  = 0;
        size_t validatedMaxStorage = 0;
        for (size_t shard = 0; shard < validatedShards; shard++) {
            expectToken(input, "shard");
            std::string filename, expectedDigest;
            uint64_t    ordinal, declaredShardSamples, declaredFileBytes, declaredStorageBytes;
            if (!(input >> std::quoted(filename) >> ordinal >> declaredShardSamples
                  >> declaredFileBytes >> declaredStorageBytes >> expectedDigest)
                || filename != shardFilename(shard) || ordinal != shard)
                throw std::runtime_error("prepared manifest shard order is invalid");
            std::filesystem::path path     = directory_ / filename;
            uintmax_t             fileSize = std::filesystem::file_size(path);
            if (fileSize > std::numeric_limits<size_t>::max())
                throw std::runtime_error("prepared shard file size exceeds size_t");
            if (fileSize != declaredFileBytes)
                throw std::runtime_error("prepared manifest shard file size does not match");
            if (sha256Hex(sha256File(path)) != expectedDigest)
                throw std::runtime_error("prepared manifest shard content digest does not match");

            PreparedCorpus corpus =
                readPreparedShard(path, true, maxShardBytes_, cacheKey_.fingerprint, ordinal);
            if (corpus.size() != checkedSize(declaredShardSamples, "shard sample count")
                || corpus.storageBytes() != checkedSize(declaredStorageBytes, "shard storage size"))
                throw std::runtime_error("prepared manifest shard metadata does not match");
            if (corpus.size() > std::numeric_limits<size_t>::max() - validatedSamples
                || fileSize > std::numeric_limits<size_t>::max() - validatedDiskBytes)
                throw std::runtime_error("prepared cache aggregate size overflows");
            validatedSamples += corpus.size();
            validatedDiskBytes += static_cast<size_t>(fileSize);
            validatedMaxStorage = std::max(validatedMaxStorage, corpus.storageBytes());
        }
        expectToken(input, "end");
        std::string trailing;
        if (input >> trailing)
            throw std::runtime_error("prepared manifest contains trailing fields");

        if (validatedSamples != checkedSize(declaredSamples, "sample count")
            || validatedDiskBytes != checkedSize(declaredDiskBytes, "disk size")
            || validatedMaxStorage != checkedSize(declaredMaxStorage, "maximum shard size"))
            throw std::runtime_error("prepared manifest aggregate metadata does not match shards");

        shardCount_           = validatedShards;
        sampleCount_          = validatedSamples;
        diskBytes_            = validatedDiskBytes;
        maxShardStorageBytes_ = validatedMaxStorage;
        reused_               = true;
        published_            = true;
        cacheStatus_          = "reused strict prepared cache";
        return true;
    }
    catch (const std::exception &error) {
        directory_.clear();
        cacheStatus_ = std::string("prepared-cache rebuild: ") + error.what();
        return false;
    }
}

std::filesystem::path FileBackedCorpus::shardPath(size_t shardIndex) const
{
    return directory_ / shardFilename(shardIndex);
}

void FileBackedCorpus::append(PreparedCorpus &&corpus)
{
    if (reused_ || published_)
        throw std::logic_error("cannot append to a published prepared corpus");
    if (corpus.empty())
        return;
    if (sampleCount_ > std::numeric_limits<size_t>::max() - corpus.size())
        throw std::length_error("prepared corpus sample count overflows size_t");

    size_t samples      = corpus.size();
    size_t storageBytes = corpus.storageBytes();
    if (storageBytes > maxShardBytes_)
        throw std::runtime_error("prepared shard exceeds its allocation credit");
    std::filesystem::path path = shardPath(shardCount_);
    writePreparedShard(path, corpus, cacheKey_.fingerprint, shardCount_);

    uintmax_t fileSize = std::filesystem::file_size(path);
    if (fileSize > std::numeric_limits<size_t>::max())
        throw std::length_error("prepared shard file size exceeds size_t");
    size_t fileBytes = static_cast<size_t>(fileSize);
    if (diskBytes_ > std::numeric_limits<size_t>::max() - fileBytes)
        throw std::length_error("prepared corpus disk byte count overflows size_t");

    corpus = PreparedCorpus {};
    PreparedCorpus verified =
        readPreparedShard(path, true, maxShardBytes_, cacheKey_.fingerprint, shardCount_);
    if (verified.size() != samples || verified.storageBytes() != storageBytes)
        throw std::runtime_error("prepared corpus shard metadata mismatch");

    shardCount_++;
    sampleCount_ += samples;
    diskBytes_ += fileBytes;
    maxShardStorageBytes_ = std::max(maxShardStorageBytes_, storageBytes);
}

void FileBackedCorpus::publish()
{
    if (reused_ || published_)
        return;

    std::filesystem::path tempPath = directory_ / "manifest.tmp";
    try {
        if (std::filesystem::exists(tempPath))
            throw std::runtime_error("prepared manifest temporary target already exists");
        std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("unable to create prepared manifest: " + tempPath.string());
        output << "RFTUNE_MANIFEST " << ManifestVersion << '\n';
        output << "fingerprint " << cacheKey_.fingerprint << '\n';
        output << "generation " << std::quoted(directory_.filename().string()) << '\n';
        output << "sources " << cacheKey_.sources.size() << '\n';
        for (const PreparedSourceInfo &source : cacheKey_.sources)
            output << "source " << std::quoted(source.configuredPath) << ' '
                   << std::quoted(source.canonicalPath) << ' ' << source.size << ' '
                   << source.modifiedTicks << ' ' << source.sha256 << '\n';
        output << "shards " << shardCount_ << '\n';
        output << "samples " << sampleCount_ << '\n';
        output << "disk_bytes " << diskBytes_ << '\n';
        output << "max_shard_storage " << maxShardStorageBytes_ << '\n';
        size_t publishedSamples = 0, publishedDiskBytes = 0, publishedMaxStorage = 0;
        for (size_t shard = 0; shard < shardCount_; shard++) {
            std::filesystem::path path = shardPath(shard);
            PreparedCorpus        corpus =
                readPreparedShard(path, true, maxShardBytes_, cacheKey_.fingerprint, shard);
            uintmax_t fileSize = std::filesystem::file_size(path);
            if (fileSize > std::numeric_limits<size_t>::max()
                || corpus.size() > std::numeric_limits<size_t>::max() - publishedSamples
                || fileSize > std::numeric_limits<size_t>::max() - publishedDiskBytes)
                throw std::runtime_error("prepared shard metadata overflows during publication");
            publishedSamples += corpus.size();
            publishedDiskBytes += static_cast<size_t>(fileSize);
            publishedMaxStorage = std::max(publishedMaxStorage, corpus.storageBytes());
            output << "shard " << std::quoted(shardFilename(shard)) << ' ' << shard << ' '
                   << corpus.size() << ' ' << fileSize << ' ' << corpus.storageBytes() << ' '
                   << sha256Hex(sha256File(path)) << '\n';
        }
        if (publishedSamples != sampleCount_ || publishedDiskBytes != diskBytes_
            || publishedMaxStorage != maxShardStorageBytes_)
            throw std::runtime_error("prepared shard aggregates changed before publication");

        std::unordered_set<std::string> validatedSources;
        for (const PreparedSourceInfo &source : cacheKey_.sources)
            if (validatedSources.emplace(source.configuredPath).second)
                validateCurrentSource(source);

        output << "end\n";
        output.flush();
        if (!output)
            throw std::runtime_error("failed to write prepared manifest: " + tempPath.string());
        output.close();
        if (!output)
            throw std::runtime_error("failed to close prepared manifest: " + tempPath.string());
        std::error_code manifestSizeError;
        uintmax_t       manifestSize = std::filesystem::file_size(tempPath, manifestSizeError);
        if (manifestSizeError)
            throw std::runtime_error("unable to inspect completed prepared manifest: "
                                     + manifestSizeError.message());
        if (manifestSize > MaxManifestBytes)
            throw std::runtime_error("prepared manifest exceeds its reusable memory allowance; "
                                     "increase --shard-size-mb or reduce the source-file count");

        atomicReplace(tempPath, manifestPath_);
        published_ = true;
    }
    catch (...) {
        std::error_code ignored;
        std::filesystem::remove(tempPath, ignored);
        throw;
    }
}

PreparedCorpus FileBackedCorpus::load(size_t shardIndex) const
{
    if (shardIndex >= shardCount_)
        throw std::out_of_range("prepared corpus shard index is out of range");
    return readPreparedShard(shardPath(shardIndex),
                             false,
                             maxShardBytes_,
                             cacheKey_.fingerprint,
                             shardIndex);
}

}  // namespace Tuning
