#include "dsp/transfer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "base/contract.hpp"
#include "base/units.hpp"

namespace analyzer::dsp {

TransferFunction::TransferFunction(const TransferConfig& config)
    : sample_rate_(config.sample_rate),
      fft_(config.size),
      window_(config.window, config.size),
      hop_(overlap_hop(config.overlap, config.size)),
      averaging_(config.averaging),
      reference_frame_(config.size, 0.0f),
      measurement_frame_(config.size, 0.0f),
      windowed_(config.size, 0.0f),
      reference_spectrum_(fft_.bins()),
      measurement_spectrum_(fft_.bins()),
      gxx_(fft_.bins(), 0.0f),
      gyy_(fft_.bins(), 0.0f),
      gxy_(fft_.bins()) {
    ANALYZER_EXPECTS(config.sample_rate > 0.0f, "sample rate must be positive");
}

std::size_t TransferFunction::push(std::span<const float> reference,
                                   std::span<const float> measurement) noexcept {
    ANALYZER_EXPECTS(reference.size() == measurement.size(),
                     "reference and measurement must be sample aligned");

    const std::size_t size = reference_frame_.size();
    std::size_t produced = 0;

    while (!reference.empty()) {
        const std::size_t wanted = size - filled_;
        const std::size_t taken = std::min(wanted, reference.size());
        const auto at = static_cast<std::ptrdiff_t>(filled_);

        std::copy_n(reference.begin(), taken, reference_frame_.begin() + at);
        std::copy_n(measurement.begin(), taken, measurement_frame_.begin() + at);

        filled_ += taken;
        reference = reference.subspan(taken);
        measurement = measurement.subspan(taken);

        if (filled_ == size) {
            process_frame();
            ++produced;

            const auto hop = static_cast<std::ptrdiff_t>(hop_);
            std::copy(reference_frame_.begin() + hop, reference_frame_.end(),
                      reference_frame_.begin());
            std::copy(measurement_frame_.begin() + hop, measurement_frame_.end(),
                      measurement_frame_.begin());
            filled_ = size - hop_;
        }
    }
    return produced;
}

void TransferFunction::process_frame() noexcept {
    window_.apply_to(reference_frame_, windowed_);
    fft_.forward(windowed_, reference_spectrum_);

    window_.apply_to(measurement_frame_, windowed_);
    fft_.forward(windowed_, measurement_spectrum_);

    // Weight for this frame's contribution. Infinite averaging uses an
    // incremental mean, which stays stable however long a measurement runs.
    float weight = 1.0f;
    switch (averaging_.mode) {
        case TransferAveraging::Mode::Infinite:
            weight = 1.0f / (static_cast<float>(frames_) + 1.0f);
            break;
        case TransferAveraging::Mode::Exponential:
            weight = (frames_ == 0) ? 1.0f : std::clamp(averaging_.alpha, 0.0f, 1.0f);
            break;
    }

    for (std::size_t k = 0; k < gxx_.size(); ++k) {
        const Complex32 x = reference_spectrum_[k];
        const Complex32 y = measurement_spectrum_[k];

        const float auto_x = std::norm(x);
        const float auto_y = std::norm(y);
        // Y * conj(X). The conjugate goes on the reference so that a
        // measurement delayed relative to it produces negative phase, which is
        // the sign convention every analyser displays.
        const Complex32 cross = y * std::conj(x);

        gxx_[k] += (auto_x - gxx_[k]) * weight;
        gyy_[k] += (auto_y - gyy_[k]) * weight;
        gxy_[k] += (cross - gxy_[k]) * weight;
    }

    if (frames_ < std::numeric_limits<std::uint32_t>::max()) {
        ++frames_;
    }
}

void TransferFunction::write_magnitude_db(std::span<float> out) const noexcept {
    ANALYZER_EXPECTS(out.size() == gxx_.size(), "output must be one per bin");
    for (std::size_t k = 0; k < out.size(); ++k) {
        // No reference energy in a bin means nothing can be said about the
        // system there. A zero would read as "flat", which is a lie.
        out[k] = kMagnitudeFloorDb;
        if (gxx_[k] > 0.0f) {
            const float magnitude = std::abs(gxy_[k]) / gxx_[k];
            if (magnitude > 0.0f) {
                out[k] = amplitude_to_db(magnitude);
            }
        }
    }
}

void TransferFunction::write_response(std::span<Complex32> out) const noexcept {
    ANALYZER_EXPECTS(out.size() == gxx_.size(), "output must be one per bin");
    for (std::size_t k = 0; k < out.size(); ++k) {
        out[k] = (gxx_[k] > 0.0f) ? gxy_[k] / gxx_[k] : Complex32{};
    }
}

void TransferFunction::write_phase_degrees(std::span<float> out) const noexcept {
    ANALYZER_EXPECTS(out.size() == gxy_.size(), "output must be one per bin");
    for (std::size_t k = 0; k < out.size(); ++k) {
        out[k] = std::arg(gxy_[k]) * kDegreesPerRadian<float>;
    }
}

void TransferFunction::write_coherence(std::span<float> out) const noexcept {
    ANALYZER_EXPECTS(out.size() == gxx_.size(), "output must be one per bin");
    for (std::size_t k = 0; k < out.size(); ++k) {
        const float denominator = gxx_[k] * gyy_[k];
        // Clamped because floating-point error can push a perfectly coherent
        // bin a hair above one, and a coherence of 1.0000001 looks like a bug
        // to anyone reading it.
        out[k] =
            (denominator > 0.0f) ? std::clamp(std::norm(gxy_[k]) / denominator, 0.0f, 1.0f) : 0.0f;
    }
}

void TransferFunction::reset() noexcept {
    std::fill(gxx_.begin(), gxx_.end(), 0.0f);
    std::fill(gyy_.begin(), gyy_.end(), 0.0f);
    std::fill(gxy_.begin(), gxy_.end(), Complex32{});
    std::fill(reference_frame_.begin(), reference_frame_.end(), 0.0f);
    std::fill(measurement_frame_.begin(), measurement_frame_.end(), 0.0f);
    filled_ = 0;
    frames_ = 0;
}

void unwrap_phase_degrees(std::span<float> phase) noexcept {
    float offset = 0.0f;
    float previous = phase.empty() ? 0.0f : phase.front();
    for (float& value : phase) {
        const float raw = value;
        const float step = raw - previous;
        if (step > 180.0f) {
            offset -= 360.0f;
        } else if (step < -180.0f) {
            offset += 360.0f;
        }
        previous = raw;
        value = raw + offset;
    }
}

}  // namespace analyzer::dsp
