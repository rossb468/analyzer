// Ported from crates/analyzer-dsp/src/target.rs.

#include "dsp/target.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "support/signals.hpp"

namespace analyzer::dsp {
namespace {

TEST(TargetCurve, FlatIsFlatEverywhere) {
    const TargetCurve target(TargetShape::flat());
    for (const float hz : {20.0f, 100.0f, 1000.0f, 20'000.0f}) {
        EXPECT_EQ(target.db_at(hz), 0.0f);
    }
}

// A tilt is defined per octave, so a doubling must move it by exactly the
// stated amount.
TEST(TargetCurve, ATiltMovesByItsSlopePerOctave) {
    const TargetCurve target(TargetShape::tilt(-1.5f));
    EXPECT_LT(std::abs(target.db_at(kReferenceHz)), 1e-6f);
    EXPECT_LT(std::abs(target.db_at(2000.0f) + 1.5f), 1e-5f);
    EXPECT_LT(std::abs(target.db_at(500.0f) - 1.5f), 1e-5f);
    EXPECT_LT(std::abs(target.db_at(4000.0f) + 3.0f), 1e-5f);
}

TEST(TargetShape, ARoomShelfReachesHalfItsLiftAtTheTransition) {
    const auto shape = TargetShape::room(6.0f, 100.0f, 0.0f);
    EXPECT_LT(std::abs(shape.db_at(100.0f) - 3.0f), 1e-5f);
    // Well below, essentially the full shelf; well above, essentially none.
    EXPECT_GT(shape.db_at(10.0f), 5.9f);
    EXPECT_LT(shape.db_at(2000.0f), 0.1f);
}

TEST(TargetShape, TheDefaultRoomTargetLiftsTheBassAndFallsWithFrequency) {
    const auto shape = TargetShape::room();
    EXPECT_GT(shape.db_at(20.0f), shape.db_at(200.0f));
    EXPECT_GT(shape.db_at(1000.0f), shape.db_at(10'000.0f));
}

TEST(TargetShape, CustomPointsAreSortedAndCleaned) {
    const auto shape = TargetShape::custom({
        {1000.0f, -2.0f},
        {0.0f, 99.0f},
        {100.0f, 3.0f},
        {std::numeric_limits<float>::quiet_NaN(), 1.0f},
    });
    ASSERT_EQ(shape.kind, TargetShape::Kind::Custom);
    const std::vector<std::pair<float, float>> expected = {{100.0f, 3.0f}, {1000.0f, -2.0f}};
    EXPECT_EQ(shape.points, expected);
}

// Interpolating in linear frequency would put the midpoint at 550 Hz; in log
// frequency it is at about 316 Hz, which is where a sparse log-spaced file
// actually means it.
TEST(TargetShape, CustomInterpolationIsLogarithmic) {
    const auto shape = TargetShape::custom({{100.0f, 0.0f}, {1000.0f, 10.0f}});
    const float midpoint = shape.db_at(316.22777f);
    EXPECT_LT(std::abs(midpoint - 5.0f), 0.01f) << "got " << midpoint;
}

// A file that stops at 20 kHz says nothing about 22 kHz.
TEST(TargetShape, CustomCurvesHoldTheirEndsRatherThanExtrapolating) {
    const auto shape = TargetShape::custom({{100.0f, 3.0f}, {1000.0f, -2.0f}});
    EXPECT_EQ(shape.db_at(20.0f), 3.0f);
    EXPECT_EQ(shape.db_at(20'000.0f), -2.0f);
}

TEST(TargetShape, AnEmptyOrSinglePointCustomCurveIsUsable) {
    EXPECT_EQ(TargetShape::custom({}).db_at(1000.0f), 0.0f);
    EXPECT_EQ(TargetShape::custom({{500.0f, 4.0f}}).db_at(1000.0f), 4.0f);
}

// Alignment is what makes a relative target drawable against an absolute
// measurement.
TEST(TargetCurve, AlignmentCentresTheTargetOnTheMeasurement) {
    const auto frequencies = test::log_spaced(20.0f, 20'000.0f, 512);
    const std::vector<float> measured(frequencies.size(), -35.0f);

    const auto target = TargetCurve(TargetShape::flat())
                            .aligned_to(frequencies, measured, kAlignFromHz, kAlignToHz);
    EXPECT_LT(std::abs(target.offset_db() + 35.0f), 1e-4f);
    EXPECT_LT(std::abs(target.db_at(1000.0f) + 35.0f), 1e-4f);
}

// Only the alignment band counts, so a deep null outside it must not drag the
// whole curve down.
TEST(TargetCurve, AlignmentIgnoresEverythingOutsideTheBand) {
    const auto frequencies = test::log_spaced(20.0f, 20'000.0f, 512);
    std::vector<float> measured;
    for (const float hz : frequencies) {
        measured.push_back(hz < 100.0f ? -90.0f : -30.0f);
    }

    const auto target = TargetCurve(TargetShape::flat())
                            .aligned_to(frequencies, measured, kAlignFromHz, kAlignToHz);
    EXPECT_LT(std::abs(target.offset_db() + 30.0f), 0.5f) << target.offset_db();
}

TEST(TargetCurve, AlignmentOverAnEmptyBandLeavesTheOffsetAlone) {
    TargetCurve target(TargetShape::flat());
    target.set_offset_db(-12.0f);
    const std::array<float, 1> frequencies = {100.0f};
    const std::array<float, 1> measured = {-40.0f};
    target.align_to(frequencies, measured, 1000.0f, 2000.0f);
    EXPECT_EQ(target.offset_db(), -12.0f);
}

TEST(TargetCurve, AlignmentSkipsNonFiniteMeasurements) {
    const std::array<float, 3> frequencies = {500.0f, 1000.0f, 1500.0f};
    const std::array<float, 3> measured = {-std::numeric_limits<float>::infinity(), -20.0f,
                                           std::numeric_limits<float>::quiet_NaN()};
    const auto target = TargetCurve(TargetShape::flat())
                            .aligned_to(frequencies, measured, kAlignFromHz, kAlignToHz);
    EXPECT_LT(std::abs(target.offset_db() + 20.0f), 1e-4f);
}

// Positive error means too much energy, which is what wants a cut.
TEST(TargetCurve, ErrorIsMeasuredMinusTarget) {
    const std::array<float, 2> frequencies = {100.0f, 1000.0f};
    const std::array<float, 2> measured = {-20.0f, -30.0f};
    TargetCurve target(TargetShape::flat());
    target.set_offset_db(-25.0f);

    std::array<float, 2> error = {0.0f, 0.0f};
    target.write_error(frequencies, measured, error);
    EXPECT_LT(std::abs(error[0] - 5.0f), 1e-5f);
    EXPECT_LT(std::abs(error[1] + 5.0f), 1e-5f);
}

// Mismatched lengths must truncate rather than panic or overrun.
TEST(TargetCurve, WritingLevelsStopsAtTheShorterSide) {
    const TargetCurve target(TargetShape::flat());
    std::array<float, 4> out = {7.0f, 7.0f, 7.0f, 7.0f};
    const std::array<float, 2> frequencies = {100.0f, 200.0f};
    target.write_levels(frequencies, out);
    EXPECT_EQ(out[0], 0.0f);
    EXPECT_EQ(out[1], 0.0f);
    EXPECT_EQ(out[2], 7.0f) << "beyond the frequencies given, untouched";
}

TEST(TargetShape, ANonPositiveFrequencyEvaluatesToZeroRatherThanNan) {
    for (const auto& shape : {TargetShape::flat(), TargetShape::room(), TargetShape::tilt(-1.0f)}) {
        EXPECT_EQ(shape.db_at(0.0f), 0.0f);
        EXPECT_EQ(shape.db_at(-100.0f), 0.0f);
        EXPECT_EQ(shape.db_at(std::numeric_limits<float>::quiet_NaN()), 0.0f);
    }
}

TEST(TargetCurve, ANonFiniteOffsetIsRefused) {
    TargetCurve target(TargetShape::flat());
    target.set_offset_db(-10.0f);
    target.set_offset_db(std::numeric_limits<float>::quiet_NaN());
    EXPECT_EQ(target.offset_db(), -10.0f);
}

}  // namespace
}  // namespace analyzer::dsp
