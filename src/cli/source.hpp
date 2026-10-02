// Where the offline analysis gets its audio: a WAV file or a synthesised sine.

#pragma once

#include <filesystem>

#include "audio/offline.hpp"

namespace analyzer::cli {

// Read a WAV file into the form the offline backend plays.
//
// Every sample format is normalised to float in -1..1 by the model's reader,
// so nothing downstream has to care how the file was stored. Throws
// model::ModelError when the file cannot be read.
audio::Source read_source(const std::filesystem::path& path);

// One channel of `seconds` of a sine at `hz`, amplitude `amplitude`, sampled at
// `rate`.
//
// The phase is computed in double and narrowed per sample, so a long
// high-frequency tone does not accumulate single-precision error.
audio::Source synthesise_sine(double hz, double rate, double seconds, float amplitude);

}  // namespace analyzer::cli
