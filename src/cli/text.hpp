// Numbers to text and back, the way the Rust harness did it.
//
// The harness's output is a contract: its text is compared byte for byte with
// what the Rust CLI printed (tests/golden/), and files it writes are read by
// REW. Rust's `{}` and `{:.N}` are not what printf or iostreams do, so the
// formatting goes through these instead:
//
// - `{}` is the shortest decimal that reads back to the same value, in plain
//   positional notation: 48000.0 prints as `48000`, 0.1 as `0.1`.
// - `{:?}` on a float is the same with a `.0` kept on whole numbers: `1.0`.
// - `{:.N}` rounds the exact binary value, ties to even, which is what
//   std::to_chars (locale-independent, unlike printf) does.
// - Parsing accepts what Rust's `str::parse` accepts - a leading `+`, `1.`,
//   `.5`, `inf`, `NaN` - and nothing else, so a flag value means the same thing
//   to both implementations. Leading or trailing space is an error.
//
// model/numeric.hpp has the same helpers for the model's text formats, but it
// is internal to that module. These are the harness's own.

#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace analyzer::cli::text {

// Rust `{}` for a float. NaN is `NaN`, infinities `inf` and `-inf`.
template <std::floating_point Float>
std::string display(Float value);

// Rust `{:?}` for a float: as display(), with `.0` appended to a whole number.
template <std::floating_point Float>
std::string debug(Float value);

// Rust `{:.N}`: exactly `places` digits after the point. `places` must be at
// most 60.
template <std::floating_point Float>
std::string fixed(Float value, int places);

// Pad on the left with spaces to `width`, as `{:>width}`. Wider text is kept.
std::string pad_left(std::string_view text, std::size_t width);

// Pad on the right with spaces to `width`, as `{:<width}`.
std::string pad_right(std::string_view text, std::size_t width);

// Rust `f64::from_str`. Out-of-range values become infinity or zero.
std::optional<double> parse_f64(std::string_view text);

// Rust `f32::from_str`, rounded once, directly to single precision.
std::optional<float> parse_f32(std::string_view text);

// Rust `usize::from_str`: an optional `+`, then decimal digits that fit.
std::optional<std::size_t> parse_usize(std::string_view text) noexcept;

}  // namespace analyzer::cli::text
