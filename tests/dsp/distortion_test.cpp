// Ported from crates/analyzer-dsp/src/distortion.rs.

#include "dsp/distortion.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "dsp/complex.hpp"
#include "dsp/fft.hpp"
#include "dsp/window.hpp"
#include "support/signals.hpp"

namespace analyzer::dsp {
namespace {

using analyzer::test::kTau;

constexpr float kRate = 48'000.0f;
constexpr std::size_t kSize = 8192;

using Harmonics = std::initializer_list<std::pair<std::uint32_t, float>>;

// Stand-in for the Rust SpectrumAnalyzer, which is ported separately: a Welch
// average of 50%-overlapped, windowed frames, in the analyzer's own power
// convention (mean-square per bin, a bin-centred sine of amplitude A reads
// A^2 / 2). Averaging is the incremental mean the analyzer uses for
// Averaging::Infinite.
std::vector<float> average_power(std::span<const float> samples, std::size_t size,
                                 WindowKind kind) {
    const Window window(kind, size);
    RealFft fft(size);
    const float scale = 1.0f / (static_cast<float>(size) * window.coherent_gain());
    const std::size_t hop = size / 2;

    std::vector<float> windowed(size);
    std::vector<Complex32> spectrum(fft.bins());
    std::vector<float> power(fft.bins(), 0.0f);
    const std::size_t last = fft.bins() - 1;

    std::size_t frames = 0;
    for (std::size_t start = 0; start + size <= samples.size(); start += hop) {
        window.apply_to(samples.subspan(start, size), windowed);
        fft.forward(windowed, spectrum);
        ++frames;
        const auto n = static_cast<float>(frames);
        for (std::size_t k = 0; k < power.size(); ++k) {
            // DC and Nyquist are real and unpaired; every bin between them
            // stands for a conjugate pair.
            const float magnitude = std::abs(spectrum[k]) * scale;
            const float frame_power =
                (k == 0 || k == last) ? magnitude * magnitude : 2.0f * magnitude * magnitude;
            power[k] += (frame_power - power[k]) / n;
        }
    }
    return power;
}

struct Spectrum {
    std::vector<float> power;
    float bin_spacing_hz;
};

// Synthesise a tone with specified harmonics, measure it, and return the power
// spectrum. Going through a real analysis rather than a synthetic spectrum
// means the test exercises windowing and leakage too.
Spectrum spectrum_of(float fundamental_hz, Harmonics harmonics, float noise) {
    const std::size_t count = kSize * 8;
    std::vector<float> samples(count, 0.0f);
    std::uint64_t rng = 0x1234'5678;

    for (std::size_t index = 0; index < count; ++index) {
        const float t = static_cast<float>(index) / kRate;
        float value = 0.5f * std::sin(kTau * fundamental_hz * t);
        for (const auto& [order, amplitude] : harmonics) {
            value +=
                0.5f * amplitude * std::sin(kTau * fundamental_hz * static_cast<float>(order) * t);
        }
        if (noise > 0.0f) {
            rng ^= rng >> 12;
            rng ^= rng << 25;
            rng ^= rng >> 27;
            const float r =
                static_cast<float>((rng * 0x2545'F491'4F6C'DD1D) >> 40) / 8'388'608.0f - 1.0f;
            value += r * noise;
        }
        samples[index] = value;
    }

    // Blackman-Harris: harmonics can sit 100 dB down, and Hann's sidelobes would
    // bury them in the fundamental's own leakage.
    return {average_power(samples, kSize, WindowKind::blackman_harris()),
            kRate / static_cast<float>(kSize)};
}

std::optional<Distortion> measure(const Spectrum& s, std::optional<float> fundamental_hz = {},
                                  const DistortionConfig& config = {}) {
    return analyse_distortion(s.power, s.bin_spacing_hz, fundamental_hz, config);
}

// A clean sine has no harmonics, so THD must be tiny rather than merely small -
// this is the measurement's own noise floor.
TEST(Distortion, ACleanSineMeasuresNearZeroDistortion) {
    const auto d = measure(spectrum_of(1000.0f, {}, 0.0f));
    ASSERT_TRUE(d);

    EXPECT_LT(d->thd_percent, 0.01f) << "clean sine measured " << d->thd_percent << "% THD";
    EXPECT_LT(std::abs(d->fundamental_hz - 1000.0f), 5.0f) << d->fundamental_hz;
}

// The defining case: a second harmonic 40 dB down is exactly 1% THD.
TEST(Distortion, ASingleHarmonicGivesTheExpectedPercentage) {
    // -40 dB is an amplitude ratio of 0.01.
    const auto d = measure(spectrum_of(1000.0f, {{2, 0.01f}}, 0.0f));
    ASSERT_TRUE(d);

    EXPECT_LT(std::abs(d->thd_percent - 1.0f), 0.05f)
        << "expected 1% THD, got " << d->thd_percent << "%";
    EXPECT_LT(std::abs(d->thd_db - -40.0f), 0.5f) << "expected -40 dB, got " << d->thd_db;

    const auto h2 = d->harmonic(2);
    ASSERT_TRUE(h2);
    EXPECT_LT(std::abs(h2->percent - 1.0f), 0.05f);
    EXPECT_LT(std::abs(h2->hz - 2000.0f), 10.0f) << "H2 at " << h2->hz;
}

// Harmonics combine in power, so two equal ones are sqrt(2) times one.
TEST(Distortion, HarmonicsCombineAsTheRootSumOfSquares) {
    const auto d = measure(spectrum_of(1000.0f, {{2, 0.01f}, {3, 0.01f}}, 0.0f));
    ASSERT_TRUE(d);

    const float expected = std::sqrt(0.01f * 0.01f + 0.01f * 0.01f) * 100.0f;
    EXPECT_LT(std::abs(d->thd_percent - expected), 0.1f)
        << "expected " << expected << "%, got " << d->thd_percent << "%";
}

TEST(Distortion, EachHarmonicIsReportedAtItsOwnLevel) {
    const auto d = measure(spectrum_of(1000.0f, {{2, 0.01f}, {3, 0.003f}, {5, 0.001f}}, 0.0f));
    ASSERT_TRUE(d);

    for (const auto& [order, amplitude] : Harmonics{{2, 0.01f}, {3, 0.003f}, {5, 0.001f}}) {
        const auto h = d->harmonic(order);
        ASSERT_TRUE(h) << "H" << order << " should have been found";
        const float expected = amplitude * 100.0f;
        EXPECT_LT(std::abs(h->percent - expected), expected * 0.15f)
            << "H" << order << ": got " << h->percent << "%, expected " << expected << "%";
        EXPECT_LT(std::abs(h->hz - 1000.0f * static_cast<float>(order)), 15.0f)
            << "H" << order << " at " << h->hz;
    }
}

// Odd-order-only distortion is what symmetric clipping produces, and the
// analysis must not invent even orders that are not there.
TEST(Distortion, AbsentOrdersAreNotInvented) {
    const auto d = measure(spectrum_of(1000.0f, {{3, 0.02f}, {5, 0.01f}}, 0.0f));
    ASSERT_TRUE(d);

    const auto h2 = d->harmonic(2);
    ASSERT_TRUE(h2);
    EXPECT_LT(h2->percent, 0.05f) << "H2 should be negligible, got " << h2->percent << "%";
    ASSERT_TRUE(d->harmonic(3));
    EXPECT_GT(d->harmonic(3)->percent, 1.0f);
    ASSERT_TRUE(d->harmonic(5));
    EXPECT_GT(d->harmonic(5)->percent, 0.5f);
}

// THD ignores noise; THD+N does not.
//
// The two combine in quadrature, which is worth stating because it makes the
// numbers unintuitive: 1% distortion with 0.33% noise gives 1.05% THD+N, not
// 1.33%. An earlier version of this test used noise that quiet and then
// asserted a 50% increase, which the physics does not allow.
TEST(Distortion, ThdPlusNoiseExceedsThdWhenNoiseIsPresent) {
    const auto quiet = measure(spectrum_of(1000.0f, {{2, 0.01f}}, 0.0f));
    const auto noisy = measure(spectrum_of(1000.0f, {{2, 0.01f}}, 0.02f));
    ASSERT_TRUE(quiet);
    ASSERT_TRUE(noisy);

    // Distortion itself is unchanged - that is the point of measuring both.
    EXPECT_LT(std::abs(noisy->thd_percent - quiet->thd_percent), 0.1f)
        << "noise must not change THD: " << quiet->thd_percent << "% vs " << noisy->thd_percent
        << "%";
    EXPECT_GT(noisy->thd_n_percent, noisy->thd_percent * 1.5f)
        << "THD+N " << noisy->thd_n_percent << "% should clearly exceed THD " << noisy->thd_percent
        << "%";
    EXPECT_GT(noisy->noise_floor_db, kDistortionFloorDb);
    EXPECT_GT(noisy->noise_floor_db, quiet->noise_floor_db + 6.0f)
        << "the measured floor should rise with the noise";
}

TEST(Distortion, TheTwoFiguresConvergeWithoutNoise) {
    const auto d = measure(spectrum_of(1000.0f, {{2, 0.02f}}, 0.0f));
    ASSERT_TRUE(d);

    EXPECT_LT(std::abs(d->thd_n_percent - d->thd_percent), d->thd_percent * 0.5f)
        << "without noise they should be close: " << d->thd_percent << "% vs " << d->thd_n_percent
        << "%";
}

// The aliasing trap. A 5 kHz fundamental at 48 kHz has H5 at 25 kHz, above
// Nyquist, where it would fold back to 23 kHz and be read as a real product. It
// must be excluded and the exclusion reported.
TEST(Distortion, HarmonicsAboveNyquistAreExcludedAndCounted) {
    const auto d = measure(spectrum_of(5000.0f, {{2, 0.01f}}, 0.0f));
    ASSERT_TRUE(d);

    // Nyquist is 24 kHz, so orders 5 through 10 are out.
    EXPECT_EQ(d->orders_above_nyquist, 6u) << "H5..H10 should be excluded";
    EXPECT_EQ(d->highest_order(), 4u);
    EXPECT_TRUE(std::all_of(d->harmonics.begin(), d->harmonics.end(),
                            [](const Harmonic& h) { return h.hz < 24'000.0f; }));
    EXPECT_NE(d->thd_label().find("to H4"), std::string::npos);
}

TEST(Distortion, ALowFundamentalKeepsEveryOrder) {
    const auto d = measure(spectrum_of(200.0f, {{2, 0.01f}}, 0.0f));
    ASSERT_TRUE(d);
    EXPECT_EQ(d->orders_above_nyquist, 0u);
    EXPECT_EQ(d->highest_order(), 10u);
    EXPECT_EQ(d->thd_label().find("to H"), std::string::npos);
}

// Summing across the lobe rather than reading one bin is what keeps the answer
// stable as the tone moves between bin centres.
TEST(Distortion, TheReadingIsStableAcrossBinBoundaries) {
    const float spacing = kRate / static_cast<float>(kSize);
    std::vector<float> readings;
    // Walk a tone across one bin in fifths.
    for (int step = 0; step < 5; ++step) {
        const float hz = 1000.0f + spacing * static_cast<float>(step) / 5.0f;
        const auto d = measure(spectrum_of(hz, {{2, 0.01f}}, 0.0f));
        ASSERT_TRUE(d);
        readings.push_back(d->thd_percent);
    }
    const float max = *std::max_element(readings.begin(), readings.end());
    const float min = *std::min_element(readings.begin(), readings.end());
    EXPECT_LT(max - min, 0.15f) << "THD swung from " << min << "% to " << max
                                << "% across one bin: " << ::testing::PrintToString(readings);
}

// Supplying the fundamental matters when a harmonic rivals it, because the
// loudest-bin heuristic would latch onto the wrong peak.
TEST(Distortion, AnExplicitFundamentalOverridesTheLoudestBin) {
    // A second harmonic louder than the fundamental - severe, but real in a
    // badly driven system.
    const auto s = spectrum_of(1000.0f, {{2, 2.0f}}, 0.0f);

    const auto guessed = measure(s);
    ASSERT_TRUE(guessed);
    EXPECT_LT(std::abs(guessed->fundamental_hz - 2000.0f), 20.0f)
        << "without a hint it should latch onto the loudest peak, got " << guessed->fundamental_hz;

    const auto told = measure(s, 1000.0f);
    ASSERT_TRUE(told);
    EXPECT_LT(std::abs(told->fundamental_hz - 1000.0f), 20.0f)
        << "with a hint it should use it, got " << told->fundamental_hz;
    EXPECT_GT(told->thd_percent, 100.0f) << "H2 above the fundamental is >100% THD";
}

TEST(Distortion, MaxOrderIsRespected) {
    DistortionConfig config;
    config.max_order = 3;
    const auto d =
        measure(spectrum_of(1000.0f, {{2, 0.01f}, {3, 0.01f}, {4, 0.01f}}, 0.0f), {}, config);
    ASSERT_TRUE(d);

    EXPECT_EQ(d->highest_order(), 3u);
    EXPECT_FALSE(d->harmonic(4)) << "H4 was not asked for";
}

// The FFI hands this module power recovered from the published decibels rather
// than the analyzer's own power array, because the frame carries dB. That
// inversion has to be lossless enough not to move the answer.
TEST(Distortion, PowerRecoveredFromDecibelsGivesTheSameAnswer) {
    const auto s = spectrum_of(1000.0f, {{2, 0.01f}, {3, 0.003f}}, 0.0f);

    // Exactly what the engine publishes, then exactly what the FFI does to get
    // back: db = 10*log10(2*power), power = 10^(db/10)/2.
    std::vector<float> round_tripped;
    for (const float p : s.power) {
        const float db = p > 0.0f ? std::max(10.0f * std::log10(2.0f * p), -200.0f) : -200.0f;
        round_tripped.push_back(std::pow(10.0f, db / 10.0f) / 2.0f);
    }

    const auto direct = measure(s);
    const auto via_db = measure({round_tripped, s.bin_spacing_hz});
    ASSERT_TRUE(direct);
    ASSERT_TRUE(via_db);

    EXPECT_LT(std::abs(direct->thd_percent - via_db->thd_percent), 0.01f)
        << "THD moved through the round trip: " << direct->thd_percent << "% vs "
        << via_db->thd_percent << "%";
    EXPECT_LT(std::abs(direct->fundamental_hz - via_db->fundamental_hz), 1.0f);
    EXPECT_EQ(direct->harmonics.size(), via_db->harmonics.size());
}

TEST(Distortion, DegenerateInputIsRefused) {
    const DistortionConfig config;
    const std::vector<float> ones(100, 1.0f);
    const std::vector<float> zeros(100, 0.0f);
    EXPECT_FALSE(analyse_distortion({}, 5.86f, {}, config));
    EXPECT_FALSE(analyse_distortion(ones, 0.0f, {}, config));
    EXPECT_FALSE(analyse_distortion(zeros, 5.86f, {}, config));
}

TEST(Distortion, SilenceProducesNoMeasurement) {
    const std::vector<float> power(1024, 0.0f);
    EXPECT_FALSE(analyse_distortion(power, 5.86f, {}, DistortionConfig{}));
}

TEST(Distortion, EveryReportedNumberIsFinite) {
    const auto d = measure(spectrum_of(1000.0f, {{2, 0.01f}, {3, 0.001f}}, 0.0001f));
    ASSERT_TRUE(d);

    EXPECT_TRUE(std::isfinite(d->thd_percent) && std::isfinite(d->thd_db));
    EXPECT_TRUE(std::isfinite(d->thd_n_percent) && std::isfinite(d->thd_n_db));
    EXPECT_TRUE(std::isfinite(d->fundamental_db) && std::isfinite(d->noise_floor_db));
    for (const Harmonic& h : d->harmonics) {
        EXPECT_TRUE(std::isfinite(h.level_db) && std::isfinite(h.relative_db) &&
                    std::isfinite(h.percent));
    }
}

}  // namespace
}  // namespace analyzer::dsp
