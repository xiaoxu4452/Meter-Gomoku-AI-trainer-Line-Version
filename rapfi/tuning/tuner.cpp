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

#include "tuner.h"

#include "../config.h"
#include "../core/iohelper.h"
#include "../core/random.h"
#include "../core/time.h"
#include "../eval/eval.h"
#include "../game/board.h"
#include "dataset.h"
#include "optimizer.h"
#include "tunedigest.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <future>
#include <iomanip>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

namespace {

using Tuning::Float;
using Tuning::LossType;
using Tuning::PolicyCandidate;
using Tuning::PreparedCacheKey;
using Tuning::PreparedCorpus;
using Tuning::Sha256;
using Tuning::TuneCoeff;
using Tuning::TuneGradient;
using Tuning::TuneParam;

constexpr Float  CoeffScale = 8;
constexpr size_t MiB        = 1024 * 1024;
constexpr size_t KiB        = 1024;

// One worker owns the source record, a growing prepared fragment, and its
// reusable coefficient scratch at the same time. Four times the maximum
// serialized sample size covers vector capacity plus a reallocating old/new
// buffer pair without relying on a particular standard-library growth ratio.
constexpr size_t WorstTermsPerSample = 4 * MAX_MOVES + 1;
constexpr size_t WorstPreparedSampleBytes =
    32 + WorstTermsPerSample * sizeof(TuneCoeff) + MAX_MOVES * sizeof(PolicyCandidate);
constexpr size_t PreparedSampleCredit = 4 * WorstPreparedSampleBytes;

struct LossPair
{
    Float value  = 0;
    Float policy = 0;

    LossPair &operator+=(const LossPair &other)
    {
        value += other.value;
        policy += other.policy;
        return *this;
    }
};

size_t checkedProduct(size_t lhs, size_t rhs, const char *what)
{
    if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs)
        throw std::length_error(std::string(what) + " byte count overflows size_t");
    return lhs * rhs;
}

bool pathComponentEqual(const std::filesystem::path &lhs, const std::filesystem::path &rhs)
{
#ifdef _WIN32
    return CompareStringOrdinal(lhs.c_str(), -1, rhs.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return lhs == rhs;
#endif
}

bool isWithinDirectory(const std::filesystem::path &directory,
                       const std::filesystem::path &candidate)
{
    std::filesystem::path normalizedDirectory = directory.lexically_normal();
    std::filesystem::path normalizedCandidate = candidate.lexically_normal();
    auto                  directoryComponent  = normalizedDirectory.begin();
    auto                  candidateComponent  = normalizedCandidate.begin();
    for (; directoryComponent != normalizedDirectory.end();
         ++directoryComponent, ++candidateComponent) {
        if (candidateComponent == normalizedCandidate.end()
            || !pathComponentEqual(*directoryComponent, *candidateComponent))
            return false;
    }
    return true;
}

void validatePreparedCacheRoot(const std::filesystem::path &root,
                               const PreparedCacheKey      &trainKey,
                               const PreparedCacheKey      *validationKey)
{
    std::error_code       error;
    std::filesystem::path absoluteRoot = std::filesystem::absolute(root, error).lexically_normal();
    if (error)
        throw std::runtime_error("unable to resolve prepared-cache root: " + root.string());
    std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(absoluteRoot, error);
    if (error)
        throw std::runtime_error("unable to canonicalize prepared-cache root: "
                                 + absoluteRoot.string());

    auto validateKey = [&](const PreparedCacheKey &key) {
        for (const Tuning::PreparedSourceInfo &source : key.sources) {
            std::filesystem::path configured =
                std::filesystem::u8path(source.configuredPath).lexically_normal();
            std::filesystem::path aliasParent =
                std::filesystem::weakly_canonical(configured.parent_path(), error);
            if (error)
                throw std::runtime_error("unable to canonicalize tuning dataset parent: "
                                         + configured.string());
            std::filesystem::path aliasLocation =
                (aliasParent / configured.filename()).lexically_normal();
            std::filesystem::path canonicalTarget =
                std::filesystem::u8path(source.canonicalPath).lexically_normal();
            if (isWithinDirectory(canonicalRoot, aliasLocation)
                || isWithinDirectory(canonicalRoot, canonicalTarget))
                throw std::invalid_argument(
                    "tuning dataset sources must be outside the prepared-cache root: "
                    + configured.string());
        }
    };

    validateKey(trainKey);
    if (validationKey)
        validateKey(*validationKey);
}

void hashUint64(Sha256 &hasher, uint64_t value)
{
    uint8_t encoded[8];
    for (size_t i = 0; i < 8; i++) {
        encoded[i] = static_cast<uint8_t>(value);
        value >>= 8;
    }
    hasher.update(encoded, sizeof(encoded));
}

void hashString(Sha256 &hasher, const std::string &value)
{
    hashUint64(hasher, value.size());
    hasher.update(value.data(), value.size());
}

void hashDouble(Sha256 &hasher, double value)
{
    uint64_t bits;
    static_assert(sizeof(bits) == sizeof(value), "double must have a 64-bit representation");
    std::memcpy(&bits, &value, sizeof(bits));
    hashUint64(hasher, bits);
}

void hashFloat(Sha256 &hasher, float value)
{
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(value), "float must have a 32-bit representation");
    std::memcpy(&bits, &value, sizeof(bits));
    uint8_t encoded[4];
    for (size_t i = 0; i < 4; i++) {
        encoded[i] = static_cast<uint8_t>(bits);
        bits >>= 8;
    }
    hasher.update(encoded, sizeof(encoded));
}

uint64_t domainSeed(uint64_t seed, uint64_t domain)
{
    PRNG mixer(seed ^ domain);
    return mixer();
}

TuneParam encodeIntegerForTruncatingExport(Score score, Float scale, Float bias)
{
    TuneParam nearest      = TuneParam((Float(score) - bias) / scale);
    TuneParam candidates[] = {
        nearest,
        std::nextafter(nearest, -std::numeric_limits<TuneParam>::infinity()),
        std::nextafter(nearest, std::numeric_limits<TuneParam>::infinity()),
    };

    for (TuneParam candidate : candidates) {
        Float reconstructed = Float(candidate) * scale + bias;
        if (std::isfinite(candidate) && std::trunc(reconstructed) == Float(score))
            return candidate;
    }

    throw std::runtime_error(
        "move-score scale and bias cannot represent an integer score without drift");
}

inline bool checkEqual(Float a, Float b)
{
    return std::abs(a - b) <= Float(1.0);
}

/// sigmoid(x) = 1/(1+exp(-x))
inline Float sigmoid(Float x)
{
    return Float(1) / (Float(1) + std::exp(-x));
}

inline Float scoreToWinrate(Float score, Float invScalingFactor)
{
    return sigmoid(score * invScalingFactor);
}

inline Float lossFunction(LossType lt, Float logit, Float target)
{
    Float pred = sigmoid(logit);
    switch (lt) {
    case LossType::L1: return std::abs(target - pred);
    case LossType::L2: {
        Float dist = target - pred;
        return dist * dist;
    }
    case LossType::BCE: {
        Float loss =
            std::max(logit, Float(0)) - logit * target + std::log1p(std::exp(-std::abs(logit)));
        Float targetBias = 0;
        if (target > 0)
            targetBias += target * std::log(target);
        if (target < 1)
            targetBias += (1 - target) * std::log1p(-target);
        return (loss + targetBias) / 2;
    }
    default: return Float(0);
    }
}

inline Float lossFunctionLogitGrad(LossType lt, Float logit, Float target)
{
    Float pred = sigmoid(logit);
    Float diff = pred - target;
    switch (lt) {
    case LossType::L1: return ((Float(0) < diff) - (diff < Float(0))) * pred * (Float(1) - pred);
    case LossType::L2: return Float(2) * diff * pred * (Float(1) - pred);
    case LossType::BCE: return diff / 2;
    default: return Float(0);
    }
}

/// Collect coefficient from eval info
template <typename Collector>
void collectEvalCoeffs(Rule r, const Evaluation::EvalInfo &evalInfo, Collector collect)
{
    Color self = evalInfo.self, oppo = ~self;

    // Collect pattern code coefficient
    for (size_t pcode = 0; pcode < PCODE_NB; pcode++) {
        int coeff[2][SIDE_NB] = {{evalInfo.plyBack[0].pcodeCount[BLACK][pcode],
                                  evalInfo.plyBack[0].pcodeCount[WHITE][pcode]},
                                 {evalInfo.plyBack[1].pcodeCount[BLACK][pcode],
                                  evalInfo.plyBack[1].pcodeCount[WHITE][pcode]}};

        // Coefficient is scaled at 2x
        if (r == RENJU) {
            collect(coeff[0][self] + coeff[1][self], 2, &Evaluation::EVALS[r + self][pcode]);
            collect(-coeff[0][oppo] - coeff[1][oppo], 2, &Evaluation::EVALS[r + oppo][pcode]);
        }
        else {
            collect(coeff[0][self] - coeff[0][oppo] + coeff[1][self] - coeff[1][oppo],
                    2,
                    &Evaluation::EVALS[r][pcode]);
        }
    }

    // Collect threat eval coefficient
    collect(1, 1, &Evaluation::EVALS_THREAT[Evaluation::tableIndex(r, self)][evalInfo.threatMask]);
}

/// Collect coefficient from board pattern codes
template <typename Collector>
void collectMoveScoreCoeffs(Rule r, const Board &board, Collector collect)
{
    Color self = board.sideToMove(), oppo = ~self;

    // Collect scores of all candidate moves
    FOR_EVERY_EMPTY_CAND_POS(&board, pos)
    {
        collect(pos,
                1,
                1,
                &Evaluation::P4SCORES[Evaluation::tableIndex(r, self)][board.pcode<BLACK>(pos)],
                &Evaluation::P4SCORES[Evaluation::tableIndex(r, oppo)][board.pcode<WHITE>(pos)]);
    }
}

template <typename T, typename UnaryOp>
T parallelIndexReduce(BS::thread_pool &pool, size_t count, T init, UnaryOp transform)
{
    if (count == 0)
        return init;

    constexpr size_t            LogicalPartitions = 64;
    size_t                      numBlocks         = std::min(count, LogicalPartitions);
    size_t                      blockSize         = (count + numBlocks - 1) / numBlocks;
    std::vector<std::future<T>> futures;
    futures.reserve(numBlocks);

    for (size_t i = 0; i < numBlocks; ++i) {
        size_t blockBegin = i * blockSize;
        size_t blockEnd   = std::min(blockBegin + blockSize, count);
        if (blockBegin == blockEnd)
            break;

        futures.emplace_back(pool.submit_task([blockBegin, blockEnd, transform]() {
            T blockResult = T {};
            for (size_t index = blockBegin; index < blockEnd; ++index)
                blockResult += transform(index);
            return blockResult;
        }));
    }

    T result = init;
    for (auto &future : futures)
        result += future.get();

    return result;
}

Float linearValue(const std::vector<TuneCoeff> &terms,
                  uint32_t                      begin,
                  uint32_t                      end,
                  const std::vector<TuneParam> &params)
{
    Float value = 0;
    for (uint32_t i = begin; i < end; i++)
        value += terms[i].coeff * params[terms[i].index];
    return value;
}

Float policyScore(const PolicyCandidate &candidate, const std::vector<TuneParam> &params)
{
    return params[candidate.indices[0]] + params[candidate.indices[1]];
}

Float computeLinearEval(const PreparedCorpus         &corpus,
                        size_t                        sample,
                        const std::vector<TuneParam> &params)
{
    return linearValue(corpus.evalTerms(),
                       corpus.evalOffsets()[sample],
                       corpus.evalOffsets()[sample + 1],
                       params)
           / CoeffScale;
}

template <bool UseTunedEval>
Float computeEvalLoss(const PreparedCorpus         &corpus,
                      size_t                        sample,
                      const std::vector<TuneParam> &params,
                      Float                         K,
                      LossType                      loss)
{
    if (corpus.evalOffsets()[sample] == corpus.evalOffsets()[sample + 1])
        return 0;

    Float eval   = UseTunedEval ? computeLinearEval(corpus, sample, params)
                                : Float(corpus.staticEvals()[sample]);
    Float result = Float(corpus.results()[sample]) * Float(0.5);
    return lossFunction(loss, eval * K, result);
}

Float computeMoveScoreLoss(const PreparedCorpus         &corpus,
                           size_t                        sample,
                           const std::vector<TuneParam> &params,
                           Float                         gamma)
{
    uint32_t begin = corpus.policyOffsets()[sample];
    uint32_t end   = corpus.policyOffsets()[sample + 1];
    uint16_t best  = corpus.bestCandidates()[sample];
    if (begin == end || best == PreparedCorpus::NoPolicyTarget)
        return 0;

    const auto &candidates = corpus.policyCandidates();
    Float       maxScore   = std::numeric_limits<Float>::lowest();
    for (uint32_t i = begin; i < end; i++)
        maxScore = std::max(maxScore, policyScore(candidates[i], params));

    Float sumExp = 0;
    for (uint32_t i = begin; i < end; i++)
        sumExp += std::exp(policyScore(candidates[i], params) - maxScore);

    Float scoreClass  = policyScore(candidates[begin + best], params) - maxScore;
    Float xClass      = std::exp(scoreClass) / sumExp;
    Float focalWeight = std::pow(Float(1) - xClass, gamma);
    return focalWeight * (-scoreClass + std::log(sumExp));
}

void computeEvalGradient(const PreparedCorpus         &corpus,
                         size_t                        sample,
                         std::vector<TuneGradient>    &grads,
                         const std::vector<TuneParam> &params,
                         Float                         K,
                         LossType                      loss)
{
    uint32_t begin = corpus.evalOffsets()[sample];
    uint32_t end   = corpus.evalOffsets()[sample + 1];
    if (begin == end)
        return;

    Float       result    = Float(corpus.results()[sample]) * Float(0.5);
    Float       logit     = computeLinearEval(corpus, sample, params) * K;
    Float       dL_dEval  = lossFunctionLogitGrad(loss, logit, result) * K;
    const auto &evalTerms = corpus.evalTerms();
    for (uint32_t i = begin; i < end; i++)
        grads[evalTerms[i].index] += evalTerms[i].coeff * dL_dEval;
}

void computeMoveScoreGradient(const PreparedCorpus         &corpus,
                              size_t                        sample,
                              std::vector<TuneGradient>    &grads,
                              const std::vector<TuneParam> &params,
                              Float                         gamma)
{
    uint32_t begin = corpus.policyOffsets()[sample];
    uint32_t end   = corpus.policyOffsets()[sample + 1];
    uint16_t best  = corpus.bestCandidates()[sample];
    if (begin == end || best == PreparedCorpus::NoPolicyTarget)
        return;

    const auto &candidates = corpus.policyCandidates();
    Float       maxScore   = std::numeric_limits<Float>::lowest();
    for (uint32_t i = begin; i < end; i++)
        maxScore = std::max(maxScore, policyScore(candidates[i], params));

    Float sumExp = 0;
    for (uint32_t i = begin; i < end; i++)
        sumExp += std::exp(policyScore(candidates[i], params) - maxScore);

    Float invSumExp = Float(1) / sumExp;
    Float bestExp   = std::exp(policyScore(candidates[begin + best], params) - maxScore);
    Float Pt        = bestExp * invSumExp;
    Float logPt     = policyScore(candidates[begin + best], params) - maxScore - std::log(sumExp);
    Float PtClamped = std::clamp(Pt, Float(1e-6), Float(1 - 1e-6));
    Float PtLogPtDivPtSub1 = PtClamped / (PtClamped - 1) * logPt;
    Float dFLdCE           = std::pow(1 - Pt, gamma) * (gamma * PtLogPtDivPtSub1 + 1);

    for (uint32_t i = begin; i < end; i++) {
        Float probability = std::exp(policyScore(candidates[i], params) - maxScore) * invSumExp;
        Float dCEdScore   = i == begin + best ? Pt - 1 : probability;
        Float dFLdScore   = dFLdCE * dCEdScore;
        grads[candidates[i].indices[0]] += CoeffScale * dFLdScore;
        grads[candidates[i].indices[1]] += CoeffScale * dFLdScore;
    }
}

}  // namespace

namespace Tuning {

Tuner::Tuner(Dataset &trainDataset, Dataset *valDataset, TuningConfig config)
    : config(config)
    , threadPool(config.numThreads != 0 ? config.numThreads
                                        : std::max<size_t>(std::thread::hardware_concurrency(), 1))
{
    MESSAGEL("Tuner worker threads = " << threadPool.get_thread_count()
                                       << ", seed = " << config.seed << ".");
    MESSAGEL("Start initializing parameters...");
    initParams();

    if (config.tuneEval && !config.usePreviousScalingFactor && config.nStepsPerIteration < 1)
        throw std::invalid_argument("scaling-factor calibration requires at least one step");

    std::optional<PreparedCacheKey> validationCacheKey;
    if (config.memoryLimitMB != 0) {
        if (config.memoryLimitMB < 32
            || config.memoryLimitMB > std::numeric_limits<size_t>::max() / MiB)
            throw std::invalid_argument("file-backed memory limit must be at least 32 MiB");
        if (config.shardSizeMB == 0
            || config.shardSizeMB > std::numeric_limits<size_t>::max() / MiB)
            throw std::invalid_argument("prepared shard target must fit size_t");
        if (config.preparedCachePath.empty())
            throw std::invalid_argument("prepared cache path is required for file-backed tuning");
        if (config.shuffleTuneEntries)
            throw std::invalid_argument("global shuffle is not supported by file-backed tuning");

        constexpr size_t RuntimeAllowance            = 16 * MiB;
        constexpr size_t CalibrationScratchAllowance = RuntimeAllowance / 2;
        if (config.tuneEval && !config.usePreviousScalingFactor) {
            size_t calibrationBytes =
                checkedProduct(checkedProduct(static_cast<size_t>(config.nStepsPerIteration),
                                              sizeof(Float),
                                              "calibration grid"),
                               LogicalPartitions + 2,
                               "calibration grid");
            if (calibrationBytes > CalibrationScratchAllowance)
                throw std::invalid_argument(
                    "scaling-factor calibration grid exceeds its runtime memory allowance; "
                    "reduce --num-steps-per-iteration");
        }
        size_t budgetBytes = checkedProduct(config.memoryLimitMB, MiB, "memory limit");
        size_t paramCopies = LogicalPartitions + 5;
        size_t fixedTrainingBytes =
            checkedProduct(checkedProduct(paramCopies, tuneParams.size(), "optimizer state"),
                           sizeof(TuneParam),
                           "optimizer state");
        if (fixedTrainingBytes > budgetBytes || RuntimeAllowance > budgetBytes - fixedTrainingBytes)
            throw std::invalid_argument(
                "memory limit is too small for optimizer state and runtime allowance");

        size_t variableBytes     = budgetBytes - fixedTrainingBytes - RuntimeAllowance;
        fileWorkerBudgetBytes    = variableBytes / 2;
        fileShardBudgetBytes     = variableBytes - fileWorkerBudgetBytes;
        size_t workerCount       = std::max<size_t>(threadPool.get_thread_count(), 1);
        size_t inputDatasetCount = valDataset ? 2 : 1;
        fileRecordLimitBytes =
            std::min<size_t>(8 * MiB,
                             fileWorkerBudgetBytes / (2 * (workerCount + inputDatasetCount)));
        if (fileRecordLimitBytes < 64 * KiB || fileShardBudgetBytes < 64 * KiB)
            throw std::invalid_argument(
                "memory limit leaves insufficient worker or shard allocation credit");
        fileJobBudgetBytes  = fileWorkerBudgetBytes - inputDatasetCount * fileRecordLimitBytes;
        fileMaxPendingJobs  = workerCount;
        fileChunkEntryLimit = std::max<size_t>(
            1,
            std::min(config.batchSize, fileJobBudgetBytes / workerCount / PreparedSampleCredit));
        size_t requestedShardBytes = checkedProduct(config.shardSizeMB, MiB, "shard target");
        fileShardTargetBytes =
            std::min(requestedShardBytes, fileShardBudgetBytes - fileShardBudgetBytes / 4);

        MESSAGEL("File-backed budget: workers "
                 << fileWorkerBudgetBytes / double(MiB) << " MiB, shard "
                 << fileShardBudgetBytes / double(MiB) << " MiB, shard target "
                 << fileShardTargetBytes / double(MiB) << " MiB.");
        PreparedCacheKey trainCacheKey =
            makePreparedCacheKey(config.trainDatasetPaths, config.trainDatasetFormat, "train");
        if (valDataset)
            validationCacheKey.emplace(makePreparedCacheKey(config.validationDatasetPaths,
                                                            config.validationDatasetFormat,
                                                            "validation"));
        validatePreparedCacheRoot(config.preparedCachePath,
                                  trainCacheKey,
                                  validationCacheKey ? &*validationCacheKey : nullptr);
        trainFileCorpus = std::make_unique<FileBackedCorpus>(config.preparedCachePath,
                                                             "train",
                                                             fileShardBudgetBytes,
                                                             std::move(trainCacheKey),
                                                             config.rebuildPreparedCache);
        MESSAGEL(trainFileCorpus->cacheStatus() << '.');
    }

    if (!trainFileCorpus || !trainFileCorpus->reused()) {
        if (trainFileCorpus)
            trainDataset.reset();
        MESSAGEL("Start initializing tune entries from training dataset...");
        initTuneEntries(trainTuneEntries, trainFileCorpus.get(), trainDataset, true);
        if (trainFileCorpus)
            trainFileCorpus->publish();
    }
    else
        MESSAGEL(trainFileCorpus->size() << " training entries restored from prepared cache.");

    if (valDataset) {
        if (config.memoryLimitMB != 0) {
            assert(validationCacheKey);
            valFileCorpus = std::make_unique<FileBackedCorpus>(config.preparedCachePath,
                                                               "validation",
                                                               fileShardBudgetBytes,
                                                               std::move(*validationCacheKey),
                                                               config.rebuildPreparedCache);
            MESSAGEL(valFileCorpus->cacheStatus() << '.');
        }
        if (!valFileCorpus || !valFileCorpus->reused()) {
            if (valFileCorpus)
                valDataset->reset();
            MESSAGEL("Start initializing tune entries from validation dataset...");
            initTuneEntries(valTuneEntries, valFileCorpus.get(), *valDataset, false);
            if (valFileCorpus)
                valFileCorpus->publish();
        }
        else
            MESSAGEL(valFileCorpus->size() << " validation entries restored from prepared cache.");
    }
}

size_t Tuner::trainingSampleCount() const
{
    return trainFileCorpus ? trainFileCorpus->size() : trainTuneEntries.size();
}

size_t Tuner::validationSampleCount() const
{
    return valFileCorpus ? valFileCorpus->size() : valTuneEntries.size();
}

/// run() runs the tuner for specified epochs. After each epoch completed, callback will be called.
void Tuner::run(size_t epochs, std::function<void(TuningStatistic)> callback)
{
    Time initTime = now();

    // Set Float output precision
    std::cout << std::setprecision(std::min(std::numeric_limits<Float>::digits10, 7)) << std::fixed;

    // Note: the last non-full batch of tune entries will be dropped.
    // Validate this before calibration, which may otherwise scan the corpus
    // many times only to discover that training cannot run.
    size_t trainSampleCount = trainFileCorpus ? trainFileCorpus->size() : trainTuneEntries.size();
    size_t numBatches       = trainSampleCount / config.batchSize;
    if (numBatches == 0)
        throw std::runtime_error("training dataset has fewer accepted entries than one batch");

    if (trainFileCorpus && trainFileCorpus->maxShardStorageBytes() > fileShardBudgetBytes)
        throw std::logic_error("prepared shard escaped its allocation credit");

    // Search a new K or use previous K. Policy-only tuning does not use a
    // value scaling factor and must not scan the corpus for calibration.
    Float K = Float(1.0) / Evaluation::ScalingFactor;
    if (!config.tuneEval) {
        MESSAGEL("Skip scaling factor search for policy-only tuning.");
    }
    else if (config.usePreviousScalingFactor) {
        MESSAGEL("Use previous inv scaling factor = " << K);
    }
    else {
        MESSAGEL("Start seaching for optimal inv scaling factor...");
        K = searchOptimalInvScalingFactor(false);
    }

    // Init gradient array and optimizer
    std::vector<TuneGradient> gradients(tuneParams.size());
    AdamOptimizer<TuneParam>  optim(tuneParams.size(),
                                   TuneParam(config.learningRate),
                                   TuneParam(config.weightDecay));

    MESSAGEL("Start training for " << epochs << " epochs, lr = " << optim.currentLR()
                                   << ", batch size = " << config.batchSize
                                   << ", number of batches = " << numBatches << ".");

    auto updateBatch = [&](const PreparedCorpus        &entries,
                           size_t                       batchBegin,
                           const std::vector<uint32_t> *sampleOrder) {
        std::fill(gradients.begin(), gradients.end(), TuneGradient(0));
        computeGradientBatch(gradients, K, entries, batchBegin, sampleOrder);
        if (std::any_of(gradients.begin(), gradients.end(), [](TuneGradient gradient) {
                return !std::isfinite(gradient);
            }))
            throw std::runtime_error("non-finite gradient in tuning batch");
        optim.step(tuneParams, gradients);
    };

    for (size_t epoch = 0; epoch <= epochs; epoch++) {
        Time startTime = now();

        if (epoch > 0) {
            if (trainFileCorpus) {
                size_t batchesProcessed = 0;
                for (size_t shard = 0; shard < trainFileCorpus->shardCount(); shard++) {
                    PreparedCorpus entries = trainFileCorpus->load(shard);
                    for (size_t batchBegin = 0; batchBegin + config.batchSize <= entries.size();
                         batchBegin += config.batchSize) {
                        updateBatch(entries, batchBegin, nullptr);
                        batchesProcessed++;
                    }
                }
                if (batchesProcessed != numBatches)
                    throw std::logic_error(
                        "prepared shard boundaries do not preserve global batches");
            }
            else {
                const std::vector<uint32_t> *order =
                    trainSampleOrder.empty() ? nullptr : &trainSampleOrder;
                for (size_t batch = 0; batch < numBatches; batch++)
                    updateBatch(trainTuneEntries, batch * config.batchSize, order);
            }
        }

        // Recalibrate against the parameters updated by this epoch before
        // reporting metrics or exporting a checkpoint with the new scale.
        if (config.tuneEval && !config.usePreviousScalingFactor && epoch > 0
            && config.recomputeInterval && epoch % config.recomputeInterval == 0) {
            K = searchOptimalInvScalingFactor(true);
        }

        // Print out current epoch and loss
        auto [valueLoss, policyLoss]       = computeLosses(K, false);
        auto [valueValLoss, policyValLoss] = computeLosses(K, true);
        if (!(std::isfinite(valueLoss) && std::isfinite(policyLoss) && std::isfinite(valueValLoss)
              && std::isfinite(policyValLoss)))
            throw std::runtime_error("non-finite tuning metric");
        Time elapsed = now() - startTime;
        if (valFileCorpus ? !valFileCorpus->empty() : !valTuneEntries.empty())
            MESSAGEL("Epoch " << epoch << " | Value " << valueLoss << " | Policy " << policyLoss
                              << " | ValueVal " << valueValLoss << " | PolicyVal " << policyValLoss
                              << " | Time(ms) " << elapsed);
        else
            MESSAGEL("Epoch " << epoch << " | Value " << valueLoss << " | Policy " << policyLoss
                              << " | Time(ms) " << elapsed);

        // Call callback after each epoch completed
        if (callback) {
            TuningStatistic stat;
            stat.currentEpoch   = epoch;
            stat.valueLoss      = valueLoss;
            stat.policyLoss     = policyLoss;
            stat.valueValLoss   = valueValLoss;
            stat.policyValLoss  = policyValLoss;
            stat.elapsedSeconds = double(elapsed) / 1000.0;
            stat.scalingFactor  = 1.0 / double(K);
            callback(stat);
        }
    }

    Time totalElapsed = now() - initTime;
    MESSAGEL("Training completed in " << (totalElapsed / 1000) << " seconds.");
}

/// initParams() inits tuneParams according to their value in the live
/// Evaluation:: model tables. It also associates TuneParam index with its
/// table address. Parameters loaded from the tables will be automatically
/// saved back when Tuner is destroyed.
void Tuner::initParams()
{
    std::vector<int> ruleSetIdx;
    PRNG             prng = PRNG(domainSeed(config.seed, 0x706172616d2d696eULL));
    std::uniform_real_distribution<double> rand;

    if (config.tuneRule[FREESTYLE])
        ruleSetIdx.push_back(FREESTYLE);
    if (config.tuneRule[STANDARD])
        ruleSetIdx.push_back(STANDARD);
    if (config.tuneRule[RENJU]) {
        ruleSetIdx.push_back(RENJU + BLACK);
        ruleSetIdx.push_back(RENJU + WHITE);
    }

    // The tuner intentionally mutates the LIVE Evaluation:: tables in place
    // through these stored addresses: evaluation during tuning always sees the
    // current candidate parameters without a copy/swap step.
    for (int r : ruleSetIdx) {
        if (config.tuneEval) {
            addArrayParams<Eval>(
                "eval/basic/table-" + std::to_string(r),
                Evaluation::EVALS[r],
                [](const Eval &ev, size_t) { return TuneParam(ev); },
                [](Eval &ev, size_t, TuneParam param) {
                    constexpr Float EvalMin = Float(std::numeric_limits<Eval>::min());
                    constexpr Float EvalMax = Float(std::numeric_limits<Eval>::max());
                    ev = static_cast<Eval>(std::clamp(Float(param), EvalMin, EvalMax));
                });

            addArrayParams<Eval>(
                "eval/threat/table-" + std::to_string(r),
                Evaluation::EVALS_THREAT[r],
                [](const Eval &ev, size_t) { return TuneParam(ev); },
                [](Eval &ev, size_t, TuneParam param) {
                    constexpr Float EvalMin = Float(std::numeric_limits<Eval>::min());
                    constexpr Float EvalMax = Float(std::numeric_limits<Eval>::max());
                    ev = static_cast<Eval>(std::clamp(Float(param), EvalMin, EvalMax));
                });
        }

        if (config.tuneMoveScore) {
            addArrayParams<MoveScorePair, arraySize(Evaluation::P4SCORES[0]), 2>(
                "move-score/table-" + std::to_string(r),
                Evaluation::P4SCORES[r],
                [scale      = config.moveScoreScale,
                 bias       = config.moveScoreBias,
                 randomInit = config.randomMoveScoreInit,
                 &rand,
                 &prng](const MoveScorePair &scorePair, size_t offset) {
                    Score score = scorePair[offset];
                    return randomInit ? TuneParam(rand(prng))
                                      : encodeIntegerForTruncatingExport(score, scale, bias);
                },
                [scoreMin = (Float)config.moveScoreMin,
                 scoreMax = (Float)config.moveScoreMax,
                 scale    = config.moveScoreScale,
                 bias     = config.moveScoreBias](MoveScorePair &scorePair,
                                              size_t         offset,
                                              TuneParam      param) {
                    Float score       = Float(param) * scale + bias;
                    scorePair[offset] = static_cast<Score>(std::clamp(score, scoreMin, scoreMax));
                });
        }
    }

    MESSAGEL(tuneParams.size() << " parameters initialized.");
}

PreparedCacheKey Tuner::makePreparedCacheKey(const std::vector<std::filesystem::path> &sourcePaths,
                                             const std::string &datasetFormat,
                                             const char        *role) const
{
    if (sourcePaths.empty() || datasetFormat.empty())
        throw std::invalid_argument(
            "strict prepared caching requires dataset paths and a dataset format");

    Sha256 hasher;
    hashString(hasher, "rapfi-classical-prepared-corpus-v4");
    hashString(hasher, role);
    hashString(hasher, datasetFormat);
    hashUint64(hasher, config.maxTuneEntries);
    hashUint64(hasher, config.batchSize);
    hashUint64(hasher, config.shardSizeMB);
    hashUint64(hasher, config.boardSizeMin);
    hashUint64(hasher, config.boardSizeMax);
    hashUint64(hasher, config.minPly);
    hashUint64(hasher, config.minPlyBeforeFull);
    hashUint64(hasher, config.tuneEval);
    hashUint64(hasher, config.tuneMoveScore);
    hashUint64(hasher, static_cast<uint64_t>(Config::GeneralCfg.defaultCandidateRange));
    for (bool tuneRule : config.tuneRule)
        hashUint64(hasher, tuneRule);
    hashDouble(hasher, config.moveScoreScale);
    hashDouble(hasher, config.moveScoreBias);
    hashUint64(hasher, static_cast<uint64_t>(config.moveScoreMin));
    hashUint64(hasher, static_cast<uint64_t>(config.moveScoreMax));
    hashUint64(hasher, static_cast<uint64_t>(CoeffScale));

    hashUint64(hasher, syncRecords.size());
    for (const ParamsSyncRecord &record : syncRecords) {
        hashString(hasher, record.layoutTag);
        hashUint64(hasher, record.baseIndex);
        hashUint64(hasher, record.numElems);
        hashUint64(hasher, record.elemSize);
        hashUint64(hasher, record.paramPerElem);
    }
    hashUint64(hasher, tuneParams.size());
    for (TuneParam param : tuneParams)
        hashFloat(hasher, param);

    PreparedCacheKey key;
    key.sources.reserve(sourcePaths.size());
    std::unordered_map<std::string, PreparedSourceInfo> knownSources;
    for (const std::filesystem::path &sourcePath : sourcePaths) {
        std::error_code       error;
        std::filesystem::path configured =
            std::filesystem::absolute(sourcePath, error).lexically_normal();
        if (error)
            throw std::runtime_error("unable to resolve tuning dataset source: "
                                     + sourcePath.string());
        std::filesystem::path canonical = std::filesystem::weakly_canonical(configured, error);
        if (error)
            throw std::runtime_error("unable to canonicalize tuning dataset source: "
                                     + configured.string());
        std::string configuredPath = configured.generic_u8string();
        std::string canonicalPath  = canonical.generic_u8string();

        auto found = knownSources.find(canonicalPath);
        if (found == knownSources.end()) {
            uintmax_t fileSize = std::filesystem::file_size(canonical, error);
            if (error)
                throw std::runtime_error("unable to inspect tuning dataset source: "
                                         + canonical.string());
            auto modified = std::filesystem::last_write_time(canonical, error);
            if (error)
                throw std::runtime_error("unable to read tuning dataset timestamp: "
                                         + canonical.string());
            std::string sourceDigest = sha256Hex(sha256File(canonical));
            error.clear();
            uintmax_t verifiedSize = std::filesystem::file_size(canonical, error);
            if (error)
                throw std::runtime_error("unable to recheck tuning dataset source: "
                                         + canonical.string());
            auto verifiedModified = std::filesystem::last_write_time(canonical, error);
            if (error || verifiedSize != fileSize || verifiedModified != modified)
                throw std::runtime_error("tuning dataset source changed while hashing: "
                                         + canonical.string());
            PreparedSourceInfo info {std::string {},
                                     canonicalPath,
                                     fileSize,
                                     static_cast<int64_t>(modified.time_since_epoch().count()),
                                     std::move(sourceDigest)};
            found = knownSources.emplace(canonicalPath, std::move(info)).first;
        }
        error.clear();
        std::filesystem::path verifiedCanonical =
            std::filesystem::weakly_canonical(configured, error);
        if (error || verifiedCanonical != canonical)
            throw std::runtime_error("tuning dataset alias changed while hashing: "
                                     + configured.string());

        PreparedSourceInfo sourceInfo = found->second;
        sourceInfo.configuredPath     = std::move(configuredPath);
        key.sources.push_back(std::move(sourceInfo));
        const PreparedSourceInfo &source = key.sources.back();
        hashString(hasher, source.configuredPath);
        hashString(hasher, source.canonicalPath);
        hashUint64(hasher, source.size);
        hashString(hasher, source.sha256);
    }
    key.fingerprint = sha256Hex(hasher.finish());
    return key;
}

/// saveParams() saves tuneParams back to their associated config value
void Tuner::saveParams() const
{
    for (const ParamsSyncRecord &record : syncRecords) {
        assert(tuneParams.size() >= record.baseIndex + record.numElems * record.paramPerElem);

        for (size_t i = 0; i < record.numElems; i++)
            for (size_t j = 0; j < record.paramPerElem; j++)
                record.setter(record[i],
                              j,
                              tuneParams[record.baseIndex + i * record.paramPerElem + j]);
    }

    MESSAGEL(tuneParams.size() << " parameters saved.");
}

void Tuner::appendTuneSample(PreparedCorpus &tuneEntries,
                             const Board    &board,
                             Rule            rule,
                             uint8_t         resultTimesTwo,
                             Pos             bestMove,
                             CompileScratch &scratch) const
{
    Value staticEval = Evaluation::evaluate(board, rule);
    if (staticEval < INT16_MIN || staticEval > INT16_MAX)
        throw std::overflow_error("static evaluation exceeds int16 storage");

    scratch.evalTerms.clear();
    if (config.tuneEval) {
        Evaluation::EvalInfo evalInfo(board, rule);
        collectEvalCoeffs(rule, evalInfo, [this, &scratch](int coeff, int coeffScale, void *addr) {
            if (coeff == 0)
                return;
            int scaledCoeff = int(coeff * CoeffScale) / coeffScale;
            if (scaledCoeff < INT16_MIN || scaledCoeff > INT16_MAX)
                throw std::overflow_error("value coefficient exceeds int16 storage");
            scratch.evalTerms.push_back({static_cast<int16_t>(scaledCoeff), paramIndex(addr)});
        });
    }

    scratch.policyCandidates.clear();
    uint16_t bestCandidate = PreparedCorpus::NoPolicyTarget;
    if (config.tuneMoveScore && bestMove != Pos {board.size(), board.size()}
        && bestMove != Pos::NONE && bestMove != Pos::PASS && board.isEmptyCandidate(bestMove)) {
        collectMoveScoreCoeffs(
            rule,
            board,
            [this, bestMove, &scratch, &bestCandidate](Pos   pos,
                                                       int   coeffSelf,
                                                       int   coeffOppo,
                                                       void *addrSelf,
                                                       void *addrOppo) {
                if (coeffSelf != 1 || coeffOppo != 1)
                    throw std::logic_error("compact policy storage requires unit coefficients");

                if (pos == bestMove)
                    bestCandidate = static_cast<uint16_t>(scratch.policyCandidates.size());
                PolicyCandidate candidate;
                candidate.indices[0] = paramIndex(addrSelf, 0);
                candidate.indices[1] = paramIndex(addrOppo, 1);
                scratch.policyCandidates.push_back(candidate);
            });
        if (bestCandidate == PreparedCorpus::NoPolicyTarget)
            throw std::logic_error("best move is missing from policy candidates");
    }

    if (config.tuneEval) {
        Float linearEval = 0;
        for (const TuneCoeff &term : scratch.evalTerms)
            linearEval += term.coeff * tuneParams[term.index];
        linearEval /= CoeffScale;
        if (!checkEqual(Float(staticEval), linearEval))
            throw std::logic_error("prepared linear evaluation differs from engine evaluation");
    }

    tuneEntries.append(resultTimesTwo,
                       static_cast<int16_t>(staticEval),
                       scratch.evalTerms,
                       scratch.policyCandidates,
                       bestCandidate);
}

/// initTuneEntries() inits tuneEntries from dataEntry read from datasets.
/// DataEntry that does not satisfy a certain condition will be skipped.
void Tuner::initTuneEntries(PreparedCorpus   &tuneEntries,
                            FileBackedCorpus *fileCorpus,
                            class Dataset    &dataset,
                            bool              buildShuffleOrder)
{
    tuneEntries.clear();

    struct PreparedJob
    {
        std::future<PreparedCorpus> future;
        size_t                      creditBytes;
    };
    std::deque<PreparedJob> jobs;
    size_t                  pendingCreditBytes = 0;
    const size_t            workerCount        = std::max<size_t>(threadPool.get_thread_count(), 1);
    size_t                  maxPendingJobs     = workerCount;
    size_t                  chunkEntryLimit    = config.batchSize;

    if (fileCorpus) {
        maxPendingJobs  = fileMaxPendingJobs;
        chunkEntryLimit = fileChunkEntryLimit;
        dataset.setMaxRecordBytes(fileRecordLimitBytes);
        dataset.setRetainExtraPVs(false);
        MESSAGEL("File-backed preparation chunk = "
                 << chunkEntryLimit << " raw entries, pending jobs = " << maxPendingJobs
                 << ", record limit = " << fileRecordLimitBytes / double(MiB) << " MiB.");
    }

    PreparedCorpus openShard;
    PreparedCorpus batchCorpus;

    auto sealOpenShard = [&]() {
        if (!fileCorpus || openShard.empty())
            return;
        if (openShard.capacityBytes() > fileShardBudgetBytes)
            throw std::logic_error("prepared shard escaped its allocation credit");
        fileCorpus->append(std::move(openShard));
        openShard = PreparedCorpus {};
    };

    auto appendBatchToOpenShard = [&]() {
        if (!fileCorpus || batchCorpus.empty())
            return;

        auto copyFitsCredit = [&]() {
            size_t peakBytes =
                openShard.appendRangePeakCapacityBytes(batchCorpus, 0, batchCorpus.size());
            return batchCorpus.capacityBytes() <= fileShardBudgetBytes
                   && peakBytes <= fileShardBudgetBytes - batchCorpus.capacityBytes();
        };

        if (!copyFitsCredit() && !openShard.empty())
            sealOpenShard();

        if (copyFitsCredit()) {
            openShard.reserveAppendRange(batchCorpus, 0, batchCorpus.size());
            openShard.appendRange(batchCorpus, 0, batchCorpus.size());
            batchCorpus = PreparedCorpus {};
            if (openShard.capacityBytes() >= fileShardTargetBytes)
                sealOpenShard();
            return;
        }

        if (!openShard.empty())
            throw std::logic_error("prepared shard could not be sealed before direct batch write");
        if (batchCorpus.capacityBytes() > fileShardBudgetBytes)
            throw std::runtime_error(
                "one prepared gradient batch exceeds its shard allocation credit; "
                "increase --memory-limit-mb or reduce --batchsize");
        fileCorpus->append(std::move(batchCorpus));
        batchCorpus = PreparedCorpus {};
    };

    auto collectFrontJob = [&]() {
        size_t         creditBytes = jobs.front().creditBytes;
        PreparedCorpus fragment    = jobs.front().future.get();
        jobs.pop_front();
        if (!fileCorpus) {
            tuneEntries.append(std::move(fragment));
            pendingCreditBytes -= creditBytes;
            return;
        }

        for (size_t begin = 0; begin < fragment.size();) {
            size_t batchSpace = config.batchSize - batchCorpus.size();
            size_t count      = std::min(batchSpace, fragment.size() - begin);

            while (true) {
                size_t peakBytes = batchCorpus.appendRangePeakCapacityBytes(fragment, begin, count);
                if (openShard.capacityBytes() <= fileShardBudgetBytes
                    && peakBytes <= fileShardBudgetBytes - openShard.capacityBytes()) {
                    batchCorpus.reserveAppendRange(fragment, begin, count);
                    batchCorpus.appendRange(fragment, begin, count);
                    break;
                }
                if (!openShard.empty()) {
                    sealOpenShard();
                    continue;
                }
                throw std::runtime_error(
                    "one prepared gradient batch exceeds its shard allocation credit; "
                    "increase --memory-limit-mb or reduce --batchsize");
            }
            begin += count;
            if (batchCorpus.size() == config.batchSize)
                appendBatchToOpenShard();
        }
        pendingCreditBytes -= creditBytes;
    };

    auto ensureWorkerCredit = [&](size_t creditBytes) {
        if (!fileCorpus)
            return;
        if (creditBytes > fileJobBudgetBytes)
            throw std::runtime_error(
                "one dataset job exceeds the worker allocation credit; "
                "increase --memory-limit-mb or reduce the record or batch size");
        while (!jobs.empty()
               && (jobs.size() >= maxPendingJobs
                   || pendingCreditBytes > fileJobBudgetBytes - creditBytes))
            collectFrontJob();
        if (pendingCreditBytes > fileJobBudgetBytes - creditBytes)
            throw std::logic_error("worker allocation credit accounting failed");
    };

    // Read dataset and convert bounded batches to compact corpus fragments.
    size_t totalEntriesRead = 0;
    if (dataset.supportsGames()) {
        using GameWork   = std::pair<GameEntry, size_t>;
        auto submitGames = [&](std::vector<GameWork> &&games,
                               size_t                  chunkEntries,
                               size_t                  creditBytes) {
            auto sharedGames = std::make_shared<std::vector<GameWork>>(std::move(games));
            jobs.push_back(PreparedJob {
                threadPool.submit_task(
                    [this, games = std::move(sharedGames), chunkEntries]() -> PreparedCorpus {
                        PreparedCorpus entries;
                        CompileScratch scratch;
                        entries.reserveSamples(chunkEntries);

                        for (const auto &work : *games) {
                            const GameEntry &game      = work.first;
                            size_t           moveLimit = work.second;
                            Board            board(game.boardsize);
                            board.newGame(game.rule);
                            for (Pos pos : game.initPosition)
                                board.move(game.rule, pos);

                            for (size_t moveIndex = 0; moveIndex < moveLimit; moveIndex++) {
                                size_t ply = game.initPosition.size() + moveIndex;
                                if (config.tuneRule[game.rule]
                                    && game.boardsize >= config.boardSizeMin
                                    && game.boardsize <= config.boardSizeMax && ply >= config.minPly
                                    && ply + config.minPlyBeforeFull
                                           <= int(game.boardsize) * int(game.boardsize)) {
                                    Result  result         = board.sideToMove() == WHITE
                                                                 ? game.result
                                                                 : flipResult(game.result);
                                    uint8_t resultTimesTwo = result == RESULT_WIN    ? 2
                                                             : result == RESULT_DRAW ? 1
                                                                                     : 0;
                                    appendTuneSample(entries,
                                                     board,
                                                     game.rule,
                                                     resultTimesTwo,
                                                     game.moveSequence[moveIndex].move,
                                                     scratch);
                                }
                                board.move(game.rule, game.moveSequence[moveIndex].move);
                            }
                        }
                        return entries;
                    }),
                creditBytes});
            pendingCreditBytes += creditBytes;
        };

        bool reachedEnd = false;
        while (totalEntriesRead < config.maxTuneEntries && !reachedEnd) {
            std::vector<GameWork> games;
            size_t                chunkEntries = 0;
            bool                  submitted    = false;

            do {
                if (fileCorpus)
                    ensureWorkerCredit(fileRecordLimitBytes);
                GameEntry game;
                if (!dataset.nextGame(&game)) {
                    reachedEnd = true;
                    break;
                }

                size_t gameMoveCount = game.moveSequence.size();
                size_t remaining     = config.maxTuneEntries - totalEntriesRead - chunkEntries;
                size_t moveLimit     = std::min(gameMoveCount, remaining);
                if (moveLimit != 0) {
                    size_t creditBytes = 0;
                    if (fileCorpus) {
                        size_t preparedBytes =
                            checkedProduct(moveLimit, PreparedSampleCredit, "prepared job");
                        if (preparedBytes > fileJobBudgetBytes - fileRecordLimitBytes)
                            throw std::runtime_error(
                                "one game cannot be prepared within the worker allocation credit");
                        creditBytes = fileRecordLimitBytes + preparedBytes;
                        ensureWorkerCredit(creditBytes);
                    }
                    chunkEntries += moveLimit;
                    games.emplace_back(std::move(game), moveLimit);
                    if (fileCorpus) {
                        totalEntriesRead += chunkEntries;
                        submitGames(std::move(games), chunkEntries, creditBytes);
                        chunkEntries = 0;
                        submitted    = true;
                        break;
                    }
                }
                if (moveLimit < gameMoveCount)
                    break;
            } while (chunkEntries < chunkEntryLimit
                     && totalEntriesRead + chunkEntries < config.maxTuneEntries);

            if (submitted || games.empty())
                continue;

            totalEntriesRead += chunkEntries;
            submitGames(std::move(games), chunkEntries, 0);
            if (jobs.size() >= maxPendingJobs)
                collectFrontJob();
        }
    }
    else {
        while (totalEntriesRead < config.maxTuneEntries) {
            size_t entriesToRead =
                std::min(chunkEntryLimit, config.maxTuneEntries - totalEntriesRead);
            size_t creditBytes =
                fileCorpus
                    ? checkedProduct(entriesToRead, PreparedSampleCredit, "prepared dataset chunk")
                    : 0;
            ensureWorkerCredit(creditBytes);
            std::vector<DataEntry> dataEntries;
            dataEntries.reserve(entriesToRead);
            for (size_t i = 0; i < entriesToRead; i++) {
                DataEntry dataEntry;
                if (!dataset.next(&dataEntry))
                    break;
                dataEntries.push_back(std::move(dataEntry));
            }

            if (dataEntries.empty())
                break;
            totalEntriesRead += dataEntries.size();

            jobs.push_back(PreparedJob {
                threadPool.submit_task([this, data = std::move(dataEntries)]() -> PreparedCorpus {
                    std::unordered_map<int, Board> boardObjectCache;
                    PreparedCorpus                 entries;
                    CompileScratch                 scratch;
                    entries.reserveSamples(data.size());

                    for (const DataEntry &dataEntry : data) {
                        if (!config.tuneRule[dataEntry.rule]
                            || dataEntry.boardsize < config.boardSizeMin
                            || dataEntry.boardsize > config.boardSizeMax
                            || dataEntry.position.size() < config.minPly
                            || dataEntry.position.size() + config.minPlyBeforeFull
                                   > int(dataEntry.boardsize) * int(dataEntry.boardsize))
                            continue;

                        auto boardIt = boardObjectCache.find(dataEntry.boardsize);
                        if (boardIt == boardObjectCache.end()) {
                            boardIt = boardObjectCache
                                          .emplace(std::piecewise_construct,
                                                   std::forward_as_tuple(dataEntry.boardsize),
                                                   std::forward_as_tuple(dataEntry.boardsize))
                                          .first;
                        }

                        Board &board = boardIt->second;
                        board.newGame(dataEntry.rule);
                        for (Pos pos : dataEntry.position)
                            board.move(dataEntry.rule, pos);

                        uint8_t resultTimesTwo = dataEntry.result == RESULT_WIN    ? 2
                                                 : dataEntry.result == RESULT_DRAW ? 1
                                                                                   : 0;
                        appendTuneSample(entries,
                                         board,
                                         dataEntry.rule,
                                         resultTimesTwo,
                                         dataEntry.move,
                                         scratch);
                    }

                    return entries;
                }),
                creditBytes});
            pendingCreditBytes += creditBytes;

            if (jobs.size() >= maxPendingJobs)
                collectFrontJob();
        }
    }

    MESSAGEL("Read " << totalEntriesRead << " tune entries from dataset, initializing...");

    while (!jobs.empty())
        collectFrontJob();

    if (fileCorpus) {
        appendBatchToOpenShard();
        sealOpenShard();
        MESSAGEL(fileCorpus->size()
                 << " tune entries initialized in " << fileCorpus->shardCount()
                 << " file-backed shards (" << fileCorpus->diskBytes() / (1024.0 * 1024.0)
                 << " MiB) at " << fileCorpus->directory().string() << '.');
    }
    else {
        MESSAGEL(tuneEntries.size() << " tune entries initialized in "
                                    << tuneEntries.capacityBytes() / (1024.0 * 1024.0) << " MiB.");
    }

    if (buildShuffleOrder && config.shuffleTuneEntries) {
        MESSAGEL("Creating logical shuffle order...");

        if (tuneEntries.size() > std::numeric_limits<uint32_t>::max())
            throw std::length_error("shuffle order exceeds 32-bit sample indices");
        trainSampleOrder.resize(tuneEntries.size());
        std::iota(trainSampleOrder.begin(), trainSampleOrder.end(), uint32_t(0));
        PRNG prng(domainSeed(config.seed, 0x73687566666c6500ULL));
        std::shuffle(trainSampleOrder.begin(), trainSampleOrder.end(), prng);
    }
}

/// searchOptimalInvScalingFactor() searches the optimal K in
/// winRate = 1 / (1 + exp(-Eval * K)). It scans the scaling-factor space for
/// several iterations and minimizes the error of either the original static
/// evaluation or the current tuned evaluation.
Float Tuner::searchOptimalInvScalingFactor(bool useTunedEval) const
{
    assert(config.nStepsPerIteration);

    Float startK = 1.0 / Float(config.scalingFactorMin);
    Float endK   = 1.0 / Float(config.scalingFactorMax);
    Float stepK  = (endK - startK) / Float(config.nStepsPerIteration);
    Float bestK  = 0;

    for (int iter = 1; iter <= config.nIterations; iter++) {
        std::vector<Float> candidates(config.nStepsPerIteration);
        Float              k = startK;
        for (Float &candidate : candidates) {
            candidate = k;
            k += stepK;
        }
        std::vector<Float> losses   = useTunedEval ? computeEvaluationLossGrid<true>(candidates)
                                                   : computeEvaluationLossGrid<false>(candidates);
        Float              bestLoss = std::numeric_limits<Float>::max();

        for (int i = 0; i < config.nStepsPerIteration; i++) {
            if (losses[i] < bestLoss) {
                bestLoss = losses[i];
                bestK    = candidates[i];
            }
        }

        MESSAGEL("Iteration " << iter << " | K " << bestK << " | Loss " << bestLoss);

        startK = bestK - stepK;
        endK   = bestK + stepK;
        stepK  = stepK * 2 / Float(config.nStepsPerIteration);
    }

    MESSAGEL("Optimal inv scaling factor K = " << bestK << " after " << config.nIterations
                                               << " iteration.");

    return bestK;
}

template <bool UseTunedEval>
std::vector<Float> Tuner::computeEvaluationLossGrid(const std::vector<Float> &candidates) const
{
    std::vector<Float> total(candidates.size(), Float(0));
    auto accumulateEntries = [this, &candidates, &total](const PreparedCorpus &entries,
                                                         bool                  applyShuffleOrder) {
        if (entries.empty())
            return;
        size_t numBlocks = std::min(entries.size(), LogicalPartitions);
        size_t blockSize = (entries.size() + numBlocks - 1) / numBlocks;
        std::vector<std::future<std::vector<Float>>> futures;
        futures.reserve(numBlocks);
        for (size_t block = 0; block < numBlocks; block++) {
            size_t blockBegin = block * blockSize;
            size_t blockEnd   = std::min(blockBegin + blockSize, entries.size());
            if (blockBegin == blockEnd)
                break;
            futures.emplace_back(threadPool.submit_task(
                [this, &entries, &candidates, applyShuffleOrder, blockBegin, blockEnd] {
                    std::vector<Float> blockLosses(candidates.size(), Float(0));
                    for (size_t logicalSample = blockBegin; logicalSample < blockEnd;
                         logicalSample++) {
                        size_t sample = applyShuffleOrder && !trainSampleOrder.empty()
                                            ? trainSampleOrder[logicalSample]
                                            : logicalSample;
                        for (size_t candidate = 0; candidate < candidates.size(); candidate++)
                            blockLosses[candidate] +=
                                ::computeEvalLoss<UseTunedEval>(entries,
                                                                sample,
                                                                tuneParams,
                                                                candidates[candidate],
                                                                config.lossType);
                    }
                    return blockLosses;
                }));
        }
        for (auto &future : futures) {
            std::vector<Float> blockLosses = future.get();
            for (size_t candidate = 0; candidate < total.size(); candidate++)
                total[candidate] += blockLosses[candidate];
        }
    };

    size_t sampleCount = 0;
    if (trainFileCorpus) {
        sampleCount = trainFileCorpus->size();
        for (size_t shard = 0; shard < trainFileCorpus->shardCount(); shard++) {
            PreparedCorpus entries = trainFileCorpus->load(shard);
            accumulateEntries(entries, false);
        }
    }
    else {
        sampleCount = trainTuneEntries.size();
        accumulateEntries(trainTuneEntries, true);
    }
    if (sampleCount == 0)
        return total;
    for (Float &loss : total)
        loss /= Float(sampleCount);
    return total;
}

/// computeEvaluationLoss() computes loss between the current tuned/static
/// evaluation and target win rate in all tune entries using the given K.
template <bool UseTunedEval>
Float Tuner::computeEvaluationLoss(Float K, bool validation) const
{
    if (!config.tuneEval)
        return Float(0.0);

    const FileBackedCorpus *fileCorpus = validation ? valFileCorpus.get() : trainFileCorpus.get();
    if (fileCorpus) {
        if (fileCorpus->empty())
            return Float(0.0);

        Float total = 0;
        for (size_t shard = 0; shard < fileCorpus->shardCount(); shard++) {
            PreparedCorpus entries = fileCorpus->load(shard);
            total += parallelIndexReduce<Float>(threadPool,
                                                entries.size(),
                                                Float(0.0),
                                                [this, &entries, K](size_t sample) {
                                                    return ::computeEvalLoss<UseTunedEval>(
                                                        entries,
                                                        sample,
                                                        tuneParams,
                                                        K,
                                                        config.lossType);
                                                });
        }
        return total / Float(fileCorpus->size());
    }

    const PreparedCorpus &entries = validation ? valTuneEntries : trainTuneEntries;

    if (entries.empty())
        return Float(0.0);

    return parallelIndexReduce<Float>(threadPool,
                                      entries.size(),
                                      Float(0.0),
                                      [this, &entries, K, validation](size_t logicalSample) {
                                          size_t sample = !validation && !trainSampleOrder.empty()
                                                              ? trainSampleOrder[logicalSample]
                                                              : logicalSample;
                                          return ::computeEvalLoss<UseTunedEval>(entries,
                                                                                 sample,
                                                                                 tuneParams,
                                                                                 K,
                                                                                 config.lossType);
                                      })
           / Float(entries.size());
}

/// computeMoveScoreLoss() computes loss of current move scores between
/// the target best move in all tune entries.
Float Tuner::computeMoveScoreLoss(bool validation) const
{
    if (!config.tuneMoveScore)
        return Float(0.0);

    const FileBackedCorpus *fileCorpus = validation ? valFileCorpus.get() : trainFileCorpus.get();
    if (fileCorpus) {
        if (fileCorpus->empty())
            return Float(0.0);

        Float total = 0;
        for (size_t shard = 0; shard < fileCorpus->shardCount(); shard++) {
            PreparedCorpus entries = fileCorpus->load(shard);
            total += parallelIndexReduce<Float>(threadPool,
                                                entries.size(),
                                                Float(0.0),
                                                [this, &entries](size_t sample) {
                                                    return ::computeMoveScoreLoss(
                                                        entries,
                                                        sample,
                                                        tuneParams,
                                                        config.moveScoreLossGamma);
                                                });
        }
        return total / Float(fileCorpus->size());
    }

    const PreparedCorpus &entries = validation ? valTuneEntries : trainTuneEntries;

    if (entries.empty())
        return Float(0.0);

    return parallelIndexReduce<Float>(threadPool,
                                      entries.size(),
                                      Float(0.0),
                                      [this, &entries, validation](size_t logicalSample) {
                                          size_t sample = !validation && !trainSampleOrder.empty()
                                                              ? trainSampleOrder[logicalSample]
                                                              : logicalSample;
                                          return ::computeMoveScoreLoss(entries,
                                                                        sample,
                                                                        tuneParams,
                                                                        config.moveScoreLossGamma);
                                      })
           / Float(entries.size());
}

std::pair<Float, Float> Tuner::computeLosses(Float K, bool validation) const
{
    const FileBackedCorpus *fileCorpus = validation ? valFileCorpus.get() : trainFileCorpus.get();
    auto                    sampleLoss = [this, K](const PreparedCorpus &entries, size_t sample) {
        return LossPair {
            config.tuneEval
                                   ? ::computeEvalLoss<true>(entries, sample, tuneParams, K, config.lossType)
                                   : Float(0),
            config.tuneMoveScore
                                   ? ::computeMoveScoreLoss(entries, sample, tuneParams, config.moveScoreLossGamma)
                                   : Float(0),
        };
    };

    if (fileCorpus) {
        if (fileCorpus->empty())
            return {0, 0};
        LossPair total;
        for (size_t shard = 0; shard < fileCorpus->shardCount(); shard++) {
            PreparedCorpus entries = fileCorpus->load(shard);
            total += parallelIndexReduce<LossPair>(
                threadPool,
                entries.size(),
                LossPair {},
                [&entries, &sampleLoss](size_t sample) { return sampleLoss(entries, sample); });
        }
        return {total.value / Float(fileCorpus->size()), total.policy / Float(fileCorpus->size())};
    }

    const PreparedCorpus &entries = validation ? valTuneEntries : trainTuneEntries;
    if (entries.empty())
        return {0, 0};
    LossPair total = parallelIndexReduce<LossPair>(
        threadPool,
        entries.size(),
        LossPair {},
        [this, validation, &entries, &sampleLoss](size_t logicalSample) {
            size_t sample = !validation && !trainSampleOrder.empty()
                                ? trainSampleOrder[logicalSample]
                                : logicalSample;
            return sampleLoss(entries, sample);
        });
    return {total.value / Float(entries.size()), total.policy / Float(entries.size())};
}

/// computeGradients() computes gradients of all parameters used in one tune
/// entries batch and accumulates them into gradients vector. These gradients
/// then will be used to tune the parameters with a gradient descent optimizer.
void Tuner::computeGradientBatch(std::vector<TuneGradient>   &grads,
                                 Float                        K,
                                 const PreparedCorpus        &entries,
                                 size_t                       batchBegin,
                                 const std::vector<uint32_t> *sampleOrder)
{
    assert(grads.size() == tuneParams.size());
    const size_t numJobs     = std::min(LogicalPartitions, config.batchSize);
    const size_t baseJobSize = config.batchSize / numJobs;
    const size_t remainder   = config.batchSize % numJobs;

    if (partitionGradients.size() != numJobs)
        partitionGradients.assign(numJobs, std::vector<TuneGradient>(tuneParams.size()));

    std::vector<std::future<void>> gradJobs;
    gradJobs.reserve(numJobs);
    for (size_t jobIdx = 0; jobIdx < numJobs; jobIdx++) {
        // Get range of tune entries for this job
        size_t jobOffset = jobIdx * baseJobSize + std::min(jobIdx, remainder);
        size_t jobSize   = baseJobSize + (jobIdx < remainder);
        size_t jobBegin  = batchBegin + jobOffset;
        size_t jobEnd    = jobBegin + jobSize;

        // Accumulate local gradient asynchronously
        auto job =
            threadPool.submit_task([this, K, jobIdx, jobBegin, jobEnd, &entries, sampleOrder] {
                std::vector<TuneGradient> &localGrads = partitionGradients[jobIdx];
                std::fill(localGrads.begin(), localGrads.end(), TuneGradient(0));

                for (size_t logicalSample = jobBegin; logicalSample < jobEnd; logicalSample++) {
                    size_t sample = sampleOrder ? (*sampleOrder)[logicalSample] : logicalSample;
                    ::computeEvalGradient(entries,
                                          sample,
                                          localGrads,
                                          tuneParams,
                                          K,
                                          config.lossType);
                    ::computeMoveScoreGradient(entries,
                                               sample,
                                               localGrads,
                                               tuneParams,
                                               config.moveScoreLossGamma);
                }

                // Scale gradient according to batch size
                TuneGradient scale = TuneGradient(1 / Float(config.batchSize));
                for (TuneGradient &gradient : localGrads)
                    gradient *= scale;
            });
        gradJobs.push_back(std::move(job));
    }

    for (auto &job : gradJobs)
        job.get();

    // Reduce in logical partition order, independent of physical scheduling.
    for (const std::vector<TuneGradient> &localGrads : partitionGradients) {
        assert(grads.size() == localGrads.size());

        for (size_t i = 0; i < grads.size(); i++)
            grads[i] += localGrads[i];
    }
}

/// addParams() adds a continous range of params in config to tuneParams
void Tuner::addParams(std::string   layoutTag,
                      void         *address,
                      size_t        numElems,
                      uint32_t      elemSize,
                      uint32_t      paramPerElem,
                      ParamGetter<> getter,
                      ParamSetter<> setter)
{
    assert(paramPerElem > 0);
    if (layoutTag.empty())
        throw std::invalid_argument("tuning parameter layout tag must not be empty");
    if (std::any_of(syncRecords.begin(), syncRecords.end(), [&](const ParamsSyncRecord &record) {
            return record.layoutTag == layoutTag;
        }))
        throw std::logic_error("duplicate tuning parameter layout tag: " + layoutTag);
    size_t baseIndex = tuneParams.size();

    // Init parameters from getter and add them to tuneParams
    if (numElems > std::numeric_limits<size_t>::max() / paramPerElem)
        throw std::length_error("tuning parameter count overflows size_t");
    size_t           numParams         = numElems * paramPerElem;
    constexpr size_t MaxParameterCount = size_t(std::numeric_limits<ParameterId>::max()) + 1;
    if (numParams > MaxParameterCount - baseIndex)
        throw std::length_error("tuning parameter count exceeds ParameterId capacity");

    syncRecords.push_back(ParamsSyncRecord {std::move(layoutTag),
                                            baseIndex,
                                            numElems,
                                            elemSize,
                                            paramPerElem,
                                            address,
                                            std::move(getter),
                                            std::move(setter)});
    const ParamsSyncRecord &record = syncRecords.back();

    tuneParams.reserve(baseIndex + numParams);
    for (size_t i = 0; i < numElems; i++) {
        size_t elementBase  = baseIndex + i * paramPerElem;
        auto [it, inserted] = paramIndices.emplace(
            record[i],
            ParameterAddress {static_cast<ParameterId>(elementBase), paramPerElem});
        if (!inserted)
            throw std::logic_error("duplicate tuning parameter address");

        for (size_t j = 0; j < paramPerElem; j++) {
            TuneParam param = record.getter(record[i], j);
            if (!std::isfinite(param))
                throw std::runtime_error("non-finite initial tuning parameter");
            tuneParams.emplace_back(param);
        }
    }
}

/// paramIndex() finds tuneParams index according to address of its config value
ParameterId Tuner::paramIndex(const void *addr, size_t offset) const
{
    auto it = paramIndices.find(addr);
    if (it == paramIndices.end())
        throw std::logic_error("unknown tuning parameter address");

    if (offset >= it->second.parameterCount)
        throw std::out_of_range("tuning parameter offset is out of range");
    size_t index = size_t(it->second.baseIndex) + offset;
    if (index >= tuneParams.size())
        throw std::out_of_range("tuning parameter offset is out of range");
    return static_cast<ParameterId>(index);
}

}  // namespace Tuning
