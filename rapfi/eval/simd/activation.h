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

#include <cassert>

/// Element-wise array kernels: zero/copy/add, clipped-relu quantization (crelu)
/// and pairwise dot product (dot2).
namespace Evaluation::simd {

/// Set an array to zeros. Return the end pointer of the output array.
template <int Size,
          typename T,
          int             Alignment = NativeAlignment,
          InstructionType Inst      = NativeInstType>
T *zero(T *output)
{
    static_assert(std::is_integral_v<T> || std::is_same_v<T, float>);
    static_assert(isAlignSizeOK(Alignment));
    assert(isPtrAligned<Alignment>(output));

    typedef detail::VecBatch<Size, T, Inst>          B;
    typedef detail::VecLoadStore<T, Alignment, Inst> LS;
    typedef detail::VecOp<T, Inst>                   Op;

    auto zero = Op::setzero();
    for (int i = 0; i < B::NumBatch; i++)
        LS::store(output + i * B::RegWidth, zero);

    return output + B::NumBatch * B::RegWidth;
}

/// Copy an array from input to output. Return the end pointer of the output array.
template <int Size,
          typename T,
          int             Alignment = NativeAlignment,
          InstructionType Inst      = NativeInstType>
T *copy(T *output, const T *input)
{
    static_assert(std::is_integral_v<T> || std::is_same_v<T, float>);
    static_assert(isAlignSizeOK(Alignment));
    assert(isPtrAligned<Alignment>(output));
    assert(isPtrAligned<Alignment>(input));

    typedef detail::VecBatch<Size, T, Inst>          B;
    typedef detail::VecLoadStore<T, Alignment, Inst> LS;
    for (int i = 0; i < B::NumBatch; i++) {
        auto data = LS::load(input + i * B::RegWidth);
        LS::store(output + i * B::RegWidth, data);
    }

    return output + B::NumBatch * B::RegWidth;
}

template <int Size,
          typename T,
          int             Alignment = NativeAlignment,
          InstructionType Inst      = NativeInstType>
T *add(T *output, const T *input0, const T *input1)
{
    static_assert(std::is_integral_v<T> || std::is_same_v<T, float>);
    static_assert(isAlignSizeOK(Alignment));
    assert(isPtrAligned<Alignment>(output));
    assert(isPtrAligned<Alignment>(input0));
    assert(isPtrAligned<Alignment>(input1));

    typedef detail::VecBatch<Size, T, Inst>          B;
    typedef detail::VecLoadStore<T, Alignment, Inst> LS;
    typedef detail::VecOp<T, Inst>                   Op;

    for (int i = 0; i < B::NumBatch; i++) {
        auto data0 = LS::load(input0 + i * B::RegWidth);
        auto data1 = LS::load(input1 + i * B::RegWidth);
        data0      = Op::add(data0, data1);
        LS::store(output + i * B::RegWidth, data0);
    }

    return output + B::NumBatch * B::RegWidth;
}

/// Divide an int32 array by a 2-exp divisor, then apply clipped relu to the int32
/// array and store the saturated int8 results.
template <int             Size,
          int             Divisor,
          bool            NoReLU    = false,
          int             Alignment = NativeAlignment,
          InstructionType Inst      = NativeInstType>
FORCE_INLINE int8_t *crelu(int8_t output[Size], const int32_t input[Size])
{
    static_assert(isAlignSizeOK(Alignment));
    assert(isPtrAligned<Alignment>(output));
    assert(isPtrAligned<Alignment>(input));
    static_assert(isPowerOfTwo(Divisor), "divisor must be a power of two");
    constexpr int Log2Divisor = floorLog2(Divisor);

    typedef detail::VecBatch<Size, int32_t, Inst>          InB;
    typedef detail::VecBatch<Size, int8_t, Inst>           OutB;
    typedef detail::VecLoadStore<int32_t, Alignment, Inst> I32LS;
    typedef detail::VecLoadStore<int8_t, Alignment, Inst>  I8LS;
    typedef detail::VecPack<int32_t, int16_t, Inst>        I32Pack;
    typedef detail::VecPack<int16_t, int8_t, Inst>         I16Pack;
    typedef detail::VecOp<int16_t, Inst>                   I16Op;
    typedef detail::VecOp<int8_t, Inst>                    I8Op;

    const auto zero = I8Op::setzero();

    for (int i = 0; i < OutB::NumBatch; i++) {
        auto in0  = I32LS::load(input + (i * 4 + 0) * InB::RegWidth);
        auto in1  = I32LS::load(input + (i * 4 + 1) * InB::RegWidth);
        auto in2  = I32LS::load(input + (i * 4 + 2) * InB::RegWidth);
        auto in3  = I32LS::load(input + (i * 4 + 3) * InB::RegWidth);
        auto in01 = I32Pack::packs(in0, in1);
        auto in23 = I32Pack::packs(in2, in3);
        if constexpr (Log2Divisor > 0) {
            in01 = I16Op::template srai<Log2Divisor>(in01);
            in23 = I16Op::template srai<Log2Divisor>(in23);
        }
        auto result = I16Pack::packs(in01, in23);
        if constexpr (!NoReLU)
            result = I8Op::max(result, zero);

        // Permute values in different lanes if required.
        if constexpr (Inst == AVX2) {
            const auto control = simde_mm256_set_epi32(7, 3, 6, 2, 5, 1, 4, 0);
            result             = simde_mm256_permutevar8x32_epi32(result, control);
        }
#ifdef USE_AVX512
        else if constexpr (Inst == AVX512) {
            const auto control =
                _mm512_set_epi32(15, 11, 7, 3, 14, 10, 6, 2, 13, 9, 5, 1, 12, 8, 4, 0);
            result = _mm512_permutexvar_epi32(control, result);
        }
#endif

        I8LS::store(output + i * OutB::RegWidth, result);
    }

    return output + Size;
}

/// Divide an int32 array by a 2-exp divisor, then apply clipped relu to the int32
/// array and store the saturated int16 results.
template <int             Size,
          int             Divisor,
          bool            NoReLU    = false,
          int             Alignment = NativeAlignment,
          InstructionType Inst      = NativeInstType>
FORCE_INLINE int16_t *crelu(int16_t output[Size], const int32_t input[Size])
{
    static_assert(isAlignSizeOK(Alignment));
    assert(isPtrAligned<Alignment>(output));
    assert(isPtrAligned<Alignment>(input));
    static_assert(isPowerOfTwo(Divisor), "divisor must be a power of two");
    constexpr int Log2Divisor = floorLog2(Divisor);

    typedef detail::VecBatch<Size, int32_t, Inst>          InB;
    typedef detail::VecBatch<Size, int16_t, Inst>          OutB;
    typedef detail::VecLoadStore<int32_t, Alignment, Inst> I32LS;
    typedef detail::VecLoadStore<int16_t, Alignment, Inst> I16LS;
    typedef detail::VecPack<int32_t, int16_t, Inst>        I32Pack;
    typedef detail::VecOp<int32_t, Inst>                   I32Op;
    typedef detail::VecOp<int16_t, Inst>                   I16Op;

    const auto zero = I16Op::setzero();

    for (int i = 0; i < OutB::NumBatch; i++) {
        auto in0 = I32LS::load(input + (i * 2 + 0) * InB::RegWidth);
        auto in1 = I32LS::load(input + (i * 2 + 1) * InB::RegWidth);
        if constexpr (Log2Divisor > 0) {
            in0 = I32Op::template srai<Log2Divisor>(in0);
            in1 = I32Op::template srai<Log2Divisor>(in1);
        }
        auto result = I32Pack::packs(in0, in1);
        if constexpr (!NoReLU)
            result = I16Op::max(result, zero);

        // Permute values in different lanes if required.
        if constexpr (Inst == AVX2) {
            const auto control = SIMDE_MM_SHUFFLE(3, 1, 2, 0);
            result             = simde_mm256_permute4x64_epi64(result, control);
        }
#ifdef USE_AVX512
        else if constexpr (Inst == AVX512) {
            const auto control = _mm512_set_epi64(7, 5, 3, 1, 6, 4, 2, 0);
            result             = _mm512_permutexvar_epi64(control, result);
        }
#endif

        I16LS::store(output + i * OutB::RegWidth, result);
    }

    return output + Size;
}

/// Compute the pairwise dot product of u7 and i8 array and store the int8 results.
template <int             OutSize,
          int             Divisor,
          int             Alignment = NativeAlignment,
          InstructionType Inst      = NativeInstType>
FORCE_INLINE int8_t *dot2(int8_t output[OutSize], int8_t input_u7[OutSize * 2], int8_t input_i8[OutSize * 2])
{
    static_assert(isPowerOfTwo(Divisor), "divisor must be a power of two");
    constexpr int Log2Divisor = floorLog2(Divisor);

    typedef detail::VecBatch<OutSize, int8_t, Inst, true> OutB;
    typedef detail::VecLoadStore<int8_t, Alignment, Inst> I8LS;
    typedef detail::VecOp<int8_t, Inst>                   I8Op;
    typedef detail::VecOp<int16_t, Inst>                  I16Op;
    typedef detail::VecPack<int16_t, int8_t, Inst>        I16Pack;

    for (int i = 0; i < OutB::NumBatch; i++) {
        auto in10 = I8LS::load(input_u7 + (2 * i + 0) * OutB::RegWidth);  // unsigned
        auto in11 = I8LS::load(input_u7 + (2 * i + 1) * OutB::RegWidth);  // unsigned
        auto in20 = I8LS::load(input_i8 + (2 * i + 0) * OutB::RegWidth);  // signed
        auto in21 = I8LS::load(input_i8 + (2 * i + 1) * OutB::RegWidth);  // signed

        auto dotsum0i16 = I8Op::dot2_u7i8(in10, in20);
        auto dotsum1i16 = I8Op::dot2_u7i8(in11, in21);
        dotsum0i16      = I16Op::template srai<Log2Divisor>(dotsum0i16);
        dotsum1i16      = I16Op::template srai<Log2Divisor>(dotsum1i16);
        auto dotsumi8   = I16Pack::packs_permuted(dotsum0i16, dotsum1i16);

        I8LS::store(output + i * OutB::RegWidth, dotsumi8);
    }

    if constexpr (OutB::NumExtra > 0) {
        constexpr InstructionType I128 = getInstTypeOfWidth(Inst, 128);

        typedef detail::VecBatch<OutB::NumExtra, int8_t, I128> OutBExtra;
        typedef detail::VecLoadStore<int8_t, Alignment, I128>  I8LS128;
        typedef detail::VecOp<int8_t, I128>                    I8Op128;
        typedef detail::VecOp<int16_t, I128>                   I16Op128;
        typedef detail::VecPack<int16_t, int8_t, I128>         I16Pack128;

        for (int i = 0; i < OutBExtra::NumBatch; i++) {
            auto in10 = I8LS128::load(input_u7 + 2 * OutB::BatchedSize
                                      + (2 * i + 0) * OutBExtra::RegWidth);  // unsigned
            auto in11 = I8LS128::load(input_u7 + 2 * OutB::BatchedSize
                                      + (2 * i + 1) * OutBExtra::RegWidth);  // unsigned
            auto in20 = I8LS128::load(input_i8 + 2 * OutB::BatchedSize
                                      + (2 * i + 0) * OutBExtra::RegWidth);  // signed
            auto in21 = I8LS128::load(input_i8 + 2 * OutB::BatchedSize
                                      + (2 * i + 1) * OutBExtra::RegWidth);  // signed

            auto dotsum0i16 = I8Op128::dot2_u7i8(in10, in20);
            auto dotsum1i16 = I8Op128::dot2_u7i8(in11, in21);
            dotsum0i16      = I16Op128::template srai<Log2Divisor>(dotsum0i16);
            dotsum1i16      = I16Op128::template srai<Log2Divisor>(dotsum1i16);
            auto dotsumi8   = I16Pack128::packs_permuted(dotsum0i16, dotsum1i16);

            I8LS128::store(output + OutB::BatchedSize + i * OutBExtra::RegWidth, dotsumi8);
        }
    }

    return output + OutSize;
}

}  // namespace Evaluation::simd
