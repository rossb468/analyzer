#include "dsp/delay.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "base/contract.hpp"

namespace analyzer::dsp {

namespace {

// Validate the constructor's arguments and pass `size` through.
//
// Done here, ahead of every member, so that a bad argument aborts with its own
// message rather than with the FFT's complaint about the size.
std::size_t checked_size(float sample_rate, std::size_t size) {
    ANALYZER_EXPECTS(size >= 4 && size % 2 == 0, "correlation size must be even and at least 4");
    ANALYZER_EXPECTS(sample_rate > 0.0f, "sample rate must be positive");
    return size;
}

float energy(std::span<const float> samples) noexcept {
    float sum = 0.0f;
    for (const float s : samples) {
        sum += s * s;
    }
    return sum;
}

}  // namespace

DelayFinder::DelayFinder(float sample_rate, std::size_t size, Weighting weighting)
    : sample_rate_(sample_rate),
      size_(checked_size(sample_rate, size)),
      weighting_(weighting),
      fft_(size_),
      padded_reference_(size_, 0.0f),
      padded_measurement_(size_, 0.0f),
      reference_spectrum_(fft_.bins()),
      measurement_spectrum_(fft_.bins()),
      cross_(fft_.bins()),
      correlation_(size_, 0.0f) {}

DelayFinder DelayFinder::acoustic(float sample_rate) {
    return DelayFinder(sample_rate, 16384, Weighting::Phat);
}

std::optional<DelayEstimate> DelayFinder::find(std::span<const float> reference,
                                               std::span<const float> measurement) noexcept {
    const std::size_t take = std::min({size_, reference.size(), measurement.size()});
    if (take == 0) {
        return std::nullopt;
    }

    std::fill(padded_reference_.begin(), padded_reference_.end(), 0.0f);
    std::fill(padded_measurement_.begin(), padded_measurement_.end(), 0.0f);
    std::copy_n(reference.begin(), take, padded_reference_.begin());
    std::copy_n(measurement.begin(), take, padded_measurement_.begin());

    if (energy(padded_reference_) <= std::numeric_limits<float>::epsilon() ||
        energy(padded_measurement_) <= std::numeric_limits<float>::epsilon()) {
        return std::nullopt;
    }

    fft_.forward(padded_reference_, reference_spectrum_);
    fft_.forward(padded_measurement_, measurement_spectrum_);

    // Y * conj(X): a measurement that lags the reference produces a positive
    // peak position.
    for (std::size_t k = 0; k < cross_.size(); ++k) {
        cross_[k] = measurement_spectrum_[k] * std::conj(reference_spectrum_[k]);
    }
    switch (weighting_) {
        case Weighting::None: break;
        case Weighting::Phat:
            for (Complex32& product : cross_) {
                const float magnitude = std::abs(product);
                product = magnitude > 1e-20f ? product / magnitude : Complex32{};
            }
            break;
    }

    // No clean-up of the DC and Nyquist bins is needed before this: realfft's
    // inverse rejected a residual imaginary part there, so the Rust zeroed it
    // first, but Fft::inverse ignores it.
    fft_.inverse(cross_, correlation_);

    return locate_peak();
}

DelayEstimate DelayFinder::locate_peak() const noexcept {
    // `>=` so the last of several equal maxima wins, as Rust's max_by does.
    std::size_t index = 0;
    float peak = correlation_[0];
    for (std::size_t i = 1; i < correlation_.size(); ++i) {
        if (correlation_[i] >= peak) {
            index = i;
            peak = correlation_[i];
        }
    }

    float sum = 0.0f;
    for (const float v : correlation_) {
        sum += std::abs(v);
    }
    const float mean = sum / static_cast<float>(size_);
    const float confidence = mean > 0.0f ? peak / mean : 0.0f;

    // Sub-sample refinement by fitting a parabola through the peak and its
    // neighbours. Worth doing: at 48 kHz one sample is 7 mm of path length,
    // and alignment work cares at that scale.
    //
    // The neighbours wrap, because the correlation is circular. Reaching for
    // index - 1 directly underflows at index 0 and silently skips refinement
    // for exactly the zero-delay case, which is the one most likely to be
    // sub-sample.
    const float before = correlation_[(index + size_ - 1) % size_];
    const float after = correlation_[(index + 1) % size_];
    const float curvature = before - 2.0f * peak + after;
    const float offset = std::abs(curvature) > 1e-20f
                             ? std::clamp(0.5f * (before - after) / curvature, -0.5f, 0.5f)
                             : 0.0f;

    // The correlation is circular, so the upper half represents negative
    // lags: the measurement arriving *before* the reference.
    const float position = static_cast<float>(index) + offset;
    const float samples = index > size_ / 2 ? position - static_cast<float>(size_) : position;

    return DelayEstimate{
        .samples = samples,
        .seconds = samples / sample_rate_,
        .confidence = confidence,
    };
}

}  // namespace analyzer::dsp
