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
#include "../../core/utils.h"

#include <algorithm>
#include <cstdint>

/// Instruction-set selection and alignment helpers for the SIMD kernels.
/// The instruction set is chosen at compile time only: NativeInstType/NativeAlignment
/// reflect the USE_* macros this binary was built with, and kernels are templated on
/// InstructionType so narrower sets can be picked for small data (getInstTypeOfWidth).
namespace Evaluation::simd {

enum InstructionType {
    SCALAR,
    SSE,
    AVX2,
    AVX512,
    NEON,
    WASM_SIMD,
};

/// Get simd register width of the given instruction type.
constexpr size_t simdBitsOfInstType(InstructionType instType)
{
    switch (instType) {
    default: return 128;
    case SSE: return 128;
    case AVX2: return 256;
    case AVX512: return 512;
    case NEON: return 128;
    case WASM_SIMD: return 128;
    }
}

/// Check if the given instruction type is the minimal length on this platform.
constexpr bool isMinimalInstType(InstructionType instType)
{
    switch (instType) {
    default: return true;
    case SSE: return true;
    case AVX2: return false;
    case AVX512: return false;
    case NEON: return true;
    case WASM_SIMD: return true;
    }
}

/// Returns the widest instruction type not exceeding the given register width.
constexpr InstructionType getInstTypeOfWidth(InstructionType instType, size_t width)
{
    return simdBitsOfInstType(instType) <= width ? instType
           : isMinimalInstType(instType)
               ? SCALAR
               : getInstTypeOfWidth(static_cast<InstructionType>(instType - 1), width);
}

#if defined(USE_AVX512)
constexpr size_t          NativeAlignment = 64;
constexpr InstructionType NativeInstType  = AVX512;
#elif defined(USE_AVX2)
constexpr size_t          NativeAlignment = 32;
constexpr InstructionType NativeInstType  = AVX2;
#elif defined(USE_SSE)
constexpr size_t          NativeAlignment = 16;
constexpr InstructionType NativeInstType  = SSE;
#elif defined(USE_NEON)
constexpr size_t          NativeAlignment = 16;
constexpr InstructionType NativeInstType  = NEON;
#elif defined(USE_WASM_SIMD)
constexpr size_t          NativeAlignment = 16;
constexpr InstructionType NativeInstType  = WASM_SIMD;
#else  // Delegate to SSE with simde's implementation
constexpr size_t          NativeAlignment = 16;
constexpr InstructionType NativeInstType  = SCALAR;
#endif

constexpr bool isAlignSizeOK(size_t alignSize)
{
    return alignSize > 0 && alignSize <= 64 && isPowerOfTwo(alignSize);
}

template <size_t AlignSize, typename T>
constexpr bool isPtrAligned(const T *pointer)
{
    static_assert(isAlignSizeOK(AlignSize), "AlignSize is not valid");
    return (reinterpret_cast<uintptr_t>(pointer) & (AlignSize - 1)) == 0;
}

}  // namespace Evaluation::simd
