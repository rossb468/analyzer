// Ported from crates/analyzer-dsp/src/window.rs.

#include "dsp/window.hpp"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::dsp {
namespace {

constexpr std::size_t kSize = 4096;

// Published coherent gain and equivalent noise bandwidth. Getting these wrong
// offsets every level reading by a constant, which is exactly the kind of
// error that survives casual inspection.
TEST(Window, CorrectionFactorsMatchPublishedValues) {
    struct Case {
        WindowKind kind;
        float coherent_gain;
        float enbw;
    };
    const Case cases[] = {
        {WindowKind::rectangular(), 1.0f, 1.0f},
        {WindowKind::hann(), 0.5f, 1.5f},
        {WindowKind::blackman_harris(), 0.35875f, 2.0044f},
        {WindowKind::flat_top(), 0.21557895f, 3.7702f},
    };

    for (const auto& c : cases) {
        const Window w(c.kind, kSize);
        EXPECT_NEAR(w.coherent_gain(), c.coherent_gain, 1e-5f) << to_string(c.kind.shape);
        EXPECT_NEAR(w.enbw_bins(), c.enbw, 1e-3f) << to_string(c.kind.shape);
    }
}

TEST(Window, AmplitudeCorrectionIsReciprocalOfCoherentGain) {
    const Window w(WindowKind::hann(), kSize);
    EXPECT_NEAR(w.amplitude_correction(), 2.0f, 1e-5f);
}

TEST(Window, CosineWindowsPeakAtUnityMidFrame) {
    for (const auto kind :
         {WindowKind::hann(), WindowKind::blackman_harris(), WindowKind::flat_top()}) {
        const Window w(kind, kSize);
        EXPECT_NEAR(w.samples()[kSize / 2], 1.0f, 1e-5f) << to_string(kind.shape);
    }
}

TEST(Window, CosineWindowsStartNearZero) {
    // Flat-top dips slightly negative at the edges; that is correct.
    EXPECT_LT(std::abs(Window(WindowKind::hann(), kSize).samples()[0]), 1e-6f);
    EXPECT_LT(std::abs(Window(WindowKind::blackman_harris(), kSize).samples()[0]), 1e-3f);
    EXPECT_LT(std::abs(Window(WindowKind::flat_top(), kSize).samples()[0]), 1e-3f);
}

// A periodic window is symmetric about its midpoint for n = 1..N, with index 0
// being the unmatched sample. Asymmetry here means the periodic and symmetric
// definitions have been mixed up.
TEST(Window, PeriodicWindowsAreSymmetricExcludingIndexZero) {
    const Window w(WindowKind::hann(), kSize);
    const auto s = w.samples();
    for (std::size_t n = 1; n < kSize / 2; ++n) {
        ASSERT_NEAR(s[n], s[kSize - n], 1e-6f) << "asymmetry at " << n;
    }
}

TEST(Window, TukeyDegeneratesToRectangularAndHann) {
    const Window rectangular(WindowKind::rectangular(), kSize);
    const Window tukey_0(WindowKind::tukey(0.0f), kSize);
    for (std::size_t i = 0; i < kSize; ++i) {
        ASSERT_NEAR(tukey_0.samples()[i], rectangular.samples()[i], 1e-6f) << "at " << i;
    }

    const Window hann(WindowKind::hann(), kSize);
    const Window tukey_1(WindowKind::tukey(1.0f), kSize);
    for (std::size_t i = 0; i < kSize; ++i) {
        ASSERT_NEAR(tukey_1.samples()[i], hann.samples()[i], 1e-5f) << "at " << i;
    }
}

TEST(Window, TukeyAlphaIsClamped) {
    const Window low(WindowKind::tukey(-5.0f), 64);
    for (const float w : low.samples()) {
        ASSERT_NEAR(w, 1.0f, 1e-6f);
    }

    const Window high(WindowKind::tukey(5.0f), 64);
    const Window hann(WindowKind::hann(), 64);
    for (std::size_t i = 0; i < 64; ++i) {
        ASSERT_NEAR(high.samples()[i], hann.samples()[i], 1e-5f);
    }
}

TEST(Window, ApplyScalesInPlace) {
    const Window w(WindowKind::hann(), 8);
    std::vector<float> frame(8, 2.0f);
    w.apply(frame);
    for (std::size_t i = 0; i < frame.size(); ++i) {
        EXPECT_NEAR(frame[i], 2.0f * w.samples()[i], 1e-6f);
    }
}

TEST(Window, ApplyToLeavesInputAlone) {
    const Window w(WindowKind::hann(), 8);
    const std::vector<float> input(8, 2.0f);
    std::vector<float> output(8, 0.0f);
    w.apply_to(input, output);
    for (std::size_t i = 0; i < input.size(); ++i) {
        EXPECT_EQ(input[i], 2.0f);
        EXPECT_NEAR(output[i], 2.0f * w.samples()[i], 1e-6f);
    }
}

TEST(WindowDeathTest, ApplyRejectsWrongLength) {
    const Window w(WindowKind::hann(), 8);
    std::vector<float> frame(7, 0.0f);
    EXPECT_DEATH(w.apply(frame), "frame length must equal");
}

TEST(WindowDeathTest, ZeroSizeIsRejected) {
    EXPECT_DEATH(Window(WindowKind::hann(), 0), "window size must be non-zero");
}

}  // namespace
}  // namespace analyzer::dsp
