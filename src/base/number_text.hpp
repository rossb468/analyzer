// Numbers to text and back, the way the Rust core did it.
//
// Shared by every module that reads or writes text: the model's file formats,
// calibration files and the command-line harness.
//
// Every text format here - the measurement header, the REW export, the filter
// exports, the settings file - prints numbers with Rust's `{}` and `{:.N}`, and
// files written by either implementation have to read in the other and, for the
// exports, come out byte for byte the same. printf and iostreams do not do that:
//
// - `{}` is the *shortest* decimal that reads back to the same value, and never
//   uses an exponent. 1e-7 prints as `0.0000001`, 1e21 as a one followed by
//   twenty-one zeros, 100.0 as `100`. %g prints `1e-07`, and %.17g prints noise.
// - `{:.N}` rounds the exact binary value, ties to even, so -6.25 at one place
//   is `-6.2`. printf does the same, but only when the locale says so about the
//   decimal point.
// - Parsing accepts what Rust's `str::parse` accepts - a leading `+`, `1.`,
//   `.5`, `inf`, `NaN` - and nothing else, so a hand-edited value is read the
//   same way in both.
//
// Formatting is built on std::to_chars, which is locale-independent and exact,
// and parsing on strtod, whose one locale dependency (the decimal point) is
// handled. std::from_chars for floating point would be the natural parser but
// is not in every libc++ this project builds against.

#pragma once

#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "base/numeric.hpp"

namespace analyzer::text {

// Radians to degrees, as Rust's `f64::to_degrees`.
//
// That multiplies by a constant rather than computing 180 / pi, which can land
// one ulp away. Phase is printed to four places, so it rarely shows, but the
// exports are meant to match digit for digit.
constexpr double to_degrees(double radians) noexcept {
    constexpr double kDegreesPerRadian = 57.2957795130823208767981548141051703;
    return radians * kDegreesPerRadian;
}

// Rust `{}` for a float: the shortest decimal that reads back exactly, in plain
// positional notation. NaN is `NaN`, infinities `inf` and `-inf`, negative zero
// `-0`.
template <std::floating_point Float>
std::string shortest(Float value);

// Rust `{:.N}`: exactly `places` digits after the point, rounding the exact
// binary value to nearest, ties to even. `places` must be at most 60.
template <std::floating_point Float>
std::string fixed(Float value, int places);

// Rust `{:+.N}`: as fixed(), with an explicit `+` on a non-negative value.
std::string signed_fixed(double value, int places);

// Pad on the left with spaces to `width`, as `{:>width}`, which is what Rust
// does for numbers by default. Wider text is left alone.
std::string pad_left(std::string_view text, std::size_t width);

// Pad on the right with spaces to `width`, as `{:<width}`.
std::string pad_right(std::string_view text, std::size_t width);

// Parse as Rust's `f64::from_str`: an optional sign, then digits with an
// optional fraction (either part may be empty, not both) and an optional
// exponent, or `inf`, `infinity` or `nan` in any case. No whitespace, no
// trailing text. Out-of-range values become infinity or zero, as in Rust.
std::optional<double> parse_f64(std::string_view text);

// As parse_f64, rounded once, directly to single precision. Rounding through
// double first can differ in the last place.
std::optional<float> parse_f32(std::string_view text);

// Parse as Rust's `u32::from_str`: an optional `+`, then decimal digits that
// fit.
std::optional<std::uint32_t> parse_u32(std::string_view text) noexcept;

// As parse_u32, for a size or count (`usize` in Rust).
std::optional<std::size_t> parse_usize(std::string_view text) noexcept;

// Parse as Rust's `bool::from_str`: exactly `true` or `false`.
std::optional<bool> parse_bool(std::string_view text) noexcept;

}  // namespace analyzer::text
