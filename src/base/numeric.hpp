// Float to integer conversion that cannot be undefined behaviour.
//
// The core turns computed floats - a bin index, a tick count, a gate position,
// a sweep length - into integers, and the float can be anything: a huge value
// from a tiny bin spacing, a negative one from a frequency below the axis, an
// infinity, a NaN. static_cast from a float outside the target's range is
// undefined behaviour in C++, and the AddressSanitizer/UBSan build aborts on it.
// Rust's `as` is defined: it truncates toward zero, saturates at the target's
// limits, and maps NaN to 0. saturating_cast reproduces that exactly, so the
// port behaves the same on the inputs the Rust tests never tried.
//
// This is the one place that conversion is written. Each module used to grow
// its own, differing in small ways (clamping at a made-up 9e18, at a caller's
// limit, at zero only) - which is how two of them would eventually disagree.
// A caller that wants a smaller ceiling takes std::min of the result.

#pragma once

#include <concepts>
#include <limits>

namespace analyzer {

namespace detail {

// 2^bits as a floating-point value, computed by repeated doubling because
// std::ldexp is not constexpr before C++23. Exact: a power of two is always
// representable, for any bit count the integers here have.
template <std::floating_point F>
constexpr F power_of_two(int bits) noexcept {
    F result = 1;
    for (int i = 0; i < bits; ++i) {
        result *= 2;
    }
    return result;
}

}  // namespace detail

// Convert `value` to `To`, truncating toward zero, saturating at the limits of
// `To`, and mapping NaN to zero.
//
// Works from any floating-point type to any integer type other than bool, and
// is noexcept and constexpr.
template <std::integral To, std::floating_point From>
    requires(!std::same_as<To, bool>)
constexpr To saturating_cast(From value) noexcept {
    // `value != value` rather than std::isnan, which is not constexpr in C++20.
    if (value != value) {
        return 0;
    }
    // max() of a 32- or 64-bit integer is not representable as a float or a
    // double - it rounds up to the next power of two - so comparing against
    // static_cast<From>(max()) and using `>` would let that very power of two
    // through to the cast, which is undefined. The bound is built as the
    // smallest power of two above max() instead, and `>=` is the correct test:
    // anything below it truncates to a representable To.
    constexpr From kAbove = detail::power_of_two<From>(std::numeric_limits<To>::digits);
    // min() is 0 for unsigned types and -2^digits for signed ones, both exactly
    // representable. `<=` catches -0.5 for an unsigned target, which would
    // otherwise truncate to 0 anyway but is cheaper to settle here.
    constexpr From kBelow = static_cast<From>(std::numeric_limits<To>::min());
    if (value >= kAbove) {
        return std::numeric_limits<To>::max();
    }
    if (value <= kBelow) {
        return std::numeric_limits<To>::min();
    }
    return static_cast<To>(value);
}

}  // namespace analyzer
