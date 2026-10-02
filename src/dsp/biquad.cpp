#include "dsp/biquad.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "base/units.hpp"

namespace analyzer::dsp {

namespace {

// The two quantities every cookbook formula shares.
struct Design {
    float cos_w;
    float alpha;
};

// Returns nullopt for a design that cannot be realised.
//
// A centre frequency at or above Nyquist has no meaning, and the caller
// gets a pass-through rather than a filter full of NaNs. This is not
// theoretical: an equaliser preset written at 96 kHz and loaded at 44.1
// puts its top band above Nyquist, and silently producing NaNs there would
// take the whole output with it.
std::optional<Design> make_design(float hz, float q, float sample_rate) noexcept {
    if (!std::isfinite(hz) || !std::isfinite(q) || sample_rate <= 0.0f) {
        return std::nullopt;
    }
    // Just under Nyquist. Exactly at it, sin(w) is zero and alpha collapses.
    const float nyquist = sample_rate * 0.5f;
    if (hz <= 0.0f || hz >= nyquist * 0.999f) {
        return std::nullopt;
    }
    q = std::max(q, 1e-3f);
    const float w = kTau<float> * hz / sample_rate;
    return Design{std::cos(w), std::sin(w) / (2.0f * q)};
}

// Cookbook A: the square root of the linear gain, because a peaking filter
// applies it twice.
float amplitude(float gain_db) noexcept {
    return std::pow(10.0f, gain_db / 40.0f);
}

}  // namespace

Biquad Biquad::normalised(float ff0, float ff1, float ff2, float a0, float fb1,
                          float fb2) noexcept {
    if (!std::isfinite(a0) || std::abs(a0) < std::numeric_limits<float>::epsilon()) {
        return identity();
    }
    return {ff0 / a0, ff1 / a0, ff2 / a0, fb1 / a0, fb2 / a0};
}

Complex32 Biquad::response_at(float hz, float sample_rate) const noexcept {
    if (sample_rate <= 0.0f) {
        return {1.0f, 0.0f};
    }
    const float w = kTau<float> * hz / sample_rate;
    const Complex32 z1 = std::polar(1.0f, -w);
    const Complex32 z2 = z1 * z1;
    const Complex32 numerator = Complex32(b0, 0.0f) + z1 * b1 + z2 * b2;
    const Complex32 denominator = Complex32(1.0f, 0.0f) + z1 * a1 + z2 * a2;
    if (std::norm(denominator) > 0.0f) {
        return numerator / denominator;
    }
    return {0.0f, 0.0f};
}

float Biquad::magnitude_at(float hz, float sample_rate) const noexcept {
    return std::abs(response_at(hz, sample_rate));
}

// ---------------------------------------------------------------- RBJ --

Biquad Biquad::peaking(float hz, float q, float gain_db, float sample_rate) noexcept {
    const auto design = make_design(hz, q, sample_rate);
    if (!design) {
        return identity();
    }
    const float a = amplitude(gain_db);
    return normalised(1.0f + design->alpha * a, -2.0f * design->cos_w, 1.0f - design->alpha * a,
                      1.0f + design->alpha / a, -2.0f * design->cos_w, 1.0f - design->alpha / a);
}

Biquad Biquad::low_shelf(float hz, float q, float gain_db, float sample_rate) noexcept {
    const auto design = make_design(hz, q, sample_rate);
    if (!design) {
        return identity();
    }
    const float a = amplitude(gain_db);
    const float sqrt_a = std::sqrt(a);
    const float two_sqrt_a_alpha = 2.0f * sqrt_a * design->alpha;
    const float cos_w = design->cos_w;
    return normalised(a * ((a + 1.0f) - (a - 1.0f) * cos_w + two_sqrt_a_alpha),
                      2.0f * a * ((a - 1.0f) - (a + 1.0f) * cos_w),
                      a * ((a + 1.0f) - (a - 1.0f) * cos_w - two_sqrt_a_alpha),
                      (a + 1.0f) + (a - 1.0f) * cos_w + two_sqrt_a_alpha,
                      -2.0f * ((a - 1.0f) + (a + 1.0f) * cos_w),
                      (a + 1.0f) + (a - 1.0f) * cos_w - two_sqrt_a_alpha);
}

Biquad Biquad::high_shelf(float hz, float q, float gain_db, float sample_rate) noexcept {
    const auto design = make_design(hz, q, sample_rate);
    if (!design) {
        return identity();
    }
    const float a = amplitude(gain_db);
    const float sqrt_a = std::sqrt(a);
    const float two_sqrt_a_alpha = 2.0f * sqrt_a * design->alpha;
    const float cos_w = design->cos_w;
    return normalised(a * ((a + 1.0f) + (a - 1.0f) * cos_w + two_sqrt_a_alpha),
                      -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cos_w),
                      a * ((a + 1.0f) + (a - 1.0f) * cos_w - two_sqrt_a_alpha),
                      (a + 1.0f) - (a - 1.0f) * cos_w + two_sqrt_a_alpha,
                      2.0f * ((a - 1.0f) - (a + 1.0f) * cos_w),
                      (a + 1.0f) - (a - 1.0f) * cos_w - two_sqrt_a_alpha);
}

Biquad Biquad::low_pass(float hz, float q, float sample_rate) noexcept {
    const auto design = make_design(hz, q, sample_rate);
    if (!design) {
        return identity();
    }
    const float shared = 1.0f - design->cos_w;
    return normalised(shared * 0.5f, shared, shared * 0.5f, 1.0f + design->alpha,
                      -2.0f * design->cos_w, 1.0f - design->alpha);
}

Biquad Biquad::high_pass(float hz, float q, float sample_rate) noexcept {
    const auto design = make_design(hz, q, sample_rate);
    if (!design) {
        return identity();
    }
    const float shared = 1.0f + design->cos_w;
    return normalised(shared * 0.5f, -shared, shared * 0.5f, 1.0f + design->alpha,
                      -2.0f * design->cos_w, 1.0f - design->alpha);
}

Biquad Biquad::band_pass(float hz, float q, float sample_rate) noexcept {
    const auto design = make_design(hz, q, sample_rate);
    if (!design) {
        return identity();
    }
    return normalised(design->alpha, 0.0f, -design->alpha, 1.0f + design->alpha,
                      -2.0f * design->cos_w, 1.0f - design->alpha);
}

Biquad Biquad::notch(float hz, float q, float sample_rate) noexcept {
    const auto design = make_design(hz, q, sample_rate);
    if (!design) {
        return identity();
    }
    return normalised(1.0f, -2.0f * design->cos_w, 1.0f, 1.0f + design->alpha,
                      -2.0f * design->cos_w, 1.0f - design->alpha);
}

Biquad Biquad::all_pass(float hz, float q, float sample_rate) noexcept {
    const auto design = make_design(hz, q, sample_rate);
    if (!design) {
        return identity();
    }
    return normalised(1.0f - design->alpha, -2.0f * design->cos_w, 1.0f + design->alpha,
                      1.0f + design->alpha, -2.0f * design->cos_w, 1.0f - design->alpha);
}

// ------------------------------------------------- weighting building --

Biquad Biquad::double_pole_highpass(float omega, float sample_rate) noexcept {
    const float c = 2.0f * sample_rate;
    omega = prewarp(omega, sample_rate);
    const float a = c + omega;
    const float b = omega - c;
    const float gain = (c * c) / (a * a);
    return {gain, -2.0f * gain, gain, 2.0f * b / a, (b * b) / (a * a)};
}

Biquad Biquad::two_pole_lowpass(float omega_a, float omega_b, float sample_rate) noexcept {
    const float c = 2.0f * sample_rate;
    omega_a = prewarp(omega_a, sample_rate);
    omega_b = prewarp(omega_b, sample_rate);
    const float pa = c + omega_a;
    const float qa = omega_a - c;
    const float pb = c + omega_b;
    const float qb = omega_b - c;
    const float norm = pa * pb;
    return {1.0f / norm, 2.0f / norm, 1.0f / norm, (pa * qb + pb * qa) / norm, (qa * qb) / norm};
}

float prewarp(float omega, float sample_rate) noexcept {
    const float c = 2.0f * sample_rate;
    // Guard against the tangent blowing up for a pole at or beyond Nyquist.
    const float normalised = std::clamp(omega / c, 0.0f, 1.55f);
    return c * std::tan(normalised);
}

}  // namespace analyzer::dsp
