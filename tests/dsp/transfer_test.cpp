// Ported from crates/analyzer-dsp/src/transfer.rs.

#include "dsp/transfer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "support/generated_signals.hpp"

namespace analyzer::dsp {
namespace {

constexpr float kRate = 48'000.0f;
constexpr std::size_t kSize = 4096;

TransferFunction estimator(TransferAveraging averaging) {
    return TransferFunction(TransferConfig{
        .sample_rate = kRate,
        .size = kSize,
        .window = WindowKind::hann(),
        .overlap = Overlap::Half,
        .averaging = averaging,
    });
}

// Pink noise at the amplitude every test here used.
std::vector<float> pink(std::size_t samples, std::uint64_t seed) {
    return test::pink_noise(samples, 0.5f, seed);
}

std::vector<float> scaled(const std::vector<float>& signal, float gain) {
    std::vector<float> out(signal.size());
    std::transform(signal.begin(), signal.end(), out.begin(), [gain](float s) { return s * gain; });
    return out;
}

// `signal` delayed by `delay` samples, zero-filled at the start.
std::vector<float> delayed(const std::vector<float>& signal, std::size_t delay) {
    std::vector<float> out(signal.size(), 0.0f);
    std::copy(signal.begin(), signal.end() - static_cast<std::ptrdiff_t>(delay),
              out.begin() + static_cast<std::ptrdiff_t>(delay));
    return out;
}

// Bins with enough reference energy to say anything about, avoiding the extreme
// ends where pink noise runs out of level.
std::pair<std::size_t, std::size_t> usable(const TransferFunction& tf) {
    const auto low = static_cast<std::size_t>(200.0f / tf.bin_spacing_hz());
    const auto high = static_cast<std::size_t>(8000.0f / tf.bin_spacing_hz());
    return {low, std::min(high, tf.bins())};
}

float mean_over(const std::vector<float>& values, std::pair<std::size_t, std::size_t> band) {
    const float sum =
        std::accumulate(values.begin() + static_cast<std::ptrdiff_t>(band.first),
                        values.begin() + static_cast<std::ptrdiff_t>(band.second), 0.0f);
    return sum / static_cast<float>(band.second - band.first);
}

// Measuring a wire: the measurement is the reference, so the system is unity
// gain, zero phase, perfect coherence.
TEST(TransferFunction, AnIdenticalSignalMeasuresAsAPerfectWire) {
    const auto signal = pink(kSize * 32, 1);
    auto tf = estimator(TransferAveraging::infinite());
    tf.push(signal, signal);
    EXPECT_GT(tf.frames(), 8u);

    std::vector<float> magnitude(tf.bins());
    std::vector<float> phase(tf.bins());
    std::vector<float> coherence(tf.bins());
    tf.write_magnitude_db(magnitude);
    tf.write_phase_degrees(phase);
    tf.write_coherence(coherence);

    const auto [first, last] = usable(tf);
    for (std::size_t bin = first; bin < last; ++bin) {
        ASSERT_LT(std::abs(magnitude[bin]), 0.01f)
            << "bin " << bin << ": " << magnitude[bin] << " dB";
        ASSERT_LT(std::abs(phase[bin]), 0.1f) << "bin " << bin << ": " << phase[bin] << " deg";
        ASSERT_GT(coherence[bin], 0.999f) << "bin " << bin << ": coherence " << coherence[bin];
    }
}

TEST(TransferFunction, AGainShowsUpAsAFlatOffset) {
    const auto reference = pink(kSize * 32, 2);
    const auto measurement = scaled(reference, 2.0f);

    auto tf = estimator(TransferAveraging::infinite());
    tf.push(reference, measurement);

    std::vector<float> magnitude(tf.bins());
    tf.write_magnitude_db(magnitude);
    const auto [first, last] = usable(tf);
    for (std::size_t bin = first; bin < last; ++bin) {
        ASSERT_LT(std::abs(magnitude[bin] - 6.0206f), 0.01f)
            << "bin " << bin << ": " << magnitude[bin] << " dB, expected +6.02";
    }
}

TEST(TransferFunction, AnAttenuationShowsUpAsANegativeOffset) {
    const auto reference = pink(kSize * 32, 3);
    const auto measurement = scaled(reference, 0.5f);

    auto tf = estimator(TransferAveraging::infinite());
    tf.push(reference, measurement);

    std::vector<float> magnitude(tf.bins());
    tf.write_magnitude_db(magnitude);
    const auto [first, last] = usable(tf);
    for (std::size_t bin = first; bin < last; ++bin) {
        ASSERT_LT(std::abs(magnitude[bin] + 6.0206f), 0.01f) << "bin " << bin;
    }
}

// A pure delay is flat in magnitude and linear in unwrapped phase, with a slope
// set by the delay. This is the property the delay finder will later exploit,
// and the sign convention matters: a later measurement means negative phase.
TEST(TransferFunction, APureDelayGivesFlatMagnitudeAndLinearPhase) {
    constexpr std::size_t kDelay = 32;
    const auto reference = pink(kSize * 32, 4);
    const auto measurement = delayed(reference, kDelay);

    auto tf = estimator(TransferAveraging::infinite());
    tf.push(reference, measurement);

    std::vector<float> magnitude(tf.bins());
    std::vector<float> phase(tf.bins());
    tf.write_magnitude_db(magnitude);
    tf.write_phase_degrees(phase);
    unwrap_phase_degrees(phase);

    const auto band = usable(tf);
    for (std::size_t bin = band.first; bin < band.second; ++bin) {
        ASSERT_LT(std::abs(magnitude[bin]), 0.2f)
            << "delay must not change magnitude: bin " << bin << " = " << magnitude[bin] << " dB";
    }

    // Phase slope in degrees per hertz should be -360 * delay / rate.
    const std::size_t low = band.first;
    const std::size_t high = band.second - 1;
    const float measured_slope =
        (phase[high] - phase[low]) / (tf.bin_frequency(high) - tf.bin_frequency(low));
    const float expected_slope = -360.0f * static_cast<float>(kDelay) / kRate;
    EXPECT_LT(std::abs(measured_slope - expected_slope), 0.001f)
        << "slope " << measured_slope << " deg/Hz, expected " << expected_slope;
}

// Uncorrelated signals must show low coherence. This is the metric's whole
// purpose: telling the user which parts of a curve to believe.
TEST(TransferFunction, UncorrelatedSignalsHaveLowCoherence) {
    const auto reference = pink(kSize * 64, 5);
    const auto measurement = pink(kSize * 64, 6);

    auto tf = estimator(TransferAveraging::infinite());
    tf.push(reference, measurement);

    std::vector<float> coherence(tf.bins());
    tf.write_coherence(coherence);

    const float mean = mean_over(coherence, usable(tf));
    EXPECT_LT(mean, 0.2f) << "uncorrelated mean coherence was " << mean;
}

// Signal plus independent noise sits between the two extremes, and more noise
// must lower it.
TEST(TransferFunction, CoherenceFallsAsNoiseIsAdded) {
    const auto reference = pink(kSize * 64, 7);
    const auto noise = pink(kSize * 64, 8);

    const auto mean_coherence = [&](float noise_gain) {
        std::vector<float> measurement(reference.size());
        for (std::size_t n = 0; n < measurement.size(); ++n) {
            measurement[n] = reference[n] + noise[n] * noise_gain;
        }
        auto tf = estimator(TransferAveraging::infinite());
        tf.push(reference, measurement);
        std::vector<float> coherence(tf.bins());
        tf.write_coherence(coherence);
        return mean_over(coherence, usable(tf));
    };

    const float clean = mean_coherence(0.05f);
    const float noisy = mean_coherence(1.0f);
    EXPECT_GT(clean, 0.9f) << "nearly clean should be coherent, got " << clean;
    EXPECT_LT(noisy, clean - 0.2f) << "noise must reduce coherence: " << clean << " -> " << noisy;
}

// Documents the trap rather than hiding it: one frame is always perfectly
// coherent by construction, whatever the signals are.
TEST(TransferFunction, ASingleFrameReportsCoherenceOfOneEvenForNoise) {
    const auto reference = pink(kSize, 9);
    const auto measurement = pink(kSize, 10);

    auto tf = estimator(TransferAveraging::infinite());
    tf.push(reference, measurement);
    EXPECT_EQ(tf.frames(), 1u);

    std::vector<float> coherence(tf.bins());
    tf.write_coherence(coherence);
    const auto [first, last] = usable(tf);
    for (std::size_t bin = first; bin < last; ++bin) {
        ASSERT_GT(coherence[bin], 0.99f)
            << "single-frame coherence should be 1, got " << coherence[bin];
    }
}

TEST(TransferFunction, CoherenceAlwaysLiesBetweenZeroAndOne) {
    const auto reference = pink(kSize * 16, 11);
    const auto measurement = pink(kSize * 16, 12);
    auto tf = estimator(TransferAveraging::infinite());
    tf.push(reference, measurement);

    std::vector<float> coherence(tf.bins());
    tf.write_coherence(coherence);
    for (std::size_t bin = 0; bin < coherence.size(); ++bin) {
        ASSERT_GE(coherence[bin], 0.0f) << "bin " << bin << " coherence out of range";
        ASSERT_LE(coherence[bin], 1.0f) << "bin " << bin << " coherence out of range";
    }
}

// Where the reference has no energy, magnitude must floor rather than reading 0
// dB, which would look like a flat response.
TEST(TransferFunction, SilentReferenceBinsFloorRatherThanReadingFlat) {
    auto tf = estimator(TransferAveraging::infinite());
    const std::vector<float> silence(kSize * 4, 0.0f);
    tf.push(silence, silence);

    std::vector<float> magnitude(tf.bins());
    tf.write_magnitude_db(magnitude);
    EXPECT_TRUE(std::all_of(magnitude.begin(), magnitude.end(),
                            [](float m) { return m <= kMagnitudeFloorDb + 1e-3f; }));
    EXPECT_TRUE(
        std::all_of(magnitude.begin(), magnitude.end(), [](float m) { return std::isfinite(m); }));
}

TEST(TransferFunction, ChunkingDoesNotChangeTheResult) {
    const auto reference = pink(kSize * 8, 13);
    const auto measurement = scaled(reference, 0.7f);

    auto whole = estimator(TransferAveraging::infinite());
    whole.push(reference, measurement);

    auto chunked = estimator(TransferAveraging::infinite());
    constexpr std::size_t kChunk = 97;
    for (std::size_t start = 0; start < reference.size(); start += kChunk) {
        const std::size_t count = std::min(kChunk, reference.size() - start);
        chunked.push(std::span<const float>(reference).subspan(start, count),
                     std::span<const float>(measurement).subspan(start, count));
    }

    EXPECT_EQ(whole.frames(), chunked.frames());
    std::vector<float> a(whole.bins());
    std::vector<float> b(chunked.bins());
    whole.write_magnitude_db(a);
    chunked.write_magnitude_db(b);
    for (std::size_t bin = 0; bin < a.size(); ++bin) {
        ASSERT_LT(std::abs(a[bin] - b[bin]), 1e-3f)
            << "bin " << bin << ": " << a[bin] << " vs " << b[bin];
    }
}

TEST(TransferFunction, ResetClearsEverything) {
    const auto signal = pink(kSize * 4, 14);
    auto tf = estimator(TransferAveraging::infinite());
    tf.push(signal, signal);
    EXPECT_GT(tf.frames(), 0u);

    tf.reset();
    EXPECT_EQ(tf.frames(), 0u);
    std::vector<float> coherence(tf.bins(), 1.0f);
    tf.write_coherence(coherence);
    EXPECT_TRUE(std::all_of(coherence.begin(), coherence.end(), [](float c) { return c == 0.0f; }));
}

TEST(TransferFunctionDeathTest, MismatchedLengthsAreRejected) {
    auto tf = estimator(TransferAveraging::infinite());
    const std::vector<float> reference(100, 0.0f);
    const std::vector<float> measurement(99, 0.0f);
    EXPECT_DEATH(tf.push(reference, measurement), "sample aligned");
}

TEST(UnwrapPhaseDegrees, RemovesTheJumps) {
    std::vector<float> phase = {170.0f, 179.0f, -179.0f, -170.0f, -179.0f, 179.0f, 170.0f};
    unwrap_phase_degrees(phase);
    // Should rise monotonically past 180 then come back, with no 360 steps.
    for (std::size_t i = 1; i < phase.size(); ++i) {
        EXPECT_LT(std::abs(phase[i] - phase[i - 1]), 180.0f)
            << "unwrap left a jump: " << phase[i - 1] << " -> " << phase[i];
    }
    EXPECT_LT(std::abs(phase[2] - 181.0f), 1e-3f) << "got " << phase[2];
}

TEST(UnwrapPhaseDegrees, AnEmptyOrSingleValueIsHarmless) {
    std::vector<float> empty;
    unwrap_phase_degrees(empty);
    std::vector<float> single = {42.0f};
    unwrap_phase_degrees(single);
    EXPECT_EQ(single, std::vector<float>{42.0f});
}

// Exponential averaging must track a change rather than holding the old answer
// forever.
TEST(TransferFunction, ExponentialAveragingFollowsAChange) {
    const auto reference = pink(kSize * 32, 15);
    const auto quiet = scaled(reference, 0.1f);
    const auto loud = scaled(reference, 2.0f);

    auto tf = estimator(TransferAveraging::exponential(0.3f));
    tf.push(reference, quiet);
    std::vector<float> magnitude(tf.bins());
    tf.write_magnitude_db(magnitude);
    const float before = magnitude[usable(tf).first];

    tf.push(reference, loud);
    tf.write_magnitude_db(magnitude);
    const float after = magnitude[usable(tf).first];

    EXPECT_LT(before, -15.0f) << "expected about -20 dB, got " << before;
    EXPECT_GT(after, before + 10.0f) << "should have tracked upward: " << before << " -> " << after;
}

}  // namespace
}  // namespace analyzer::dsp
