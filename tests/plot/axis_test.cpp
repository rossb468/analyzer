// Ported from crates/analyzer-plot/src/axis.rs.

#include "plot/axis.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::plot {
namespace {

TEST(FrequencyAxis, FrequencyEndpointsMapToTheEdges) {
    const FrequencyAxis axis = FrequencyAxis::audible(1000.0f);
    EXPECT_LT(std::abs(axis.freq_to_x(20.0f)), 1e-3f);
    EXPECT_NEAR(axis.freq_to_x(20000.0f), 1000.0f, 1e-3f);
}

// Each decade must occupy equal width. Three decades across 900 px is
// 300 px each, which is the whole point of a log axis.
TEST(FrequencyAxis, DecadesAreEvenlySpaced) {
    const FrequencyAxis axis(20.0f, 20000.0f, 900.0f);
    const float a = axis.freq_to_x(20.0f);
    const float b = axis.freq_to_x(200.0f);
    const float c = axis.freq_to_x(2000.0f);
    const float d = axis.freq_to_x(20000.0f);
    EXPECT_NEAR(b - a, 300.0f, 0.01f);
    EXPECT_NEAR(c - b, 300.0f, 0.01f);
    EXPECT_NEAR(d - c, 300.0f, 0.01f);
}

TEST(FrequencyAxis, FrequencyRoundTripsThroughPixels) {
    const FrequencyAxis axis = FrequencyAxis::audible(1440.0f);
    for (const float hz : {20.0f, 63.0f, 100.0f, 440.0f, 1000.0f, 4700.0f, 19000.0f}) {
        const float back = axis.x_to_freq(axis.freq_to_x(hz));
        EXPECT_LT(std::abs(back - hz) / hz, 1e-4f) << hz << " Hz came back as " << back << " Hz";
    }
}

TEST(FrequencyAxis, FrequencyOutsideTheRangeExtrapolatesRatherThanClamping) {
    const FrequencyAxis axis = FrequencyAxis::audible(1000.0f);
    EXPECT_LT(axis.freq_to_x(10.0f), 0.0f);
    EXPECT_GT(axis.freq_to_x(40000.0f), 1000.0f);
    // Zero and negatives cannot be placed at all.
    EXPECT_EQ(axis.freq_to_x(0.0f), -std::numeric_limits<float>::infinity());
}

TEST(FrequencyAxis, FrequencyTicksCoverTheExpectedDecades) {
    const FrequencyAxis axis = FrequencyAxis::audible(1000.0f);
    const auto ticks = axis.ticks();

    std::vector<float> majors;
    for (const Tick& tick : ticks) {
        if (tick.major) {
            majors.push_back(tick.value);
        }
    }
    EXPECT_EQ(majors, (std::vector<float>{100.0f, 1000.0f, 10000.0f}));

    for (const Tick& tick : ticks) {
        EXPECT_TRUE(tick.value >= 20.0f && tick.value <= 20000.0f) << tick.value;
    }
    // Ticks must be in ascending pixel order for a renderer to stride them.
    for (std::size_t i = 1; i < ticks.size(); ++i) {
        EXPECT_LT(ticks[i - 1].position, ticks[i].position);
    }
}

TEST(FormatFrequency, FrequencyLabelsUseAudioConventions) {
    EXPECT_EQ(format_frequency(20.0f), "20");
    EXPECT_EQ(format_frequency(500.0f), "500");
    EXPECT_EQ(format_frequency(1000.0f), "1k");
    EXPECT_EQ(format_frequency(2000.0f), "2k");
    EXPECT_EQ(format_frequency(20000.0f), "20k");
    EXPECT_EQ(format_frequency(1500.0f), "1.5k");
    EXPECT_EQ(format_frequency(6.3f), "6.3");
}

TEST(LevelAxis, LevelTopIsZeroAndBottomIsTheHeight) {
    const LevelAxis axis = LevelAxis::full_scale(600.0f);
    EXPECT_LT(std::abs(axis.db_to_y(0.0f)), 1e-3f);
    EXPECT_NEAR(axis.db_to_y(-120.0f), 600.0f, 1e-3f);
    // Halfway in level is halfway down the pixels.
    EXPECT_NEAR(axis.db_to_y(-60.0f), 300.0f, 1e-3f);
}

TEST(LevelAxis, LevelRoundTripsThroughPixels) {
    const LevelAxis axis(-90.0f, 6.0f, 480.0f);
    for (const float db : {6.0f, 0.0f, -12.5f, -60.0f, -89.9f}) {
        const float back = axis.y_to_db(axis.db_to_y(db));
        EXPECT_NEAR(back, db, 1e-3f) << db << " came back as " << back;
    }
}

TEST(LevelAxis, LevelTicksAlignToMultiplesNotToTheRange) {
    // A range starting at -117 must still tick on multiples of 10.
    const LevelAxis axis(-117.0f, 3.0f, 600.0f);
    const auto ticks = axis.ticks(10.0f);
    for (const Tick& tick : ticks) {
        EXPECT_LT(std::abs(std::fmod(tick.value, 10.0f)), 1e-3f) << tick.value;
    }
    ASSERT_FALSE(ticks.empty());
    EXPECT_EQ(ticks.front().value, -110.0f);
    EXPECT_EQ(ticks.back().value, 0.0f);
}

TEST(LevelAxis, LevelTicksStayInsideTheRange) {
    const LevelAxis axis = LevelAxis::full_scale(600.0f);
    for (const Tick& tick : axis.ticks(10.0f)) {
        EXPECT_TRUE(tick.value >= -120.0f && tick.value <= 0.0f) << tick.value;
        EXPECT_TRUE(tick.position >= -1e-3f && tick.position <= 600.0f + 1e-3f) << tick.position;
    }
}

TEST(LevelAxis, AnAbsurdTickStepDoesNotExplode) {
    const LevelAxis axis(-200.0f, 0.0f, 600.0f);
    EXPECT_LE(axis.ticks(0.0001f).size(), 1025u);
}

TEST(LevelAxis, ResizingPreservesTheDataRange) {
    const FrequencyAxis axis = FrequencyAxis::audible(800.0f).with_width(1600.0f);
    EXPECT_EQ(axis.width(), 1600.0f);
    EXPECT_NEAR(axis.freq_to_x(20000.0f), 1600.0f, 1e-3f);

    const LevelAxis level = LevelAxis::full_scale(300.0f).with_height(900.0f);
    EXPECT_NEAR(level.db_to_y(-120.0f), 900.0f, 1e-3f);
}

TEST(FrequencyAxisDeathTest, AFrequencyAxisThroughZeroIsRejected) {
    EXPECT_DEATH(FrequencyAxis(0.0f, 20000.0f, 100.0f), "need 0 < min_hz < max_hz");
}

TEST(LevelAxisDeathTest, AnInvertedLevelAxisIsRejected) {
    EXPECT_DEATH(LevelAxis(0.0f, -120.0f, 100.0f), "need min_db < max_db");
}

}  // namespace
}  // namespace analyzer::plot
