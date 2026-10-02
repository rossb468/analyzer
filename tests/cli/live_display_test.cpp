#include "cli/live_display.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "audio/target.hpp"

#if ANALYZER_AUDIO_COREAUDIO
#include "cli/live.hpp"
#endif

namespace analyzer::cli {
namespace {

TEST(LiveDisplay, BarSpansTheFullRange) {
    EXPECT_TRUE(bar(-200.0f).ends_with("..]"));
    EXPECT_NE(bar(-90.0f).find(".."), std::string::npos);
    EXPECT_NE(bar(0.0f).find("##"), std::string::npos);
    // Clamps rather than overflowing.
    EXPECT_EQ(bar(50.0f).size(), bar(-50.0f).size());
}

// The Rust asserted only the ends of the range; the exact shape is what the
// meter shows, so check it too.
TEST(LiveDisplay, BarFillsInProportionToTheLevel) {
    EXPECT_EQ(bar(-90.0f), "[" + std::string(40, '.') + "]");
    EXPECT_EQ(bar(0.0f), "[" + std::string(40, '#') + "]");
    EXPECT_EQ(bar(-45.0f), "[" + std::string(20, '#') + std::string(20, '.') + "]");
    EXPECT_EQ(bar(std::nanf("")), "[" + std::string(40, '.') + "]");
}

TEST(LiveDisplay, BroadbandOfSilenceIsTheFloor) {
    const std::vector<float> bins(128, -200.0f);
    EXPECT_LT(broadband_db(bins), -100.0f);
}

// A single full-scale bin means a full-scale broadband level.
TEST(LiveDisplay, BroadbandOfOneFullScaleBinIsZeroDb) {
    std::vector<float> bins(128, -200.0f);
    bins[10] = 0.0f;
    EXPECT_LT(std::abs(broadband_db(bins)), 0.01f) << broadband_db(bins);
}

// Two equal tones carry twice the power: +3 dB, not +6.
TEST(LiveDisplay, TwoEqualBinsAddThreeDecibels) {
    std::vector<float> bins(128, -200.0f);
    bins[10] = -20.0f;
    bins[50] = -20.0f;
    const float total = broadband_db(bins);
    // Doubling power is +10*log10(2) = +3.0103 dB, so -20 becomes -16.9897.
    // Rounding that to -17.0 is what made this fail the first time.
    const float expected = -20.0f + 10.0f * std::log10(2.0f);
    EXPECT_LT(std::abs(total - expected), 0.001f) << "got " << total << ", want " << expected;
}

TEST(LiveDisplay, PeakBinFindsTheLoudest) {
    std::vector<float> bins(16, -80.0f);
    bins[7] = -12.0f;
    EXPECT_EQ(peak_bin(bins), (std::pair<std::size_t, float>{7, -12.0f}));
}

TEST(LiveDisplay, PeakOfEmptyDoesNotPanic) {
    EXPECT_EQ(peak_bin({}).first, 0u);
}

#if ANALYZER_AUDIO_COREAUDIO
// Enumeration must work without any capture permission.
TEST(LiveDisplay, ListDevicesSucceeds) {
    const std::string text = list_devices();
    EXPECT_NE(text.find("Audio devices"), std::string::npos);
    EXPECT_NE(text.find("uid:"), std::string::npos);
}
#endif

}  // namespace
}  // namespace analyzer::cli
