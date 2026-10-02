#include "cal/weighting.hpp"

#include <cmath>

namespace analyzer::cal {

namespace {

// Pole frequencies from IEC 61672-1.
constexpr float kF1 = 20.598997f;
constexpr float kF2 = 107.65265f;
constexpr float kF3 = 737.86223f;
constexpr float kF4 = 12194.217f;

// Normalisation constants making each curve exactly 0 dB at 1 kHz.
constexpr float kAOffset = 2.0f;
constexpr float kCOffset = 0.062f;

float a_weighting_db(float hz) noexcept {
    const float f2 = hz * hz;
    const float numerator = kF4 * kF4 * f2 * f2;
    const float denominator =
        (f2 + kF1 * kF1) * std::sqrt((f2 + kF2 * kF2) * (f2 + kF3 * kF3)) * (f2 + kF4 * kF4);
    if (denominator <= 0.0f) {
        return -200.0f;
    }
    return 20.0f * std::log10(numerator / denominator) + kAOffset;
}

float c_weighting_db(float hz) noexcept {
    const float f2 = hz * hz;
    const float numerator = kF4 * kF4 * f2;
    const float denominator = (f2 + kF1 * kF1) * (f2 + kF4 * kF4);
    if (denominator <= 0.0f) {
        return -200.0f;
    }
    return 20.0f * std::log10(numerator / denominator) + kCOffset;
}

}  // namespace

float db_at(Weighting weighting, float hz) noexcept {
    if (hz <= 0.0f) {
        return -200.0f;
    }
    switch (weighting) {
        case Weighting::Z: return 0.0f;
        case Weighting::A: return a_weighting_db(hz);
        case Weighting::C: return c_weighting_db(hz);
    }
    return 0.0f;
}

std::string_view label(Weighting weighting) noexcept {
    switch (weighting) {
        case Weighting::Z: return "Z";
        case Weighting::A: return "A";
        case Weighting::C: return "C";
    }
    return "?";
}

}  // namespace analyzer::cal
