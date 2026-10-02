// Conversions between the C enums and structs and the core's own types.
//
// Values arrive from C, where nothing stops a caller passing an integer that is
// not an enumerator. Rust treated that as undefined behaviour; here every
// conversion ends in a defined answer, the default a fresh session would use,
// rather than falling off the end of a switch.

#pragma once

#include "dsp/eq.hpp"
#include "dsp/spectrum.hpp"
#include "dsp/window.hpp"
#include "ffi/c_api.hpp"
#include "model/filter_export.hpp"
#include "model/settings.hpp"
#include "model/wav.hpp"
#include "plot/reduce.hpp"

namespace analyzer::ffi {

// Tukey is the only window with a parameter; it is fixed at alpha 0.25.
dsp::WindowKind to_window_kind(AnalyzerWindow window) noexcept;

dsp::Overlap to_overlap(AnalyzerOverlap overlap) noexcept;

plot::Reduction to_reduction(AnalyzerReduction reduction) noexcept;

dsp::FilterKind to_filter_kind(AnalyzerFilterKind kind) noexcept;
AnalyzerFilterKind to_c(dsp::FilterKind kind) noexcept;

dsp::FilterBand to_filter_band(const AnalyzerBand& band) noexcept;
AnalyzerBand to_c(const dsp::FilterBand& band) noexcept;

model::FilterFormat to_filter_format(AnalyzerFilterFormat format) noexcept;

model::SampleDepth to_sample_depth(AnalyzerSampleDepth depth) noexcept;

// The settings as a flat struct. The optional SPL offset is split into a flag
// and a value rather than using a sentinel, because every sentinel worth
// choosing is a level someone could legitimately measure.
AnalyzerSettings to_c(const model::Settings& settings) noexcept;

// The settings as the core stores them, validated, so a hand-edited or
// out-of-range value cannot reach the axis code.
model::Settings to_settings(const AnalyzerSettings& settings);

}  // namespace analyzer::ffi
