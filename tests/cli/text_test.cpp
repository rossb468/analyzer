#include "cli/text.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace analyzer::cli {
namespace {

// `{:?}` keeps the `.0` that `{}` drops. The harness prints a Tukey window's
// alpha this way in the header.
TEST(DebugFloat, KeepsTheDecimalPointOnWholeNumbers) {
    EXPECT_EQ(debug_float(0.25f), "0.25");
    EXPECT_EQ(debug_float(1.0f), "1.0");
    EXPECT_EQ(debug_float(48'000.0), "48000.0");
}

TEST(DebugFloat, LeavesTheValuesWithNoDigitsAlone) {
    EXPECT_EQ(debug_float(std::numeric_limits<double>::quiet_NaN()), "NaN");
    EXPECT_EQ(debug_float(std::numeric_limits<double>::infinity()), "inf");
    EXPECT_EQ(debug_float(-std::numeric_limits<double>::infinity()), "-inf");
}

}  // namespace
}  // namespace analyzer::cli
