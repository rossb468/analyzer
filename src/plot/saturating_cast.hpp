// Float to integer conversion that cannot be undefined behaviour.
//
// Plot code turns computed floats - a bin index, a tick count - into integers,
// and the float can be anything: a huge value from a tiny bin spacing, a
// negative one from a frequency below the axis, a NaN. static_cast from a float
// outside the target's range is undefined behaviour in C++. Rust's `as` is
// defined: it truncates toward zero, saturates at the type's limits, and maps
// NaN to 0. This reproduces that, so the port behaves the same on the inputs
// the Rust tests never tried.
//
// Internal to this module; not part of its interface.

#pragma once

#include <cmath>
#include <concepts>
#include <limits>

namespace analyzer::plot {

template <std::integral Int>
constexpr Int saturating_cast(float value) noexcept {
    if (std::isnan(value)) {
        return 0;
    }
    // For 32- and 64-bit Int, max() is not representable as a float and rounds
    // up to a power of two, so `>=` (not `>`) is the correct test: anything
    // below that bound truncates to a representable Int.
    constexpr auto kMin = static_cast<float>(std::numeric_limits<Int>::min());
    constexpr auto kMax = static_cast<float>(std::numeric_limits<Int>::max());
    if (value <= kMin) {
        return std::numeric_limits<Int>::min();
    }
    if (value >= kMax) {
        return std::numeric_limits<Int>::max();
    }
    return static_cast<Int>(value);
}

}  // namespace analyzer::plot
