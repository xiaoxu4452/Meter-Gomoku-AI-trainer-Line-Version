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
#include "vec.h"

#include <array>
#include <cstring>

/// Linear (affine) layer kernels with int32 accumulation: the detail::Affine
/// specializations pick a strategy from the layer dimensions, and the one-off
/// preprocess functions permute weights on load into the order the chosen
/// strategy consumes at inference time.
namespace Evaluation::simd {

namespace detail {

    // ------------------------------------------------------------------------
    // Affine transform operation (y = Ax + b) template
    template <int OutSize,
              int InSize,
              typename InType,
              int             Alignment,
              InstructionType I,
              typename Enabled = void>
    struct Affine
    {
        static_assert(always_false_v<std::integral_constant<int, OutSize>,
                                     std::integral_constant<int, InSize>,
                                     InType>,
                      "No valid implementation for this affine parameter");
    };

    template <int OutSize, int InSize, int Alignment, InstructionType I>
    struct Affine<
        OutSize,
        InSize,
        int8_t,
        Alignment,
        I,
        std::enable_if_t<(OutSize > 1 && VecBatch<OutSize, int32_t, I, true>::NumExtra == 0)>>
    {
        static constexpr int ChunkSize = 4;
        static constexpr int NumChunks = InSize / ChunkSize;
        static_assert(InSize % ChunkSize == 0, "InSize must be a multiple of ChunkSize=4");

        template <bool SignedInput, bool Bias, bool PreReLU, bool PostReLU>
        static void
        forward(int32_t *output, const int8_t *input, const int8_t *weight, const int32_t *bias)
        {
            typedef detail::VecLoadStore<int8_t, Alignment, I>  I8LS;
            typedef detail::VecLoadStore<int32_t, Alignment, I> I32LS;
            typedef detail::VecOp<int8_t, I>                    I8Op;
            typedef detail::VecOp<int32_t, I>                   I32Op;

            const auto input32 = reinterpret_cast<const int32_t *>(input);

            typedef VecBatch<OutSize, int32_t, I> OutB;
            typename I32Op::R                     acc[OutB::NumBatch];
            for (int j = 0; j < OutB::NumBatch; j++) {
                if constexpr (Bias)
                    acc[j] = I32LS::load(bias + j * OutB::RegWidth);
                else
                    acc[j] = I32Op::setzero();
            }

            for (int i = 0; i < NumChunks; i++) {
                auto in0 = typename I8Op::R(I32Op::set1(input32[i]));  // Broadcast input value
                auto w0 =
                    reinterpret_cast<const typename I8Op::R *>(weight + i * OutSize * ChunkSize);
                if constexpr (PreReLU)
                    in0 = I8Op::max(in0, I8Op::setzero());

                for (int j = 0; j < OutB::NumBatch; j++) {
                    if constexpr (SignedInput)
                        I8Op::dot4_i8i8_accum(acc[j], in0, I8LS::load(&w0[j]));
                    else
                        I8Op::dot4_u7i8_accum(acc[j], in0, I8LS::load(&w0[j]));
                }
            }

            for (int j = 0; j < OutB::NumBatch; j++) {
                if constexpr (PostReLU)
                    acc[j] = I32Op::max(acc[j], I32Op::setzero());
                I32LS::store(output + j * OutB::RegWidth, acc[j]);
            }
        }
    };

    template <int OutSize, int InSize, int Alignment, InstructionType I>
    struct Affine<
        OutSize,
        InSize,
        int8_t,
        Alignment,
        I,
        std::enable_if_t<!(OutSize > 1 && VecBatch<OutSize, int32_t, I, true>::NumExtra == 0)
                         && (OutSize >= 4 && OutSize % 4 == 0)
                         && (detail::VecBatch<InSize, int8_t, I, true>::NumExtra == 0)>>
    {
        template <bool SignedInput, bool Bias, bool PreReLU, bool PostReLU>
        static void
        forward(int32_t *output, const int8_t *input, const int8_t *weight, const int32_t *bias)
        {
            constexpr InstructionType I128 = getInstTypeOfWidth(I, 128);

            typedef detail::VecBatch<InSize, int8_t, I>            B;
            typedef detail::VecLoadStore<int8_t, Alignment, I>     I8LS;
            typedef detail::VecLoadStore<int32_t, Alignment, I128> I32LS128;
            typedef detail::VecOp<int8_t, I>                       I8Op;
            typedef detail::VecOp<int32_t, I>                      I32Op;
            typedef detail::VecOp<int32_t, I128>                   I32Op128;

            constexpr int OutNumBatches = OutSize / 4;
            for (int i = 0; i < OutNumBatches; i++) {
                // Prepare weight offsets. One offset for one row of weights.
                // This is a simple index into a 2d array.
                const int offset0 = (i * 4 + 0) * InSize;
                const int offset1 = (i * 4 + 1) * InSize;
                const int offset2 = (i * 4 + 2) * InSize;
                const int offset3 = (i * 4 + 3) * InSize;

                // Accumulation starts from 0, we add the bias only at the end.
                auto sum0 = I32Op::setzero();
                auto sum1 = I32Op::setzero();
                auto sum2 = I32Op::setzero();
                auto sum3 = I32Op::setzero();

                // Each innermost loop processes a 32x4 chunk of weights, so 128 weights at a time!
                for (int j = 0; j < B::NumBatch; j++) {
                    // We unroll by 4 so that we can reuse this value, reducing the number of
                    // memory operations required.
                    auto in = I8LS::load(input + j * B::RegWidth);
                    if constexpr (PreReLU)
                        in = I8Op::max(in, I32Op::setzero());

                    // Processes a 4Lx1 chunk of int8 and produces a Lx1 chunk of int32.
                    const auto w0 = I8LS::load(weight + offset0 + j * B::RegWidth);
                    const auto w1 = I8LS::load(weight + offset1 + j * B::RegWidth);
                    const auto w2 = I8LS::load(weight + offset2 + j * B::RegWidth);
                    const auto w3 = I8LS::load(weight + offset3 + j * B::RegWidth);
                    if constexpr (SignedInput) {
                        I8Op::dot4_i8i8_accum(sum0, in, w0);
                        I8Op::dot4_i8i8_accum(sum1, in, w1);
                        I8Op::dot4_i8i8_accum(sum2, in, w2);
                        I8Op::dot4_i8i8_accum(sum3, in, w3);
                    }
                    else {
                        I8Op::dot4_u7i8_accum(sum0, in, w0);
                        I8Op::dot4_u7i8_accum(sum1, in, w1);
                        I8Op::dot4_u7i8_accum(sum2, in, w2);
                        I8Op::dot4_u7i8_accum(sum3, in, w3);
                    }
                }

                // Adds horizontally L values from each sum together, producing 4 int32 values.
                auto outval = I32Op::hsum4(sum0, sum1, sum2, sum3);
                if constexpr (Bias)
                    outval = I32Op128::add(outval, I32LS128::load(bias + i * 4));
                if constexpr (PostReLU)
                    outval = I32Op128::max(outval, I32Op128::setzero());
                I32LS128::store(output + i * 4, outval);
            }
        }
    };

    template <int OutSize, int InSize, int Alignment, InstructionType I>
    struct Affine<
        OutSize,
        InSize,
        int8_t,
        Alignment,
        I,
        std::enable_if_t<!(OutSize > 1 && VecBatch<OutSize, int32_t, I, true>::NumExtra == 0)
                         && (OutSize >= 4 && OutSize % 4 == 0)
                         && (detail::VecBatch<InSize, int8_t, I, true>::NumExtra > 0)
                         && (detail::VecBatch<InSize, int8_t, I, true>::NumBatch == 0)>>
    {
        template <bool SignedInput, bool Bias, bool PreReLU, bool PostReLU>
        static void
        forward(int32_t *output, const int8_t *input, const int8_t *weight, const int32_t *bias)
        {
            if constexpr (!detail::VecBatch<InSize,
                                            int8_t,
                                            getInstTypeOfWidth(simd::NativeInstType, 256),
                                            true>::NumExtra) {
                Affine<OutSize,
                       InSize,
                       int8_t,
                       std::min(Alignment, 32),
                       getInstTypeOfWidth(simd::NativeInstType, 256)>::
                    template forward<SignedInput, Bias, PreReLU, PostReLU>(output,
                                                                           input,
                                                                           weight,
                                                                           bias);
            }
            else if constexpr (!detail::VecBatch<InSize,
                                                 int8_t,
                                                 getInstTypeOfWidth(simd::NativeInstType, 128),
                                                 true>::NumExtra) {
                Affine<OutSize,
                       InSize,
                       int8_t,
                       std::min(Alignment, 16),
                       getInstTypeOfWidth(simd::NativeInstType, 128)>::
                    template forward<SignedInput, Bias, PreReLU, PostReLU>(output,
                                                                           input,
                                                                           weight,
                                                                           bias);
            }
            else {
                static_assert(always_false_v<std::integral_constant<int, OutSize>,
                                             std::integral_constant<int, InSize>,
                                             std::integral_constant<int, Alignment>,
                                             std::integral_constant<InstructionType, I>>,
                              "No valid implementation for this affine parameter");
            }
        }
    };

    template <int OutSize, int InSize, int Alignment, InstructionType I>
    struct Affine<
        OutSize,
        InSize,
        int16_t,
        Alignment,
        I,
        std::enable_if_t<(OutSize > 1 && VecBatch<OutSize, int32_t, I, true>::NumExtra == 0)>>
    {
        static constexpr int ChunkSize = 2;
        static constexpr int NumChunks = InSize / ChunkSize;
        static_assert(InSize % ChunkSize == 0, "InSize must be a multiple of ChunkSize=2");

        template <bool SignedInput, bool Bias, bool PreReLU, bool PostReLU>
        static void
        forward(int32_t *output, const int16_t *input, const int16_t *weight, const int32_t *bias)
        {
            typedef detail::VecLoadStore<int16_t, Alignment, I> I16LS;
            typedef detail::VecLoadStore<int32_t, Alignment, I> I32LS;
            typedef detail::VecOp<int16_t, I>                   I16Op;
            typedef detail::VecOp<int32_t, I>                   I32Op;

            const auto input32 = reinterpret_cast<const int32_t *>(input);

            typedef VecBatch<OutSize, int32_t, I> OutB;
            typename I32Op::R                     acc[OutB::NumBatch];
            for (int j = 0; j < OutB::NumBatch; j++) {
                if constexpr (Bias)
                    acc[j] = I32LS::load(bias + j * OutB::RegWidth);
                else
                    acc[j] = I32Op::setzero();
            }

            for (int i = 0; i < NumChunks; i++) {
                auto in0 = typename I16Op::R(I32Op::set1(input32[i]));  // Broadcast input value
                auto w0 =
                    reinterpret_cast<const typename I16Op::R *>(weight + i * OutSize * ChunkSize);
                if constexpr (PreReLU)
                    in0 = I16Op::max(in0, I16Op::setzero());

                for (int j = 0; j < OutB::NumBatch; j++)
                    acc[j] = I32Op::add(acc[j], I16Op::dot2(in0, I16LS::load(&w0[j])));
            }

            for (int j = 0; j < OutB::NumBatch; j++) {
                if constexpr (PostReLU)
                    acc[j] = I32Op::max(acc[j], I32Op::setzero());
                I32LS::store(output + j * OutB::RegWidth, acc[j]);
            }
        }
    };

    template <class T, class = void>
    struct HasChunkSize : std::false_type
    {};

    template <class T>
    struct HasChunkSize<T, std::void_t<decltype(T::ChunkSize)>> : std::true_type
    {};

}  // namespace detail

/// Preprocess int8/int16 linear layer with int32 accumulation.
template <int             OutSize,
          int             InSize,
          int             Alignment = NativeAlignment,
          InstructionType Inst      = NativeInstType,
          typename InputType        = int8_t>
void preprocessLinear(InputType weight[OutSize * InSize])
{
    static_assert(std::is_same_v<InputType, int8_t> || std::is_same_v<InputType, int16_t>,
                  "Only int8_t or int16_t weight is supported");

    typedef detail::Affine<OutSize, InSize, InputType, Alignment, Inst> Affine;
    if constexpr (detail::HasChunkSize<Affine>::value) {
        constexpr int ChunkSize = Affine::ChunkSize;

        InputType weightScrambled[OutSize * InSize];
        for (int i = 0; i < OutSize * InSize; i++) {
            int offset             = i % ChunkSize;
            int idxChunk           = i / ChunkSize;
            int colChunk           = idxChunk % (InSize / ChunkSize);
            int rowChunk           = i / InSize;
            int transposedIdxChunk = colChunk * OutSize + rowChunk;

            weightScrambled[transposedIdxChunk * ChunkSize + offset] = weight[i];
        }

        std::memcpy(weight, weightScrambled, sizeof(InputType) * OutSize * InSize);
    }
}

/// Preprocess int8/int16 hyper linear layer used for computing dynamic linear weight.
template <int DynamicOutSize,
          int DynamicInSize,
          typename DynamicWeightType,
          int             HyperInSize,
          int             DynamicWeightOffset,
          int             Alignment = NativeAlignment,
          InstructionType Inst      = NativeInstType,
          typename WeightType       = int8_t,
          typename BiasType         = int32_t>
void preprocessDynamicWeightLinear(WeightType *weight, BiasType *bias)
{
    typedef detail::Affine<DynamicOutSize, DynamicInSize, DynamicWeightType, Alignment, Inst>
        DynamicAffine;
    if constexpr (detail::HasChunkSize<DynamicAffine>::value) {
        typedef std::array<WeightType, HyperInSize> Row;

        constexpr int ChunkSize = DynamicAffine::ChunkSize;
        Row           rowScrambled[DynamicOutSize * DynamicInSize];
        for (int i = 0; i < DynamicOutSize * DynamicInSize; i++) {
            int offset             = i % ChunkSize;
            int idxChunk           = i / ChunkSize;
            int colChunk           = idxChunk % (DynamicInSize / ChunkSize);
            int rowChunk           = i / DynamicInSize;
            int transposedIdxChunk = colChunk * DynamicOutSize + rowChunk;

            rowScrambled[transposedIdxChunk * ChunkSize + offset] =
                *reinterpret_cast<Row *>(weight + (i + DynamicWeightOffset) * HyperInSize);
        }

        for (int i = 0; i < DynamicOutSize * DynamicInSize; i++)
            *reinterpret_cast<Row *>(weight + (i + DynamicWeightOffset) * HyperInSize) =
                rowScrambled[i];

        if (bias) {
            BiasType biasScrambled[DynamicOutSize * DynamicInSize];
            for (int i = 0; i < DynamicOutSize * DynamicInSize; i++) {
                int offset             = i % ChunkSize;
                int idxChunk           = i / ChunkSize;
                int colChunk           = idxChunk % (DynamicInSize / ChunkSize);
                int rowChunk           = i / DynamicInSize;
                int transposedIdxChunk = colChunk * DynamicOutSize + rowChunk;

                biasScrambled[transposedIdxChunk * ChunkSize + offset] =
                    bias[i + DynamicWeightOffset];
            }

            for (int i = 0; i < DynamicOutSize * DynamicInSize; i++)
                bias[i + DynamicWeightOffset] = biasScrambled[i];
        }
    }
}

/// Apply int8/int16 linear layer with int32 accumulation.
template <int             OutSize,
          int             InSize,
          bool            SignedInput = false,
          bool            Bias        = true,
          bool            PreReLU     = false,
          bool            PostReLU    = false,
          int             Alignment   = NativeAlignment,
          InstructionType Inst        = NativeInstType,
          typename AccType            = int32_t,
          typename InputType          = int8_t>
FORCE_INLINE AccType *linear(AccType         *output,
                             const InputType *input,
                             const InputType  weight[OutSize * InSize],
                             const AccType    bias[OutSize])
{
    static_assert(std::is_same_v<AccType, int32_t>, "Only int32_t accumulator is supported");
    static_assert(std::is_same_v<InputType, int8_t> || std::is_same_v<InputType, int16_t>,
                  "Only int8_t or int16_t input is supported");
    static_assert(isAlignSizeOK(Alignment));
    assert(isPtrAligned<Alignment>(output));
    assert(isPtrAligned<Alignment>(input));
    assert(isPtrAligned<Alignment>(weight));
    if constexpr (Bias)
        assert(isPtrAligned<Alignment>(bias));

    typedef detail::Affine<OutSize, InSize, InputType, Alignment, Inst> Affine;
    Affine::template forward<SignedInput, Bias, PreReLU, PostReLU>(output, input, weight, bias);

    return output + OutSize;
}

}  // namespace Evaluation::simd
