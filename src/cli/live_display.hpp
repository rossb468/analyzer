// The pieces of the live meter that are plain arithmetic.
//
// Split out of the CoreAudio implementation so they build, and are tested,
// everywhere: a meter bar and a broadband level have nothing to do with a
// sound card.

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <utility>

namespace analyzer::cli {

// A level this low over the whole run means nothing is arriving.
//
// Real microphones always produce some self-noise, so a spectrum pinned at the
// floor is a permission or routing problem, not a quiet room.
inline constexpr float kSilenceDb = -160.0f;

// Total level across the spectrum, undoing the per-bin dB conversion.
// kSilenceDb when there is no power at all.
float broadband_db(std::span<const float> bins);

// The loudest bin and its level. The last of several equal bins wins, as the
// Rust `max_by` did. An empty spectrum gives bin 0 at kSilenceDb.
std::pair<std::size_t, float> peak_bin(std::span<const float> bins);

// A 40-column meter spanning -90 to 0 dBFS, clamped at both ends.
std::string bar(float db);

}  // namespace analyzer::cli
