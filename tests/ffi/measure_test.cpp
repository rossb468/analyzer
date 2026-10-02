// Ported from crates/analyzer-ffi/src/lib.rs.

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include "ffi/internal.hpp"

namespace analyzer::ffi {
namespace {

TEST(Measure, CallsTolerateNullHandles) {
    const AnalyzerMeasureConfig config = analyzer_measure_config_default();
    AnalyzerMeasureProgress progress{};
    AnalyzerMeasureResult result{};
    AnalyzerStatus status = status_ok();

    EXPECT_FALSE(analyzer_session_start_measurement(nullptr, &config, &status));
    EXPECT_NE(status.code, 0);
    EXPECT_FALSE(analyzer_session_measure_progress(nullptr, &progress));
    EXPECT_FALSE(analyzer_session_finish_measurement(nullptr, &result, &status));
    analyzer_session_cancel_measurement(nullptr);
    EXPECT_FALSE(analyzer_session_has_measurement(nullptr));
    EXPECT_FALSE(analyzer_session_measurement_result(nullptr, &result));
    EXPECT_EQ(analyzer_session_copy_measured(nullptr, nullptr, 0), 0u);
    EXPECT_EQ(analyzer_session_copy_impulse(nullptr, nullptr, 0, 0.05f), 0u);
}

// The sweep defaults must not be loud. A measurement stimulus that starts at
// full scale is one that damages something.
TEST(Measure, TheDefaultSweepIsWellBelowFullScale) {
    const AnalyzerMeasureConfig config = analyzer_measure_config_default();
    EXPECT_LE(config.level_db, -6.0f) << config.level_db;
    EXPECT_GT(config.end_hz, config.start_hz);
    EXPECT_GT(config.seconds, 0.0f);
    EXPECT_GT(config.tail_seconds, 0.0f) << "no tail leaves no decay to measure";
}

// A degenerate window is refused before the session is even looked at.
TEST(Measure, ADegenerateImpulseWindowIsRefused) {
    float scratch[8] = {};
    for (const float seconds : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN()}) {
        EXPECT_EQ(analyzer_session_copy_impulse(nullptr, scratch, 8, seconds), 0u);
    }
}

}  // namespace
}  // namespace analyzer::ffi
