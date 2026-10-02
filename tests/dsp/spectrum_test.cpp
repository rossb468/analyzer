// Ported from crates/analyzer-dsp/src/spectrum.rs.

#include "dsp/spectrum.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "support/signals.hpp"

namespace analyzer::dsp {
namespace {

using analyzer::test::kTau;

constexpr float kRate = 48'000.0f;
constexpr std::size_t kSize = 4096;

// `len` samples of a sine of `bin` whole cycles per kSize samples.
std::vector<float> sine(std::size_t bin, float amplitude, std::size_t len) {
    return test::sine(len, static_cast<float>(bin), kSize, amplitude);
}

SpectrumAnalyzer analyzer(WindowKind window, Averaging averaging) {
    return SpectrumAnalyzer(SpectrumConfig{
        .sample_rate = kRate,
        .size = kSize,
        .window = window,
        .overlap = Overlap::None,
        .averaging = averaging,
    });
}

TEST(Overlap, HopFollowsOverlap) {
    EXPECT_EQ(overlap_hop(Overlap::None, 4096), 4096u);
    EXPECT_EQ(overlap_hop(Overlap::Half, 4096), 2048u);
    EXPECT_EQ(overlap_hop(Overlap::ThreeQuarters, 4096), 1024u);
    EXPECT_EQ(overlap_hop(Overlap::SevenEighths, 4096), 512u);
    // Never zero, however small the frame.
    EXPECT_EQ(overlap_hop(Overlap::SevenEighths, 4), 1u);
}

TEST(SpectrumAnalyzer, FullScaleSineReadsZeroDbFs) {
    auto a = analyzer(WindowKind::hann(), Averaging::none());
    a.push(sine(64, 1.0f, kSize));

    std::vector<float> db(a.bins());
    a.write_db_fs(db);

    const std::size_t peak = test::peak_bin(a.power());
    EXPECT_EQ(peak, 64u);
    EXPECT_LT(std::abs(db[peak]), 0.01f) << "expected 0 dBFS, got " << db[peak];
}

TEST(SpectrumAnalyzer, HalfAmplitudeSineReadsMinusSixDb) {
    auto a = analyzer(WindowKind::hann(), Averaging::none());
    a.push(sine(64, 0.5f, kSize));

    std::vector<float> db(a.bins());
    a.write_db_fs(db);

    const float level = db[test::peak_bin(a.power())];
    EXPECT_LT(std::abs(level + 6.0206f), 0.01f) << "expected -6.02 dBFS, got " << level;
}

// The end-to-end check on the window correction factors: the same tone must
// read the same level through every window. This is what catches a wrong
// coherent gain, which otherwise just offsets everything by a constant.
TEST(SpectrumAnalyzer, MeasuredLevelIsIndependentOfWindow) {
    for (const auto window :
         {WindowKind::rectangular(), WindowKind::hann(), WindowKind::blackman_harris(),
          WindowKind::flat_top(), WindowKind::tukey(0.25f)}) {
        auto a = analyzer(window, Averaging::none());
        a.push(sine(64, 0.5f, kSize));

        std::vector<float> db(a.bins());
        a.write_db_fs(db);
        const float level = db[64];

        EXPECT_LT(std::abs(level + 6.0206f), 0.05f)
            << to_string(window.shape) << " read " << level << " dBFS, expected -6.02";
    }
}

// With no taper the power spectrum must sum to the mean square of the input.
TEST(SpectrumAnalyzer, UnwindowedPowerSumsToMeanSquare) {
    auto a = analyzer(WindowKind::rectangular(), Averaging::none());
    std::vector<float> input(kSize);
    for (std::size_t n = 0; n < kSize; ++n) {
        const float t = static_cast<float>(n) / static_cast<float>(kSize);
        input[n] = 0.1f + 0.3f * std::sin(kTau * 37.0f * t) + 0.2f * std::cos(kTau * 211.0f * t);
    }

    a.push(input);

    float sum_of_squares = 0.0f;
    for (const float x : input) {
        sum_of_squares += x * x;
    }
    const float mean_square = sum_of_squares / static_cast<float>(kSize);
    const float total = std::accumulate(a.power().begin(), a.power().end(), 0.0f);
    const float error = std::abs(mean_square - total) / mean_square;
    EXPECT_LT(error, 1e-4f) << "mean square " << mean_square << ", spectral total " << total
                            << ", error " << error;
}

TEST(SpectrumAnalyzer, FramesAreCountedPerHop) {
    SpectrumAnalyzer a(SpectrumConfig{
        .sample_rate = kRate,
        .size = kSize,
        .window = WindowKind::hann(),
        .overlap = Overlap::ThreeQuarters,
        .averaging = Averaging::none(),
    });
    EXPECT_EQ(a.hop(), kSize / 4);

    // First frame needs a full buffer; each later frame needs one hop.
    EXPECT_EQ(a.push(std::vector<float>(kSize, 0.0f)), 1u);
    EXPECT_EQ(a.push(std::vector<float>(kSize / 4, 0.0f)), 1u);
    EXPECT_EQ(a.push(std::vector<float>(kSize, 0.0f)), 4u);
}

// Chunking must not change the answer: the audio driver's buffer size is not
// the analyzer's business.
TEST(SpectrumAnalyzer, StreamingInChunksMatchesOneBlock) {
    const auto input = sine(97, 0.3f, kSize * 4);

    auto whole = analyzer(WindowKind::hann(), Averaging::infinite());
    whole.push(input);

    auto chunked = analyzer(WindowKind::hann(), Averaging::infinite());
    constexpr std::size_t kChunk = 113;
    for (std::size_t start = 0; start < input.size(); start += kChunk) {
        const std::size_t count = std::min(kChunk, input.size() - start);
        chunked.push(std::span<const float>(input).subspan(start, count));
    }

    EXPECT_EQ(whole.frames(), chunked.frames());
    for (std::size_t k = 0; k < whole.bins(); ++k) {
        ASSERT_LT(std::abs(whole.power()[k] - chunked.power()[k]), 1e-9f)
            << "bin " << k << ": " << whole.power()[k] << " vs " << chunked.power()[k];
    }
}

TEST(SpectrumAnalyzer, PeakHoldRetainsTheMaximum) {
    auto a = analyzer(WindowKind::hann(), Averaging::peak_hold());
    a.push(sine(64, 1.0f, kSize));
    const float loud = a.power()[64];

    a.push(sine(64, 0.01f, kSize));
    EXPECT_LT(std::abs(a.power()[64] - loud), 1e-9f) << "peak hold must not decay";
}

TEST(SpectrumAnalyzer, LinearAveragingHoldsAfterItsFrameCount) {
    auto a = analyzer(WindowKind::hann(), Averaging::linear(2));
    a.push(sine(64, 1.0f, kSize));
    a.push(sine(64, 1.0f, kSize));
    const float held = a.power()[64];
    EXPECT_EQ(a.frames(), 2u);

    // Further frames are ignored once the average is complete.
    a.push(sine(64, 0.001f, kSize));
    EXPECT_LT(std::abs(a.power()[64] - held), 1e-9f);
}

TEST(SpectrumAnalyzer, ExponentialAveragingConvergesTowardsTheInput) {
    auto a = analyzer(WindowKind::hann(), Averaging::exponential(0.5f));
    a.push(sine(64, 1.0f, kSize));
    const float start = a.power()[64];

    // Silence: the average should decay towards zero, not jump there.
    a.push(std::vector<float>(kSize, 0.0f));
    const float after = a.power()[64];
    EXPECT_LT(after, start) << "should decay: " << start << " -> " << after;
    EXPECT_GT(after, 0.0f) << "should not snap to zero: " << after;
}

TEST(Averaging, ExponentialFromTimeConstantIsInRange) {
    const Averaging averaging = Averaging::exponential_over(1.0f, 46.875f);
    ASSERT_EQ(averaging.mode, Averaging::Mode::Exponential);
    EXPECT_GT(averaging.alpha, 0.0f);
    EXPECT_LT(averaging.alpha, 1.0f);

    // Degenerate inputs fall back to no averaging rather than dividing by zero.
    EXPECT_EQ(Averaging::exponential_over(0.0f, 100.0f), Averaging::none());
    EXPECT_EQ(Averaging::exponential_over(1.0f, 0.0f), Averaging::none());
}

TEST(SpectrumAnalyzer, ResetClearsAverageAndPartialFrame) {
    auto a = analyzer(WindowKind::hann(), Averaging::infinite());
    a.push(sine(64, 1.0f, kSize * 2));
    EXPECT_GT(a.frames(), 0u);

    a.reset();
    EXPECT_EQ(a.frames(), 0u);
    EXPECT_TRUE(std::all_of(a.power().begin(), a.power().end(), [](float p) { return p == 0.0f; }));
}

TEST(SpectrumAnalyzer, BinGeometryMatchesSampleRate) {
    const auto a = analyzer(WindowKind::hann(), Averaging::none());
    EXPECT_EQ(a.bins(), kSize / 2 + 1);
    EXPECT_LT(std::abs(a.bin_spacing_hz() - kRate / static_cast<float>(kSize)), 1e-6f);
    EXPECT_LT(std::abs(a.bin_frequency(100) - 100.0f * kRate / static_cast<float>(kSize)), 1e-3f);

    // Hann's ENBW is 1.5 bins.
    EXPECT_LT(std::abs(a.enbw_hz() - 1.5f * a.bin_spacing_hz()), 1e-3f);
}

TEST(SpectrumAnalyzer, EmptyPushIsANoOp) {
    auto a = analyzer(WindowKind::hann(), Averaging::none());
    EXPECT_EQ(a.push({}), 0u);
    EXPECT_EQ(a.frames(), 0u);
}

TEST(SpectrumAnalyzer, SilentInputReadsAtTheFloor) {
    auto a = analyzer(WindowKind::hann(), Averaging::none());
    a.push(std::vector<float>(kSize, 0.0f));

    std::vector<float> db(a.bins());
    a.write_db_fs(db);
    EXPECT_TRUE(std::all_of(db.begin(), db.end(), [](float d) { return d <= kSpectrumFloorDb + 1e-3f; }));
    EXPECT_TRUE(std::all_of(db.begin(), db.end(), [](float d) { return std::isfinite(d); }))
        << "floor must be finite";
}

}  // namespace
}  // namespace analyzer::dsp
