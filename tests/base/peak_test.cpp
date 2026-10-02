// Which bin is "the" peak when several tie.

#include "base/peak.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

namespace analyzer {
namespace {

TEST(LastMaxIndex, FindsTheLoudestValue) {
    const std::vector<float> values{-90.0f, -40.0f, -10.0f, -60.0f};
    EXPECT_EQ(last_max_index(values), 2u);
}

// A silent spectrum is all one value. The reports and the live meter name the
// top bin then, not bin 0, and std::max_element would say the opposite.
TEST(LastMaxIndex, TheLastOfSeveralEqualMaximaWins) {
    EXPECT_EQ(last_max_index(std::vector<float>(8, -200.0f)), 7u);
    EXPECT_EQ(last_max_index(std::vector<float>{1.0f, 5.0f, 5.0f, 2.0f}), 2u);
}

// The total order puts a NaN above everything and -0.0 below +0.0.
TEST(LastMaxIndex, UsesTheIeeeTotalOrder) {
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(last_max_index(std::vector<float>{0.0f, kNaN, 3.0f}), 1u);
    EXPECT_EQ(last_max_index(std::vector<float>{0.0f, -0.0f}), 0u);
}

// Ranking by magnitude: a large negative sample beats a smaller positive one,
// and of two equal magnitudes the later wins.
TEST(LastMaxIndex, ARankingProjectionChangesWhatIsLoudest) {
    const std::vector<float> samples{0.5f, -0.9f, 0.2f, 0.9f};
    const auto magnitude = [](float s) { return std::fabs(s); };
    EXPECT_EQ(last_max_index(samples, magnitude), 3u);
    EXPECT_EQ(last_max_index(std::vector<float>{0.5f, -0.9f, 0.2f}, magnitude), 1u);
}

TEST(LastMaxIndex, ASingleValueIsItsOwnPeak) {
    EXPECT_EQ(last_max_index(std::vector<float>{-3.0f}), 0u);
}

TEST(LastMaxIndexDeathTest, NothingHasNoPeak) {
    EXPECT_DEATH(last_max_index(std::span<const float>{}), "cannot take the peak of nothing");
}

}  // namespace
}  // namespace analyzer
