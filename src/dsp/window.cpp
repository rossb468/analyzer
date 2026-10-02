#include "dsp/window.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "base/contract.hpp"

namespace analyzer::dsp {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

// Coefficients of a generalised cosine window, a0 ... an.
//
// The window is w[n] = sum over k of (-1)^k * a_k * cos(2*pi*k*n / N),
// evaluated periodically - denominator N, not N - 1. Periodic is the correct
// choice for spectral analysis; the symmetric variant is for filter design.
constexpr float kHann[] = {0.5f, 0.5f};

// Four-term Blackman-Harris. Very low sidelobes at the cost of a wider main
// lobe, so better dynamic range and worse frequency resolution than Hann.
constexpr float kBlackmanHarris[] = {0.35875f, 0.48829f, 0.14128f, 0.01168f};

// Five-term flat-top. Deliberately poor frequency resolution in exchange for a
// very flat main lobe, which makes amplitude accurate regardless of where a
// tone falls between bins. This is the window to calibrate with.
constexpr float kFlatTop[] = {0.21557895f, 0.41663158f, 0.27726316f, 0.08357895f, 0.006947368f};

void fill_cosine(std::vector<float>& samples, std::span<const float> coefficients) {
    const auto n = static_cast<float>(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const float phase = 2.0f * kPi * static_cast<float>(i) / n;
        float value = 0.0f;
        for (std::size_t k = 0; k < coefficients.size(); ++k) {
            const float sign = (k % 2 == 0) ? 1.0f : -1.0f;
            value += sign * coefficients[k] * std::cos(static_cast<float>(k) * phase);
        }
        samples[i] = value;
    }
}

void fill_tukey(std::vector<float>& samples, float alpha) {
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    const auto n = static_cast<float>(samples.size());
    // Length of each tapered end, in samples.
    const float taper = alpha * n / 2.0f;

    for (std::size_t index = 0; index < samples.size(); ++index) {
        const auto i = static_cast<float>(index);
        if (taper > 0.0f && i < taper) {
            samples[index] = 0.5f * (1.0f - std::cos(kPi * i / taper));
        } else if (taper > 0.0f && i > n - taper) {
            samples[index] = 0.5f * (1.0f - std::cos(kPi * (n - i) / taper));
        } else {
            samples[index] = 1.0f;
        }
    }
}

}  // namespace

const char* to_string(WindowKind::Shape shape) noexcept {
    switch (shape) {
        case WindowKind::Shape::Rectangular: return "Rectangular";
        case WindowKind::Shape::Hann: return "Hann";
        case WindowKind::Shape::BlackmanHarris: return "BlackmanHarris";
        case WindowKind::Shape::FlatTop: return "FlatTop";
        case WindowKind::Shape::Tukey: return "Tukey";
    }
    return "unknown";
}

Window::Window(WindowKind kind, std::size_t size) : kind_(kind), samples_(size, 0.0f) {
    ANALYZER_EXPECTS(size > 0, "window size must be non-zero");

    switch (kind.shape) {
        case WindowKind::Shape::Rectangular:
            std::fill(samples_.begin(), samples_.end(), 1.0f);
            break;
        case WindowKind::Shape::Hann: fill_cosine(samples_, kHann); break;
        case WindowKind::Shape::BlackmanHarris: fill_cosine(samples_, kBlackmanHarris); break;
        case WindowKind::Shape::FlatTop: fill_cosine(samples_, kFlatTop); break;
        case WindowKind::Shape::Tukey: fill_tukey(samples_, kind.tukey_alpha); break;
    }

    // Derive the factors from the samples rather than tabulating them, so they
    // cannot drift out of step with the coefficients above. The tests check
    // these against published values.
    const auto n = static_cast<float>(size);
    float sum = 0.0f;
    float sum_of_squares = 0.0f;
    for (const float w : samples_) {
        sum += w;
        sum_of_squares += w * w;
    }

    coherent_gain_ = sum / n;
    enbw_bins_ = (sum == 0.0f) ? 0.0f : n * sum_of_squares / (sum * sum);
}

float Window::amplitude_correction() const noexcept {
    return (coherent_gain_ == 0.0f) ? 0.0f : 1.0f / coherent_gain_;
}

void Window::apply(std::span<float> frame) const noexcept {
    ANALYZER_EXPECTS(frame.size() == size(), "frame length must equal the window size");
    for (std::size_t i = 0; i < frame.size(); ++i) {
        frame[i] *= samples_[i];
    }
}

void Window::apply_to(std::span<const float> input, std::span<float> output) const noexcept {
    ANALYZER_EXPECTS(input.size() == size(), "input length must equal window size");
    ANALYZER_EXPECTS(output.size() == size(), "output length must equal window size");
    for (std::size_t i = 0; i < input.size(); ++i) {
        output[i] = input[i] * samples_[i];
    }
}

}  // namespace analyzer::dsp
