#include "cli/source.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

#include "base/numeric.hpp"
#include "model/wav.hpp"

namespace analyzer::cli {

audio::Source read_source(const std::filesystem::path& path) {
    model::WavFile file = model::read_wav(path);
    return audio::Source(std::move(file.samples), file.channels, file.sample_rate);
}

audio::Source synthesise_sine(double hz, double rate, double seconds, float amplitude) {
    const auto frames = saturating_cast<std::size_t>(std::max(std::round(rate * seconds), 0.0));
    std::vector<float> samples(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        const double phase = 2.0 * std::numbers::pi * hz * static_cast<double>(n) / rate;
        samples[n] = amplitude * static_cast<float>(std::sin(phase));
    }
    return audio::Source::mono(std::move(samples), rate);
}

}  // namespace analyzer::cli
