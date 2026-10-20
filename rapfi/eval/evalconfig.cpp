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

#include "evalconfig.h"

#include "../config.h"           // GeneralCfg.messageMode gates the load messages
#include "../core/filesystem.h"  // pathToConsoleString (iohelper.h does not provide it)
#include "../core/iohelper.h"
#include "evaluator.h"
#include "mix10nnue.h"
#include "mix9svqnnue.h"

#ifdef USE_ORT_EVALUATOR
    #include "onnxevaluator.h"
#endif

namespace Evaluation {

EvaluatorConfig EvalCfg;

namespace {

using std::filesystem::path;
using std::filesystem::u8path;

/// Wraps a per-arch construction lambda with the shared weight-entry
/// iteration, lazy path resolution, and error policy (verbatim behavior of
/// the old config.cpp warpEvaluatorMaker).
template <typename ArchMaker>
EvaluatorMakerFunc wrapEvaluatorMaker(EvaluatorWeightsConfig cfg,
                                      PathResolver           resolvePath,
                                      ArchMaker              maker,
                                      bool                   separateBlackAndWhiteWeights)
{
    return [cfg = std::move(cfg), resolvePath = std::move(resolvePath), maker,
            separateBlackAndWhiteWeights](int              boardSize,
                                          Rule             rule,
                                          Numa::NumaNodeId numaId) -> std::unique_ptr<Evaluator> {
        try {
            for (const auto &weightCfg : cfg.weights) {
                path weightPath;
                path blackWeightPath, whiteWeightPath;

                if (weightCfg.file)
                    weightPath = resolvePath(u8path(*weightCfg.file));
                else if (!separateBlackAndWhiteWeights)
                    throw std::runtime_error("must specify weight_file in weight configs.");

                if (separateBlackAndWhiteWeights && weightPath.empty()) {
                    // Deliberate resolve-before-presence-check, replicating the
                    // old cpptoml dereference order (disengaged -> empty string).
                    blackWeightPath = resolvePath(u8path(weightCfg.fileBlack.value_or("")));
                    whiteWeightPath = resolvePath(u8path(weightCfg.fileWhite.value_or("")));
                    if (!weightCfg.fileBlack || !weightCfg.fileWhite)
                        throw std::runtime_error("must specify weight_file or weight_file_black "
                                                 "and weight_file_white in weight configs.");
                }
                else {
                    blackWeightPath = weightPath;
                    whiteWeightPath = weightPath;
                }

                try {
                    return maker(boardSize,
                                 rule,
                                 numaId,
                                 weightPath,
                                 std::make_pair(blackWeightPath, whiteWeightPath));
                }
                catch (const UnsupportedRuleError &e) {
                }
                catch (const UnsupportedBoardSizeError &e) {
                }
                catch (const std::exception &e) {
                    if (Config::GeneralCfg.messageMode != MsgMode::NONE)
                        MESSAGEL("Failed to load from "
                                 << (!weightPath.empty()
                                         ? pathToConsoleString(weightPath)
                                         : pathToConsoleString(blackWeightPath) + " and "
                                               + pathToConsoleString(whiteWeightPath))
                                 << " due to error: " << e.what());
                }
            }

            if (Config::GeneralCfg.messageMode != MsgMode::NONE)
                MESSAGEL("Evaluator " << cfg.type
                                      << " disabled: no compatible weight config found.");
            return nullptr;
        }
        catch (const std::exception &e) {
            ERRORL("Evaluator " << cfg.type << " failed to initialized: " << e.what());
            return nullptr;
        }
    };
}

}  // namespace

EvaluatorMakerFunc makeEvaluatorMaker(EvaluatorWeightsConfig cfg, PathResolver resolvePath)
{
    if (cfg.type == "mix9svq") {
        return wrapEvaluatorMaker(
            std::move(cfg),
            std::move(resolvePath),
            [](int boardSize,
               Rule rule,
               Numa::NumaNodeId numaId,
               const path &weightPath,
               std::pair<path, path> blackAndWhiteWeightPath) {
                return std::make_unique<mix9svq::Evaluator>(boardSize,
                                                            rule,
                                                            numaId,
                                                            blackAndWhiteWeightPath.first,
                                                            blackAndWhiteWeightPath.second);
            },
            true);
    }
    else if (cfg.type == "mix10") {
        return wrapEvaluatorMaker(
            std::move(cfg),
            std::move(resolvePath),
            [](int boardSize,
               Rule rule,
               Numa::NumaNodeId numaId,
               const path &weightPath,
               std::pair<path, path> blackAndWhiteWeightPath) {
                return std::make_unique<mix10::Evaluator>(boardSize,
                                                          rule,
                                                          numaId,
                                                          blackAndWhiteWeightPath.first,
                                                          blackAndWhiteWeightPath.second);
            },
            true);
    }
#ifdef USE_ORT_EVALUATOR
    else if (cfg.type == "ort") {
        std::string deviceName = cfg.ortDevice;
        return wrapEvaluatorMaker(
            std::move(cfg),
            std::move(resolvePath),
            [deviceName](int boardSize,
                         Rule rule,
                         Numa::NumaNodeId numaId,
                         const path &weightPath,
                         std::pair<path, path>) {
                return std::make_unique<onnx::OnnxEvaluator>(boardSize,
                                                             rule,
                                                             weightPath,
                                                             deviceName);
            },
            false);
    }
#endif
    else {
        throw std::runtime_error("unsupported evaluator type " + cfg.type);
    }
}

}  // namespace Evaluation
