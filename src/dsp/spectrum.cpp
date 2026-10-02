#include "dsp/spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "base/contract.hpp"

namespace analyzer::dsp {

std::size_t overlap_hop(Overlap overlap, std::size_t size) noexcept {
    std::size_t hop = size;
    switch (overlap) {
        case Overlap::None: hop = size; break;
        case Overlap::Half: hop = size / 2; break;
        case Overlap::ThreeQuarters: hop = size / 4; break;
        case Overlap::SevenEighths: hop = size / 8; break;
    }
    return std::max<std::size_t>(hop, 1);
}

float overlap_fraction(Overlap overlap) noexcept {
    switch (overlap) {
        case Overlap::None: return 0.0f;
        case Overlap::Half: return 0.5f;
        case Overlap::ThreeQuarters: return 0.75f;
        case Overlap::SevenEighths: return 0.875f;
    }
    return 0.0f;
}

Averaging Averaging::exponential_over(float seconds, float frames_per_second) noexcept {
    if (seconds <= 0.0f || frames_per_second <= 0.0f) {
        return none();
    }
    const float alpha = 1.0f - std::exp(-1.0f / (seconds * frames_per_second));
    return exponential(std::clamp(alpha, std::numeric_limits<float>::min(), 1.0f));
}

SpectrumAnalyzer::SpectrumAnalyzer(const SpectrumConfig& config)
    : sample_rate_(config.sample_rate),
      fft_(config.size),
      window_(config.window, config.size),
      hop_(overlap_hop(config.overlap, config.size)),
      averaging_(config.averaging),
      frame_(config.size, 0.0f),
      windowed_(config.size, 0.0f),
      spectrum_(fft_.bins()),
      frame_power_(fft_.bins(), 0.0f),
      power_(fft_.bins(), 0.0f),
      amplitude_scale_(1.0f / (static_cast<float>(config.size) * window_.coherent_gain())) {
    ANALYZER_EXPECTS(config.sample_rate > 0.0f, "sample rate must be positive");
}

std::size_t SpectrumAnalyzer::push(std::span<const float> samples) noexcept {
    const std::size_t size = frame_.size();
    std::size_t produced = 0;

    while (!samples.empty()) {
        const std::size_t wanted = size - filled_;
        const std::size_t taken = std::min(wanted, samples.size());

        std::copy_n(samples.begin(), taken, frame_.begin() + static_cast<std::ptrdiff_t>(filled_));
        filled_ += taken;
        samples = samples.subspan(taken);

        if (filled_ == size) {
            process_frame();
            ++produced;

            // Slide by one hop, keeping the overlapping tail.
            std::copy(frame_.begin() + static_cast<std::ptrdiff_t>(hop_), frame_.end(),
                      frame_.begin());
            filled_ = size - hop_;
        }
    }

    return produced;
}

void SpectrumAnalyzer::process_frame() noexcept {
    window_.apply_to(frame_, windowed_);
    fft_.forward(windowed_, spectrum_);

    // Convert to mean-square power per bin. DC and Nyquist are real and
    // unpaired; every bin between them stands for a conjugate pair, so its
    // amplitude is doubled and its mean square is halved: 2 * |X * s|^2.
    const std::size_t last = spectrum_.size() - 1;
    for (std::size_t k = 0; k < spectrum_.size(); ++k) {
        const float magnitude = std::abs(spectrum_[k]) * amplitude_scale_;
        frame_power_[k] =
            (k == 0 || k == last) ? magnitude * magnitude : 2.0f * magnitude * magnitude;
    }

    accumulate();
    if (frames_ < std::numeric_limits<std::uint32_t>::max()) {
        ++frames_;
    }
}

void SpectrumAnalyzer::accumulate() noexcept {
    const bool first = frames_ == 0;

    switch (averaging_.mode) {
        case Averaging::Mode::None:
            std::copy(frame_power_.begin(), frame_power_.end(), power_.begin());
            break;

        case Averaging::Mode::Exponential:
            if (first) {
                std::copy(frame_power_.begin(), frame_power_.end(), power_.begin());
            } else {
                const float alpha = std::clamp(averaging_.alpha, 0.0f, 1.0f);
                for (std::size_t k = 0; k < power_.size(); ++k) {
                    power_[k] += alpha * (frame_power_[k] - power_[k]);
                }
            }
            break;

        // Incremental mean, which avoids a second accumulator buffer and
        // stays numerically stable as the count grows.
        case Averaging::Mode::Infinite: {
            const float n = static_cast<float>(frames_) + 1.0f;
            for (std::size_t k = 0; k < power_.size(); ++k) {
                power_[k] += (frame_power_[k] - power_[k]) / n;
            }
            break;
        }

        case Averaging::Mode::Linear:
            if (frames_ < averaging_.frames) {
                const float n = static_cast<float>(frames_) + 1.0f;
                for (std::size_t k = 0; k < power_.size(); ++k) {
                    power_[k] += (frame_power_[k] - power_[k]) / n;
                }
            }
            // Otherwise hold: the average is complete.
            break;

        case Averaging::Mode::PeakHold:
            if (first) {
                std::copy(frame_power_.begin(), frame_power_.end(), power_.begin());
            } else {
                for (std::size_t k = 0; k < power_.size(); ++k) {
                    power_[k] = std::max(power_[k], frame_power_[k]);
                }
            }
            break;
    }
}

void SpectrumAnalyzer::write_db_fs(std::span<float> out) const noexcept {
    ANALYZER_EXPECTS(out.size() == power_.size(), "output must be one per bin");
    for (std::size_t k = 0; k < out.size(); ++k) {
        // Full-scale sine has mean square 0.5, so 2 * power normalises it to
        // unity at 0 dBFS.
        out[k] =
            power_[k] > 0.0f ? std::max(10.0f * std::log10(2.0f * power_[k]), kDbFloor) : kDbFloor;
    }
}

void SpectrumAnalyzer::reset() noexcept {
    std::fill(power_.begin(), power_.end(), 0.0f);
    std::fill(frame_.begin(), frame_.end(), 0.0f);
    filled_ = 0;
    frames_ = 0;
}

void SpectrumAnalyzer::set_averaging(Averaging averaging) noexcept {
    averaging_ = averaging;
    std::fill(power_.begin(), power_.end(), 0.0f);
    frames_ = 0;
}

}  // namespace analyzer::dsp
