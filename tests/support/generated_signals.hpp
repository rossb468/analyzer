// Stimuli for dsp tests, made by the real dsp::Generator.
//
// The Rust tests build their noise and sweeps with Generator, so a seed here
// gives the same signal as there - and a test that feeds the code under test
// the very stimulus the product uses is a better test than one fed a private
// copy of the algorithm. (The copies this replaced had drifted into six
// near-identical files.)
//
// Everything is deterministic: the generator's xorshift is the same on every
// platform, which is why it is not <random>.
//
// Links analyzer::dsp, so only tests that already do should include this.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "dsp/generator.hpp"

namespace analyzer::test {

// Sample rate every dsp test runs at.
inline constexpr float kGeneratedRate = 48'000.0f;

// `count` samples of `signal`, from a fresh generator.
inline std::vector<float> generate(dsp::Signal signal, std::size_t count, std::uint64_t seed = 1,
                                   float sample_rate = kGeneratedRate) {
    dsp::Generator generator(sample_rate, signal, seed);
    std::vector<float> out(count, 0.0f);
    generator.fill(out);
    return out;
}

// Uniform white noise in [-amplitude, amplitude).
inline std::vector<float> white_noise(std::size_t count, float amplitude, std::uint64_t seed) {
    return generate(dsp::Signal::white_noise(amplitude), count, seed);
}

// Pink noise, -3 dB per octave.
inline std::vector<float> pink_noise(std::size_t count, float amplitude, std::uint64_t seed) {
    return generate(dsp::Signal::pink_noise(amplitude), count, seed);
}

// One non-repeating exponential (Farina) sweep exactly `count` samples long.
inline std::vector<float> sweep(std::size_t count, float start_hz = 20.0f, float end_hz = 20'000.0f,
                                float amplitude = 0.5f, float sample_rate = kGeneratedRate) {
    return generate(dsp::Signal::sweep(start_hz, end_hz, static_cast<float>(count) / sample_rate,
                                       amplitude, false),
                    count, 1, sample_rate);
}

}  // namespace analyzer::test
