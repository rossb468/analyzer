// Welch spectra for dsp tests, measured by the real dsp::SpectrumAnalyzer.
//
// Half-overlapped frames averaged with Averaging::Infinite, which is a plain
// running mean over every frame in the buffer: the long-average reading a test
// wants when it asks what a signal looks like rather than how the analyzer
// behaves over time. Tests that care about the analyzer itself build their own
// SpectrumConfig.
//
// Links analyzer::dsp, so only tests that already do should include this.

#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "dsp/spectrum.hpp"
#include "dsp/window.hpp"

namespace analyzer::test {

// An analyzer that has been fed all of `samples`.
inline dsp::SpectrumAnalyzer analyse(std::span<const float> samples, float sample_rate,
                                     std::size_t size, dsp::WindowKind window) {
    dsp::SpectrumAnalyzer analyzer({
        .sample_rate = sample_rate,
        .size = size,
        .window = window,
        .overlap = dsp::Overlap::Half,
        .averaging = dsp::Averaging::infinite(),
    });
    analyzer.push(samples);
    return analyzer;
}

// Linear mean-square power per bin. A bin-centred sine of amplitude A reads
// A^2 / 2.
inline std::vector<float> average_power(std::span<const float> samples, float sample_rate,
                                        std::size_t size, dsp::WindowKind window) {
    const auto analyzer = analyse(samples, sample_rate, size, window);
    const auto power = analyzer.power();
    return {power.begin(), power.end()};
}

// The same in dBFS, where 0 dBFS is a full-scale sine.
inline std::vector<float> average_db_fs(std::span<const float> samples, float sample_rate,
                                        std::size_t size, dsp::WindowKind window) {
    const auto analyzer = analyse(samples, sample_rate, size, window);
    std::vector<float> db(analyzer.bins());
    analyzer.write_db_fs(db);
    return db;
}

}  // namespace analyzer::test
