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

#include "optimizer.h"

#include <cassert>
#include <cmath>
#include <stdexcept>

namespace Tuning {

template <typename T>
AdamOptimizer<T>::AdamOptimizer(std::size_t numParams,
                                T           lr,
                                T           weightDecay,
                                T           beta1,
                                T           beta2,
                                T           epsilon)
    : lr(lr)
    , weightDecay(weightDecay)
    , beta1(beta1)
    , beta2(beta2)
    , epsilon(epsilon)
    , stepCount(0)
{
    m.resize(numParams);
    v.resize(numParams);
    nextParams.resize(numParams);
}

template <typename T>
void AdamOptimizer<T>::step(std::vector<T> &params, const std::vector<T> &gradients)
{
    const size_t numParams = params.size();
    assert(numParams == m.size());
    assert(numParams == gradients.size());

    stepCount++;

    // Bias correction is constant for every parameter in this step. Computing
    // these powers inside the parameter loop is particularly expensive for the
    // large policy table, and some compilers do not hoist std::pow themselves.
    const double biasCorrection1 = 1.0 - std::pow(double(beta1), double(stepCount));
    const double biasCorrection2 = 1.0 - std::pow(double(beta2), double(stepCount));

    for (size_t i = 0; i < numParams; i++) {
        m[i] = beta1 * m[i] + (T(1.0) - beta1) * gradients[i];
        v[i] = beta2 * v[i] + (T(1.0) - beta2) * gradients[i] * gradients[i];

        T m_corr = T(double(m[i]) / biasCorrection1);
        T v_corr = T(double(v[i]) / biasCorrection2);

        T nextParam =
            params[i] - lr * (m_corr / (std::sqrt(v_corr) + epsilon) + weightDecay * params[i]);
        if (!std::isfinite(m[i]) || !std::isfinite(v[i]) || !std::isfinite(nextParam))
            throw std::runtime_error("Adam produced a non-finite optimizer state");
        nextParams[i] = nextParam;
    }

    // Publish the complete update only after every candidate parameter passed
    // validation. A failed step cannot expose a partially updated vector.
    params.swap(nextParams);
}

}  // namespace Tuning

template class Tuning::AdamOptimizer<float>;
template class Tuning::AdamOptimizer<double>;
