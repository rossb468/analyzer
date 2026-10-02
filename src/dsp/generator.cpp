#include "dsp/generator.hpp"

#include <algorithm>
#include <cmath>

#include "base/contract.hpp"
#include "base/numeric.hpp"
#include "base/units.hpp"

namespace analyzer::dsp {

namespace {

// Zero is a fixed point of xorshift, so it must never be the state.
constexpr std::uint64_t kZeroSeedReplacement = 0x9E37'79B9'7F4A'7C15;

}  // namespace

const char* to_string(Signal::Kind kind) noexcept {
    switch (kind) {
        case Signal::Kind::Silence: return "Silence";
        case Signal::Kind::Sine: return "Sine";
        case Signal::Kind::WhiteNoise: return "WhiteNoise";
        case Signal::Kind::PinkNoise: return "PinkNoise";
        case Signal::Kind::Sweep: return "Sweep";
    }
    return "unknown";
}

Generator::Rng::Rng(std::uint64_t seed) noexcept : state(seed == 0 ? kZeroSeedReplacement : seed) {}

float Generator::Rng::next_sample() noexcept {
    std::uint64_t x = state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    state = x;
    const std::uint64_t scrambled = x * 0x2545'F491'4F6C'DD1D;
    // Top 24 bits, mapped to [-1, 1). The low bits of xorshift are weak. 24 bits
    // is exactly what a float's mantissa holds, so the conversion is lossless.
    return static_cast<float>(scrambled >> 40) / 8'388'608.0f - 1.0f;
}

float Generator::PinkFilter::process(float white) noexcept {
    b[0] = 0.99886f * b[0] + white * 0.0555179f;
    b[1] = 0.99332f * b[1] + white * 0.0750759f;
    b[2] = 0.96900f * b[2] + white * 0.153852f;
    b[3] = 0.86650f * b[3] + white * 0.3104856f;
    b[4] = 0.55000f * b[4] + white * 0.5329522f;
    b[5] = -0.7616f * b[5] - white * 0.016898f;
    const float pink = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + white * 0.5362f;
    b[6] = white * 0.115926f;
    return pink * kScale;
}

void Generator::PinkFilter::reset() noexcept {
    std::fill(std::begin(b), std::end(b), 0.0f);
}

Generator::Generator(float sample_rate, Signal signal, std::uint64_t seed)
    : sample_rate_(static_cast<double>(sample_rate)), signal_(signal), rng_(seed) {
    ANALYZER_EXPECTS(sample_rate > 0.0f, "sample rate must be positive");
}

void Generator::set_signal(Signal signal) noexcept {
    signal_ = signal;
    reset();
}

void Generator::reset() noexcept {
    phase_ = 0.0;
    position_ = 0;
    finished_ = false;
    pink_.reset();
}

void Generator::fill(std::span<float> out) noexcept {
    switch (signal_.kind) {
        case Signal::Kind::Silence: std::fill(out.begin(), out.end(), 0.0f); break;
        case Signal::Kind::Sine: fill_sine(out); break;
        case Signal::Kind::WhiteNoise: fill_white(out); break;
        case Signal::Kind::PinkNoise: fill_pink(out); break;
        case Signal::Kind::Sweep: fill_sweep(out); break;
    }
}

void Generator::fill_sine(std::span<float> out) noexcept {
    const float gain = std::clamp(signal_.amplitude, 0.0f, 1.0f);
    const double increment = kTau<double> * static_cast<double>(signal_.hz) / sample_rate_;
    for (float& slot : out) {
        slot = static_cast<float>(std::sin(phase_)) * gain;
        phase_ += increment;
        // Wrapping keeps the accumulator small enough that its resolution
        // never degrades, however long the generator runs.
        if (phase_ >= kTau<double>) {
            phase_ -= kTau<double>;
        }
    }
}

void Generator::fill_white(std::span<float> out) noexcept {
    const float gain = std::clamp(signal_.amplitude, 0.0f, 1.0f);
    for (float& slot : out) {
        slot = rng_.next_sample() * gain;
    }
}

void Generator::fill_pink(std::span<float> out) noexcept {
    const float gain = std::clamp(signal_.amplitude, 0.0f, 1.0f);
    for (float& slot : out) {
        const float white = rng_.next_sample();
        // Clamped because the filter's peak is statistical, not bounded, and a
        // rare overshoot must not clip the converter.
        slot = std::clamp(pink_.process(white) * gain, -1.0f, 1.0f);
    }
}

void Generator::fill_sweep(std::span<float> out) noexcept {
    const float gain = std::clamp(signal_.amplitude, 0.0f, 1.0f);
    // fmax rather than std::max: it returns the other argument when one is NaN,
    // so a NaN frequency or duration falls back to the floor instead of
    // propagating.
    const double start = std::fmax(static_cast<double>(signal_.start_hz), 1e-3);
    const double end = std::fmax(static_cast<double>(signal_.end_hz), start + 1e-3);
    const double duration = std::fmax(static_cast<double>(signal_.seconds), 1e-3);
    // A sweep length is a duration times a rate, so a hostile or absurd
    // `seconds` can overflow the frame count; saturating_cast pins it at the
    // largest count instead of invoking undefined behaviour.
    const auto total = saturating_cast<std::uint64_t>(duration * sample_rate_);
    const double ratio = std::log(end / start);

    for (float& slot : out) {
        if (position_ >= total) {
            if (signal_.repeat) {
                position_ = 0;
            } else {
                finished_ = true;
                slot = 0.0f;
                continue;
            }
        }

        // Farina's exponential sweep. Instantaneous frequency rises
        // geometrically, so equal time is spent in every octave.
        const double t = static_cast<double>(position_) / sample_rate_;
        const double phase =
            (kTau<double> * start * duration / ratio) * (std::exp(t / duration * ratio) - 1.0);
        slot = static_cast<float>(std::sin(phase)) * gain;
        ++position_;
    }
}

}  // namespace analyzer::dsp
