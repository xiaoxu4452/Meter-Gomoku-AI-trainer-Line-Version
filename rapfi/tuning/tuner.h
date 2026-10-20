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

#include "../core/pos.h"
#include "../core/types.h"
#include "tunecorpus.h"
#include "tunestore.h"

#include <BS_thread_pool.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Board;

namespace Tuning {

using Float        = double;  // stable scalar intermediates and global reductions
using TuneParam    = float;   // high-cardinality parameter and optimizer state
using TuneGradient = float;   // persistent/per-partition gradient storage

/// LossType represents a type of loss function to use.
enum class LossType { L1, L2, BCE };

/// ParamGetter gets a TuneParam from a raw parameter given by an address and an offset.
template <typename AddrType = void *>
using ParamGetter = std::function<TuneParam(AddrType, size_t)>;
/// ParamSetter sets a raw parameter given by an address and an offset to a TuneParam.
template <typename AddrType = void *>
using ParamSetter = std::function<void(AddrType, size_t, TuneParam)>;

/// ParamsRecord struct is a helper to sync tune parameters with the config.
/// It also maps a range of addresses that contains the tunable parameters
/// to its base index in the tuneParams array.
struct ParamsSyncRecord
{
    std::string layoutTag;
    size_t      baseIndex;
    size_t      numElems;
    uint32_t    elemSize;
    uint32_t    paramPerElem;
    void       *address;

    ParamGetter<> getter;
    ParamSetter<> setter;

    void *operator[](size_t i) const { return static_cast<char *>(address) + elemSize * i; }
};

/// TuningConfig struct records all configuration settings used in Tuner
struct TuningConfig
{
    // --------------------------------------------
    // General training settings

    size_t                             batchSize      = 8192;
    size_t                             maxTuneEntries = UINT32_MAX;
    size_t                             numThreads     = 0;
    uint64_t                           seed           = 1;
    size_t                             memoryLimitMB  = 0;
    size_t                             shardSizeMB    = 64;
    std::filesystem::path              preparedCachePath;
    std::vector<std::filesystem::path> trainDatasetPaths;
    std::vector<std::filesystem::path> validationDatasetPaths;
    std::string                        trainDatasetFormat;
    std::string                        validationDatasetFormat;
    bool                               rebuildPreparedCache = false;
    double                             learningRate         = 0.01;
    double                             weightDecay          = 0.0;
    double                             moveScoreLossGamma   = 0.0;
    double                             moveScoreScale       = 24.0;
    double                             moveScoreBias        = 24.0;
    Score                              moveScoreMin         = -999;
    Score                              moveScoreMax         = 999;
    LossType                           lossType             = LossType::BCE;
    bool                               shuffleTuneEntries   = false;
    bool                               tuneEval             = true;
    bool                               tuneMoveScore        = false;
    bool                               randomMoveScoreInit  = false;

    // --------------------------------------------
    // Data entry filter settings

    bool     tuneRule[RULE_NB] = {};
    uint8_t  boardSizeMin      = 5;
    uint8_t  boardSizeMax      = MAX_BOARD_SIZE;
    uint16_t minPly            = 1;
    uint16_t minPlyBeforeFull  = 50;

    // --------------------------------------------
    // Scaling Factor searching settings

    bool   usePreviousScalingFactor = false;
    int    nIterations              = 10;
    int    nStepsPerIteration       = 10;
    double scalingFactorMin         = 100;
    double scalingFactorMax         = 400;
    size_t recomputeInterval        = 0;
};

/// TuningStatistic struct records all current statistic in tuning process.
/// This can be used to produce a training record for reporting.
struct TuningStatistic
{
    size_t currentEpoch;
    double valueLoss, policyLoss;
    double valueValLoss, policyValLoss;
    double elapsedSeconds;
    double scalingFactor;
};

/// Tuner runs the whole tuning process for the given dataset and tuning config.
/// Some actions in tuning process will be performed in parallel.
class Tuner
{
public:
    Tuner(class Dataset &trainDataset, class Dataset *valDataset, TuningConfig config = {});
    Tuner(const Tuner &) = delete;

    void run(size_t epochs, std::function<void(TuningStatistic)> callback = nullptr);
    void saveParams() const;
    size_t trainingSampleCount() const;
    size_t validationSampleCount() const;

private:
    static constexpr size_t LogicalPartitions = 64;

    const TuningConfig                config;
    PreparedCorpus                    trainTuneEntries, valTuneEntries;
    std::unique_ptr<FileBackedCorpus> trainFileCorpus, valFileCorpus;
    std::vector<uint32_t>             trainSampleOrder;
    std::vector<TuneParam>            tuneParams;
    std::vector<ParamsSyncRecord>     syncRecords;
    struct ParameterAddress
    {
        ParameterId baseIndex;
        uint32_t    parameterCount;
    };
    std::unordered_map<const void *, ParameterAddress> paramIndices;
    std::vector<std::vector<TuneGradient>>             partitionGradients;
    size_t                                             fileWorkerBudgetBytes = 0;
    size_t                                             fileJobBudgetBytes    = 0;
    size_t                                             fileShardBudgetBytes  = 0;
    size_t                                             fileShardTargetBytes  = 0;
    size_t                                             fileRecordLimitBytes  = 0;
    size_t                                             fileChunkEntryLimit   = 0;
    size_t                                             fileMaxPendingJobs    = 0;
    /// Worker pool for dataset transformation and loss/gradient computation.
    /// mutable: the const loss-computation methods submit tasks through it.
    mutable BS::thread_pool threadPool;

    struct CompileScratch
    {
        std::vector<TuneCoeff>       evalTerms;
        std::vector<PolicyCandidate> policyCandidates;
    };

    void             initParams();
    PreparedCacheKey makePreparedCacheKey(const std::vector<std::filesystem::path> &sourcePaths,
                                          const std::string                        &datasetFormat,
                                          const char                               *role) const;
    void             initTuneEntries(PreparedCorpus   &tuneEntries,
                                     FileBackedCorpus *fileCorpus,
                                     class Dataset    &dataset,
                                     bool              buildShuffleOrder);
    void             appendTuneSample(PreparedCorpus &tuneEntries,
                                      const Board    &board,
                                      Rule            rule,
                                      uint8_t         resultTimesTwo,
                                      Pos             bestMove,
                                      CompileScratch &scratch) const;
    Float            searchOptimalInvScalingFactor(bool useTunedEval) const;
    template <bool UseTunedEval>
    std::vector<Float> computeEvaluationLossGrid(const std::vector<Float> &candidates) const;
    template <bool UseTunedEval = true>
    Float                   computeEvaluationLoss(Float K, bool validation) const;
    Float                   computeMoveScoreLoss(bool validation) const;
    std::pair<Float, Float> computeLosses(Float K, bool validation) const;
    void                    computeGradientBatch(std::vector<TuneGradient>   &grads,
                                                 Float                        K,
                                                 const PreparedCorpus        &entries,
                                                 size_t                       batchBegin,
                                                 const std::vector<uint32_t> *sampleOrder);

    void        addParams(std::string   layoutTag,
                          void         *address,
                          size_t        numElems,
                          uint32_t      elemSize,
                          uint32_t      paramPerElem,
                          ParamGetter<> getter,
                          ParamSetter<> setter);
    ParameterId paramIndex(const void *address, size_t offset = 0) const;

    /* helper functions to add typed params to synced tune params */

    template <typename T, size_t ParamPerElem = 1>
    void addSingleParam(std::string            layoutTag,
                        T                     &param,
                        ParamGetter<const T &> getter,
                        ParamSetter<T &>       setter);
    template <typename T, size_t Length, size_t ParamPerElem = 1>
    void addArrayParams(std::string layoutTag,
                        T (&paramArray)[Length],
                        ParamGetter<const T &> getter,
                        ParamSetter<T &>       setter);
};

}  // namespace Tuning

template <typename T, size_t ParamPerElem>
inline void Tuning::Tuner::addSingleParam(std::string            layoutTag,
                                          T                     &param,
                                          ParamGetter<const T &> getter,
                                          ParamSetter<T &>       setter)
{
    addParams(
        std::move(layoutTag),
        &param,  // std::addressof() might be better
        1,
        sizeof(T),
        ParamPerElem,
        [getter = std::move(getter)](void *addr, size_t offset) -> TuneParam {
            return getter(*static_cast<const T *>(addr), offset);
        },
        [setter = std::move(setter)](void *addr, size_t offset, TuneParam param) -> void {
            setter(*static_cast<T *>(addr), offset, param);
        });
}

template <typename T, size_t Length, size_t ParamPerElem>
inline void Tuning::Tuner::addArrayParams(std::string layoutTag,
                                          T (&paramArray)[Length],
                                          ParamGetter<const T &> getter,
                                          ParamSetter<T &>       setter)
{
    addParams(
        std::move(layoutTag),
        paramArray,
        Length,
        sizeof(T),
        ParamPerElem,
        [getter = std::move(getter)](void *addr, size_t offset) -> TuneParam {
            return getter(*static_cast<const T *>(addr), offset);
        },
        [setter = std::move(setter)](void *addr, size_t offset, TuneParam param) -> void {
            setter(*static_cast<T *>(addr), offset, param);
        });
}
