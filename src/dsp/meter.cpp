#include "dsp/meter.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

#include "base/contract.hpp"
#include "base/units.hpp"

namespace analyzer::dsp {

namespace {

// RMS of a full-scale sine, the reference for 0 dBFS.
constexpr float kFullScaleRms = std::numbers::sqrt2_v<float> / 2.0f;

// Pole frequencies from IEC 61672-1, in radians per second.
constexpr float kW1 = 20.598'997f * kTau<float>;
constexpr float kW2 = 107.652'65f * kTau<float>;
constexpr float kW3 = 737.862'23f * kTau<float>;
constexpr float kW4 = 12'194.217f * kTau<float>;

// Attack and decay time constants in seconds.
struct TimeConstants {
    float attack;
    float decay;
};

TimeConstants time_constants(const Integration& integration) noexcept {
    switch (integration.kind) {
        case Integration::Kind::Fast: return {0.125f, 0.125f};
        case Integration::Kind::Slow: return {1.0f, 1.0f};
        case Integration::Kind::Impulse: return {0.035f, 1.5f};
        case Integration::Kind::Custom: {
            const float clamped = std::max(integration.seconds, 1e-4f);
            return {clamped, clamped};
        }
    }
    return {0.125f, 0.125f};
}

// One-pole coefficient for a given time constant.
float coefficient(float seconds, float sample_rate) noexcept {
    if (seconds <= 0.0f) {
        return 1.0f;
    }
    return std::clamp(1.0f - std::exp(-1.0f / (seconds * sample_rate)),
                      std::numeric_limits<float>::min(), 1.0f);
}

// RMS to dBFS, referenced to a full-scale sine.
float to_db(float rms) noexcept {
    return amplitude_to_db(rms / kFullScaleRms, kMeterFloorDb);
}

// Checked before anything is built from the rate, so a bad one aborts with the
// right message rather than after the filter has been designed from garbage.
float require_positive(float sample_rate) {
    ANALYZER_EXPECTS(sample_rate > 0.0f, "sample rate must be positive");
    return sample_rate;
}

}  // namespace

const char* to_string(MeterWeighting weighting) noexcept {
    switch (weighting) {
        case MeterWeighting::Z: return "Z";
        case MeterWeighting::A: return "A";
        case MeterWeighting::C: return "C";
    }
    return "unknown";
}

const char* to_string(Integration::Kind kind) noexcept {
    switch (kind) {
        case Integration::Kind::Fast: return "Fast";
        case Integration::Kind::Slow: return "Slow";
        case Integration::Kind::Impulse: return "Impulse";
        case Integration::Kind::Custom: return "Custom";
    }
    return "unknown";
}

LevelMeter::WeightingFilter::WeightingFilter(MeterWeighting weighting, float sample_rate) {
    switch (weighting) {
        case MeterWeighting::Z: break;
        // C is s^2 / ((s+w1)^2(s+w4)^2): two zeros at DC, four poles.
        //
        // Cascading two highpass sections here would give s^4 and an extra
        // 12 dB/octave of bass rolloff - C would read ~50 dB low at 31.5 Hz
        // instead of -3. The w4 pair has to be a lowpass, not a highpass.
        case MeterWeighting::C:
            sections_ = {
                Biquad::double_pole_highpass(kW1, sample_rate),
                Biquad::two_pole_lowpass(kW4, kW4, sample_rate),
            };
            break;
        // A is s^4 / ((s+w1)^2(s+w2)(s+w3)(s+w4)^2): four zeros at DC, so both
        // w1 and w4 sections are highpasses here, plus the w2/w3 pair that
        // pulls the midrange down onto the equal-loudness contour.
        case MeterWeighting::A:
            sections_ = {
                Biquad::double_pole_highpass(kW1, sample_rate),
                Biquad::double_pole_highpass(kW4, sample_rate),
                Biquad::two_pole_lowpass(kW2, kW3, sample_rate),
            };
            break;
    }

    // Normalise so the cascade is exactly unity at 1 kHz, which is the
    // definition of both curves.
    float response = 1.0f;
    for (const Biquad& section : sections_) {
        response *= section.magnitude_at(1000.0f, sample_rate);
    }
    gain_ = (response > 0.0f) ? 1.0f / response : 1.0f;
}

float LevelMeter::WeightingFilter::process(float x) noexcept {
    for (Biquad& section : sections_) {
        x = section.process(x);
    }
    return x * gain_;
}

void LevelMeter::WeightingFilter::reset() noexcept {
    for (Biquad& section : sections_) {
        section.reset();
    }
}

LevelMeter::LevelMeter(float sample_rate, MeterWeighting weighting, Integration integration)
    : sample_rate_(require_positive(sample_rate)),
      filter_(weighting, sample_rate),
      weighting_(weighting),
      integration_(integration),
      attack_(coefficient(time_constants(integration).attack, sample_rate)),
      decay_(coefficient(time_constants(integration).decay, sample_rate)) {}

void LevelMeter::push(std::span<const float> samples) noexcept {
    for (const float sample : samples) {
        const float weighted = filter_.process(sample);
        const float square = weighted * weighted;

        // Asymmetric so Impulse can rise fast and fall slowly.
        const float rate = (square > mean_square_) ? attack_ : decay_;
        mean_square_ += (square - mean_square_) * rate;

        peak_ = std::max(peak_, std::abs(weighted));

        energy_ += static_cast<double>(square);
    }
    samples_ += samples.size();
}

float LevelMeter::rms_db() const noexcept {
    return to_db(std::sqrt(mean_square_));
}

float LevelMeter::peak_db() const noexcept {
    // A peak is an amplitude, and a full-scale sine peaks at 1.0 while
    // reading 0 dBFS, so no RMS correction applies.
    if (peak_ > 0.0f) {
        return amplitude_to_db(peak_);
    }
    return kMeterFloorDb;
}

float LevelMeter::leq_db() const noexcept {
    if (samples_ == 0) {
        return kMeterFloorDb;
    }
    const double mean = energy_ / static_cast<double>(samples_);
    return to_db(std::sqrt(static_cast<float>(mean)));
}

float LevelMeter::elapsed_seconds() const noexcept {
    return static_cast<float>(samples_) / sample_rate_;
}

void LevelMeter::reset() noexcept {
    filter_.reset();
    mean_square_ = 0.0f;
    peak_ = 0.0f;
    energy_ = 0.0;
    samples_ = 0;
}

}  // namespace analyzer::dsp
