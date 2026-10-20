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

/// Umbrella header for the SIMD abstraction layer (namespace Evaluation::simd).
/// The implementation is split by concern:
///   simd/isa.h        - instruction-set selection and alignment helpers
///   simd/vec.h        - register-level primitives (detail::VecBatch/LoadStore/Cvt/Pack/Op)
///   simd/linear.h     - affine layer kernels and weight preprocessing
///   simd/activation.h - element-wise kernels (zero/copy/add, crelu, dot2)
#include "simd/activation.h"
#include "simd/isa.h"
#include "simd/linear.h"
#include "simd/vec.h"
