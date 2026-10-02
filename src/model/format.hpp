// The on-disk measurement container.
//
// A text header followed by raw little-endian f64. Both halves are chosen
// deliberately.
//
// The header is plain text, which means `head -20 measurement.anlz` tells you
// what a file is without any tooling, and an unknown key is ignored rather than
// fatal - so a file written by a newer version still loads. It also means zero
// serialisation dependencies for something that has to stay readable for years.
//
// The data is raw little-endian f64, contiguous after a known offset, which
// makes it memory-mappable. A long impulse response is megabytes, and a format
// that requires parsing every value to reach the end is a format that gets slow
// exactly when measurements get interesting.
//
//   ANLZ1
//   name: Living room, left
//   sample_rate: 48000
//   kind: impulse_response
//   points: 65536
//   time_zero_samples: 1234.5
//   spl_offset_db: 134.2
//   ---
//   <points x 8 bytes, little-endian f64>
//
// Complex data is stored interleaved as real, imaginary pairs, so `points`
// counts complex values and the block is `points x 16` bytes. A transfer
// function is its bins followed by its coherence.
//
// Numbers in the header are written as the shortest decimal that reads back to
// the same value, never in scientific notation, so a file written by this code
// is byte-for-byte the file the Rust core wrote.

#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "model/measurement.hpp"

namespace analyzer::model {

// First line of every file. The digit is the format version.
inline constexpr std::string_view kMagic = "ANLZ1";

// Serialise a measurement.
std::vector<std::byte> write_measurement(const Measurement& measurement);

// Deserialise a measurement.
//
// Throws BadMagicError for a file that is not ours, MissingSeparatorError for a
// header that never ends, MissingFieldError or BadValueError for a required key
// that is absent or does not parse, UnknownKindError for a kind this version
// does not know, and TruncatedError for a data block shorter than the header
// promised. All derive from FormatError.
//
// Reading is strict where Settings parsing is forgiving: a measurement that
// silently substituted defaults for numbers it could not parse would be a lie.
// The optional keys (id, captured_at, channels and the references) are the
// exception - absent or unparseable, they read as unknown.
Measurement read_measurement(std::span<const std::byte> bytes);

}  // namespace analyzer::model
