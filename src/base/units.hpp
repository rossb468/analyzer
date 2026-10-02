// Decibels and degrees: the unit conversions the whole core shares.
//
// Decibels come in two flavours and mixing them up is a 2x error in the answer.
// An amplitude (a voltage, a pressure, a sample value) is converted with 20 *
// log10, because power goes as the square of amplitude. A power (a mean square,
// a bin of a power spectrum) is converted with 10 * log10. Every conversion in
// the core goes through one of the four functions below, so which one a call
// site meant is visible in its name.
//
// The floored overloads are for display and storage. log10(0) is negative
// infinity, which no plot axis or text file wants, so a silent input is given a
// finite floor instead. A NaN input also reads as the floor: the test is
// `value > 0`, which NaN fails.

#pragma once

#include <algorithm>
#include <cmath>
#include <concepts>
#include <numbers>

namespace analyzer {

// 20 * log10(amplitude). Negative infinity for zero, NaN for a negative input.
template <std::floating_point Float>
Float amplitude_to_db(Float amplitude) noexcept {
    return Float{20} * std::log10(amplitude);
}

// 10 * log10(power). Negative infinity for zero, NaN for a negative input.
template <std::floating_point Float>
Float power_to_db(Float power) noexcept {
    return Float{10} * std::log10(power);
}

// As amplitude_to_db(), but never below `floor_db`: a zero, negative or NaN
// amplitude gives `floor_db` itself.
template <std::floating_point Float>
Float amplitude_to_db(Float amplitude, Float floor_db) noexcept {
    return amplitude > Float{0} ? std::max(amplitude_to_db(amplitude), floor_db) : floor_db;
}

// As power_to_db(), but never below `floor_db`: a zero, negative or NaN power
// gives `floor_db` itself.
template <std::floating_point Float>
Float power_to_db(Float power, Float floor_db) noexcept {
    return power > Float{0} ? std::max(power_to_db(power), floor_db) : floor_db;
}

// The amplitude ratio that `db` decibels stands for: 10^(db / 20).
template <std::floating_point Float>
Float db_to_amplitude(Float db) noexcept {
    return std::pow(Float{10}, db / Float{20});
}

// The power ratio that `db` decibels stands for: 10^(db / 10).
template <std::floating_point Float>
Float db_to_power(Float db) noexcept {
    return std::pow(Float{10}, db / Float{10});
}

// Multiply a phase in radians by this to get degrees.
template <std::floating_point Float>
inline constexpr Float kDegreesPerRadian = Float{180} / std::numbers::pi_v<Float>;

// Multiply a phase in degrees by this to get radians.
template <std::floating_point Float>
inline constexpr Float kRadiansPerDegree = std::numbers::pi_v<Float> / Float{180};

// A full turn in radians, 2 * pi.
template <std::floating_point Float>
inline constexpr Float kTau = Float{2} * std::numbers::pi_v<Float>;

}  // namespace analyzer
