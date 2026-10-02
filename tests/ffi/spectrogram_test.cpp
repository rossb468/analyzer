// Ported from crates/analyzer-ffi/src/lib.rs.

#include <limits>

#include <gtest/gtest.h>

#include "ffi/internal.hpp"

namespace analyzer::ffi {
namespace {

TEST(Spectrogram, CallsTolerateNullHandles) {
    float scratch[8] = {};
    EXPECT_EQ(analyzer_session_copy_spectrogram_column(nullptr, scratch, 8), 0u);
    EXPECT_EQ(analyzer_session_copy_spectrogram_column(nullptr, nullptr, 0), 0u);
    AnalyzerTick ticks[8] = {};
    EXPECT_EQ(analyzer_frequency_ticks_for(nullptr, 100.0f, ticks, 8), 0u);
}

// A zero or non-finite axis length would divide by zero inside the axis;
// refusing it here is cheaper than checking at every use.
TEST(Spectrogram, ADegenerateAxisLengthIsRefused) {
    AnalyzerTick ticks[8] = {};
    for (const float length : {0.0f, -10.0f, std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity()}) {
        EXPECT_EQ(analyzer_frequency_ticks_for(nullptr, length, ticks, 8), 0u);
    }
}

}  // namespace
}  // namespace analyzer::ffi
