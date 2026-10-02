// Ported from crates/analyzer-model/src/wav.rs.

#include "model/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "model/error.hpp"
#include "test_util.hpp"

namespace analyzer::model {
namespace {

using dsp::Signal;
using test::scratch;

constexpr float kRate = 48'000.0f;

// The integer a file stored, recovered from the normalised sample the reader
// returns. Dividing by 2^(bits-1) is exact in binary, so this loses nothing.
std::int32_t stored_integer(float normalised, int bits) {
    return static_cast<std::int32_t>(std::lround(normalised * std::ldexp(1.0f, bits - 1)));
}

TEST(Wav, RendersTheRequestedLength) {
    const std::vector<float> samples = render(Signal::sine(1000.0f, 0.5f), kRate, 0.5f);
    EXPECT_EQ(samples.size(), 24'000u);
}

// Two runs must produce identical bytes, or a parity comparison cannot be
// repeated and a noise measurement cannot be compared against itself.
TEST(Wav, GenerationIsReproducible) {
    const std::vector<float> one = render(Signal::pink_noise(0.5f), kRate, 0.1f);
    const std::vector<float> two = render(Signal::pink_noise(0.5f), kRate, 0.1f);
    EXPECT_EQ(one, two);
}

TEST(Wav, AFloatRoundTripIsExact) {
    const auto path = scratch("float.wav");
    const std::vector<float> samples = render(Signal::sine(997.0f, 0.5f), kRate, 0.05f);
    write_wav(path, samples, kRate, SampleDepth::Float32);

    const WavFile file = read_wav(path);
    EXPECT_EQ(file.channels, 1u);
    EXPECT_EQ(file.sample_rate, 48'000.0);
    EXPECT_EQ(file.bits_per_sample, 32);
    EXPECT_TRUE(file.is_float);

    EXPECT_EQ(file.samples, samples) << "float output must not change a sample";
    std::filesystem::remove(path);
}

TEST(Wav, IntegerDepthsRoundTripWithinTheirResolution) {
    for (const auto& [depth, bits] :
         {std::pair{SampleDepth::Int16, 15}, std::pair{SampleDepth::Int24, 23}}) {
        const auto path = scratch(std::string(as_key(depth)) + ".wav");
        const std::vector<float> samples = render(Signal::sine(997.0f, 0.5f), kRate, 0.05f);
        write_wav(path, samples, kRate, depth);

        const WavFile file = read_wav(path);
        EXPECT_EQ(file.bits_per_sample, bits + 1);
        EXPECT_FALSE(file.is_float);
        const auto peak = static_cast<float>((std::int64_t{1} << bits) - 1);

        ASSERT_EQ(file.samples.size(), samples.size());
        float worst = 0.0f;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const auto read = static_cast<float>(stored_integer(file.samples[i], bits + 1)) / peak;
            worst = std::max(worst, std::abs(read - samples[i]));
        }
        // One quantisation step is the most it may move.
        EXPECT_LE(worst, 1.0f / peak + 1e-7f) << as_key(depth) << " drifted " << worst;
        std::filesystem::remove(path);
    }
}

// Wrapping would turn a sample a hair over full scale into a full-scale
// excursion of the opposite sign - the loudest possible click, at exactly the
// moment the signal was already at its peak.
TEST(Wav, OverFullScaleSaturatesRatherThanWrapping) {
    const auto path = scratch("clip.wav");
    write_wav(path, std::vector<float>{1.5f, -1.5f, 0.0f}, kRate, SampleDepth::Int16);

    const WavFile file = read_wav(path);
    ASSERT_EQ(file.samples.size(), 3u);
    EXPECT_EQ(stored_integer(file.samples[0], 16), 32'767);
    EXPECT_EQ(stored_integer(file.samples[1], 16), -32'767);
    EXPECT_EQ(stored_integer(file.samples[2], 16), 0);
    std::filesystem::remove(path);
}

TEST(Wav, WriteSignalReportsWhatItWrote) {
    const auto path = scratch("oneshot.wav");
    const std::size_t frames =
        write_signal(path, Signal::white_noise(0.25f), kRate, 0.25f, SampleDepth::Float32);
    EXPECT_EQ(frames, 12'000u);
    EXPECT_TRUE(std::filesystem::exists(path));
    std::filesystem::remove(path);
}

TEST(Wav, DegenerateParametersAreRefusedRatherThanProducingAFile) {
    for (const auto& [rate, seconds] : {std::pair{0.0f, 1.0f}, std::pair{-48'000.0f, 1.0f},
                                        std::pair{48'000.0f, 0.0f}, std::pair{48'000.0f, -1.0f}}) {
        EXPECT_THROW(render(Signal::sine(1000.0f, 0.5f), rate, seconds), BadParameterError)
            << "rate " << rate << " seconds " << seconds << " should fail";
    }
    EXPECT_THROW(render(Signal::silence(), std::numeric_limits<float>::quiet_NaN(), 1.0f),
                 BadParameterError);
}

TEST(Wav, EveryDepthTokenRoundTrips) {
    for (const SampleDepth depth : {SampleDepth::Int16, SampleDepth::Int24, SampleDepth::Float32}) {
        EXPECT_EQ(depth_from_key(as_key(depth)), std::optional(depth));
    }
    EXPECT_EQ(depth_from_key("nonsense"), std::nullopt);
}

// A sweep is the parity run's most demanding stimulus and the measurement path's
// stimulus too, so it must survive a file round trip intact.
TEST(Wav, ASweepSurvivesAFloatRoundTrip) {
    const auto path = scratch("sweep.wav");
    const Signal signal = Signal::sweep(20.0f, 20'000.0f, 0.2f, 0.5f, false);
    write_signal(path, signal, kRate, 0.2f, SampleDepth::Float32);

    const WavFile file = read_wav(path);
    EXPECT_EQ(file.samples, render(signal, kRate, 0.2f));
    // It should actually sweep: energy late in the file sits well above the
    // starting frequency, so a file of silence or a stuck tone fails here.
    EXPECT_TRUE(std::ranges::any_of(file.samples, [](float s) { return std::abs(s) > 0.4f; }))
        << "the sweep is silent";
    std::filesystem::remove(path);
}

}  // namespace
}  // namespace analyzer::model
