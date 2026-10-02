// Ported from crates/analyzer-cal/src/curve.rs.

#include "cal/curve.hpp"

#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::cal {
namespace {

ResponseCurve curve() {
    return ResponseCurve({
        {20.0f, -2.0f},
        {100.0f, -0.5f},
        {1000.0f, 0.0f},
        {10000.0f, 1.5f},
        {20000.0f, 3.0f},
    });
}

TEST(ResponseCurve, AFlatCurveCorrectsNothing) {
    const ResponseCurve flat = ResponseCurve::flat();
    EXPECT_TRUE(flat.is_flat());
    for (const float hz : {20.0f, 1000.0f, 20000.0f}) {
        EXPECT_EQ(flat.db_at(hz), 0.0f);
    }
}

TEST(ResponseCurve, ExactPointsReturnTheirOwnValue) {
    const ResponseCurve c = curve();
    EXPECT_NEAR(c.db_at(20.0f), -2.0f, 1e-4f);
    EXPECT_NEAR(c.db_at(1000.0f), 0.0f, 1e-4f);
    EXPECT_NEAR(c.db_at(20000.0f), 3.0f, 1e-4f);
}

// Interpolation is logarithmic in frequency. The geometric midpoint between
// 100 Hz and 10 kHz is 1 kHz, so a value there must land halfway between
// the two endpoints' levels - linear-in-frequency interpolation would put
// the halfway point at 5050 Hz and be badly wrong down low.
TEST(ResponseCurve, InterpolationIsLogarithmicInFrequency) {
    const ResponseCurve c({{100.0f, 0.0f}, {10000.0f, 10.0f}});
    EXPECT_NEAR(c.db_at(1000.0f), 5.0f, 1e-3f) << "got " << c.db_at(1000.0f);
    // A linear interpolator would read about 0.9 dB here.
    EXPECT_GT(c.db_at(1000.0f), 4.0f);
}

TEST(ResponseCurve, OutsideTheRangeTheEndpointsAreHeld) {
    const ResponseCurve c = curve();
    EXPECT_NEAR(c.db_at(1.0f), -2.0f, 1e-4f) << "below the first point";
    EXPECT_NEAR(c.db_at(96000.0f), 3.0f, 1e-4f) << "above the last point";
}

TEST(ResponseCurve, PointsAreSortedAndDeduplicated) {
    const ResponseCurve c({
        {1000.0f, 1.0f},
        {20.0f, -1.0f},
        {1000.0f, 9.0f},
        {100.0f, 0.0f},
    });
    const auto& points = c.points();
    ASSERT_EQ(points.size(), 3u);
    for (std::size_t i = 1; i < points.size(); ++i) {
        EXPECT_LT(points[i - 1].hz, points[i].hz) << "not sorted";
    }
}

TEST(ResponseCurve, NonPositiveAndNonFinitePointsAreDropped) {
    const ResponseCurve c({
        {0.0f, 5.0f},
        {-100.0f, 5.0f},
        {std::numeric_limits<float>::quiet_NaN(), 1.0f},
        {100.0f, std::numeric_limits<float>::infinity()},
        {1000.0f, 0.5f},
    });
    ASSERT_EQ(c.points().size(), 1u);
    EXPECT_NEAR(c.db_at(1000.0f), 0.5f, 1e-4f);
}

TEST(ResponseCurve, ParsesATypicalVendorFile) {
    const char* text =
        "* Microphone calibration\n"
        "* Serial 12345\n"
        " \n"
        "20.0    -2.00   0.0\n"
        "100.0   -0.50   0.0\n"
        "1000.0   0.00   0.0\n"
        "10000.0  1.50   0.0\n";
    const ResponseCurve c = ResponseCurve::parse(text);
    EXPECT_EQ(c.points().size(), 4u);
    EXPECT_NEAR(c.db_at(1000.0f), 0.0f, 1e-4f);
    EXPECT_NEAR(c.db_at(20.0f), -2.0f, 1e-4f);
}

TEST(ResponseCurve, ParsesCommaSeparatedAndHashComments) {
    const ResponseCurve c = ResponseCurve::parse("# header\n20,-1.5\n1000,0\n20000,2.5\n");
    EXPECT_EQ(c.points().size(), 3u);
    EXPECT_NEAR(c.db_at(20000.0f), 2.5f, 1e-4f);
}

// Vendor files carry stray junk. Skipping bad lines beats rejecting a file
// that is 99% usable.
TEST(ResponseCurve, UnparseableLinesAreSkippedNotFatal) {
    const ResponseCurve c = ResponseCurve::parse(
        "Sens Factor =-.4dB, SERNO: 1234\n"
        "not numbers at all\n"
        "20.0 -1.0\n"
        "1000.0 0.0\n"
        "trailing junk\n");
    EXPECT_EQ(c.points().size(), 2u);
}

TEST(ResponseCurve, AnEmptyFileYieldsAFlatCurve) {
    EXPECT_TRUE(ResponseCurve::parse("").is_flat());
    EXPECT_TRUE(ResponseCurve::parse("* only comments\n# nothing else\n").is_flat());
}

TEST(ResponseCurve, DisplaySummarisesTheRange) {
    EXPECT_EQ(to_string(ResponseCurve::flat()), "flat");
    EXPECT_EQ(to_string(curve()), "5 points, 20-20000 Hz");
}

TEST(ResponseCurve, ASinglePointCurveIsAConstantOffset) {
    const ResponseCurve c({{1000.0f, 2.5f}});
    for (const float hz : {10.0f, 1000.0f, 20000.0f}) {
        EXPECT_NEAR(c.db_at(hz), 2.5f, 1e-4f);
    }
}

}  // namespace
}  // namespace analyzer::cal
