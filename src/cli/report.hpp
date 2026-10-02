// Rendering a spectrum as text.
//
// Shared by the offline and live paths so both emit byte-identical formats.
// The layout deliberately mirrors REW's text import - `#` comments then
// whitespace-separated columns - since that is the cheap interoperability
// bridge in both directions.

#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "dsp/spectrum.hpp"
#include "dsp/window.hpp"
#include "engine/engine.hpp"

namespace analyzer::cli {

// Everything the header needs that the frame itself does not carry.
struct Meta {
    // Where the audio came from, for the header.
    std::string source;
    // Channels the source delivered.
    std::size_t channels = 0;
    // Which one was analysed.
    std::size_t channel = 0;
    // Rate the analysis ran at.
    double sample_rate = 0.0;
    // Analysis window.
    dsp::WindowKind window;
    // Frame overlap.
    dsp::Overlap overlap = dsp::Overlap::ThreeQuarters;
    // Averaging mode.
    dsp::Averaging averaging;
    // Effective noise bandwidth of one bin.
    float enbw_hz = 0.0f;
    // FFT size.
    std::size_t fft_size = 0;
    // Samples between frames.
    std::size_t hop = 0;
};

// A window as the Rust core's `Debug` printed it: `Hann`, `Tukey { alpha: 0.25 }`.
// The header carries it in that form because the golden files do.
std::string describe(dsp::WindowKind window);

// An averaging mode the same way: `Infinite`, `PeakHold`, `Exponential { alpha: 0.2 }`.
std::string describe(dsp::Averaging averaging);

// Render `frame` as text.
//
// `min_db` omits bins below a level; `peak_only` emits just the loudest bin.
std::string render(const engine::SpectrumFrame& frame, const Meta& meta,
                   std::optional<float> min_db, bool peak_only);

}  // namespace analyzer::cli
