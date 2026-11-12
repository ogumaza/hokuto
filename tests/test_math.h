// SPDX-License-Identifier: MIT

// Comparisons of measured values with a tolerance. The tests of the synths, the imports and the measurements
// share them.

#pragma once

#include <cmath>

namespace hokuto::testing
{

// Whether `value` is within `tolerance` of `expected`.
inline bool Near(double value, double expected, double tolerance)
{
    return std::abs(value - expected) <= tolerance;
}

} // namespace hokuto::testing
