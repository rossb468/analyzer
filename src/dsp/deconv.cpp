#include "dsp/deconv.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <utility>

#include "base/contract.hpp"

namespace analyzer::dsp {

namespace {

// Validate the constructor's arguments and derive the transform size from them.
//
// Done here, ahead of every member, so that a bad argument aborts with its own
// message rather than with the FFT's complaint about the size it implies.
std::size_t transform_size(float sample_rate, std::size_t max_length) {
    ANALYZER_EXPECTS(max_length > 0, "max_length must be non-zero");
    ANALYZER_EXPECTS(sample_rate > 0.0f, "sample rate must be positive");
    return std::bit_ceil(max_length * 2);
}

// Index of the largest absolute sample, refined between samples.
std::optional<float> locate_peak(std::span<const float> samples) noexcept {
    if (samples.empty()) {
        return std::nullopt;
    }

    // `>=` so the last of several equal maxima wins; std::max_element would
    // pick the first. It matters for a recording of silence, whose response is
    // all zeros.
    std::size_t index = 0;
    float peak = std::abs(samples[0]);
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const float magnitude = std::abs(samples[i]);
        if (magnitude >= peak) {
            index = i;
            peak = magnitude;
        }
    }

    // Parabolic refinement on the magnitude envelope. Neighbours are clamped
    // rather than wrapped: unlike a circular correlation, an impulse response has
    // real ends and the sample before index 0 does not exist.
    const float before = index > 0 ? std::abs(samples[index - 1]) : peak;
    const float after = index + 1 < samples.size() ? std::abs(samples[index + 1]) : peak;

    const float curvature = before - 2.0f * peak + after;
    const float offset = std::abs(curvature) > 1e-20f
                             ? std::clamp(0.5f * (before - after) / curvature, -0.5f, 0.5f)
                             : 0.0f;
    return static_cast<float>(index) + offset;
}

}  // namespace

float ImpulseResponse::time_at(std::size_t index) const noexcept {
    if (sample_rate <= 0.0f) {
        return 0.0f;
    }
    return (static_cast<float>(index) - peak_samples) / sample_rate;
}

float ImpulseResponse::peak_amplitude() const noexcept {
    float peak = 0.0f;
    for (const float sample : samples) {
        peak = std::fmax(peak, std::abs(sample));
    }
    return peak;
}

float ImpulseResponse::duration_seconds() const noexcept {
    if (sample_rate <= 0.0f) {
        return 0.0f;
    }
    return static_cast<float>(samples.size()) / sample_rate;
}

Deconvolver::Deconvolver(float sample_rate, std::size_t max_length)
    : size_(transform_size(sample_rate, max_length)),
      sample_rate_(sample_rate),
      fft_(size_),
      padded_(size_, 0.0f),
      stimulus_spectrum_(fft_.bins()),
      response_spectrum_(fft_.bins()),
      quotient_(fft_.bins()),
      result_(size_, 0.0f) {}

std::optional<ImpulseResponse> Deconvolver::deconvolve(std::span<const float> stimulus,
                                                       std::span<const float> response,
                                                       float regularisation) {
    if (stimulus.empty() || response.empty()) {
        return std::nullopt;
    }
    if (stimulus.size() > max_length() || response.size() > max_length()) {
        return std::nullopt;
    }

    transform(stimulus, stimulus_spectrum_);
    transform(response, response_spectrum_);

    // Floor the denominator relative to the strongest bin, so the epsilon
    // means the same thing regardless of how loud the measurement was.
    float peak_power = 0.0f;
    for (const Complex32 bin : stimulus_spectrum_) {
        peak_power = std::fmax(peak_power, std::norm(bin));
    }
    if (peak_power <= 0.0f) {
        return std::nullopt;
    }
    const float floor = std::fmax(regularisation, 0.0f) * peak_power;

    for (std::size_t k = 0; k < quotient_.size(); ++k) {
        const Complex32 x = stimulus_spectrum_[k];
        const float denominator = std::norm(x) + floor;
        quotient_[k] =
            denominator > 0.0f ? response_spectrum_[k] * std::conj(x) / denominator : Complex32{};
    }

    // The quotient's DC and Nyquist bins may carry a residual imaginary part;
    // Fft::inverse ignores it, so there is nothing to clean up first.
    fft_.inverse(quotient_, result_);

    // The inverse transform is unnormalised.
    const float scale = 1.0f / static_cast<float>(size_);
    for (float& sample : result_) {
        sample *= scale;
    }

    // Keep only the causal region.
    //
    // The transform is circular, so anything the deconvolution produces at
    // negative time - acausal pre-ringing, and whatever the regularisation
    // leaves behind - lands at the far end of the buffer. It is not part of
    // the decay and it is large enough to matter: left in place it holds a
    // Schroeder integral flat for seconds and then plunges, which reads as a
    // reverberation time of minutes.
    //
    // No more impulse response can be recovered than the length of the
    // recording, so that is the bound.
    const std::size_t causal = std::min(response.size(), result_.size());
    std::vector<float> samples(result_.begin(),
                               result_.begin() + static_cast<std::ptrdiff_t>(causal));

    const std::optional<float> peak = locate_peak(samples);
    if (!peak) {
        return std::nullopt;
    }
    return ImpulseResponse{std::move(samples), *peak, sample_rate_};
}

void Deconvolver::transform(std::span<const float> input, std::vector<Complex32>& target) noexcept {
    std::fill(padded_.begin(), padded_.end(), 0.0f);
    std::copy(input.begin(), input.end(), padded_.begin());
    fft_.forward(padded_, target);
}

}  // namespace analyzer::dsp
