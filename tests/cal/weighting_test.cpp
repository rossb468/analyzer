// Ported from crates/analyzer-cal/src/weighting.rs.

#include "cal/weighting.hpp"

#include <cmath>

#include <gtest/gtest.h>

namespace analyzer::cal {
namespace {

struct Case {
    float hz;
    float expected;
    float tolerance;
};

// Reference values from IEC 61672-1 Table 3. Tolerances match the class 1
// limits at each frequency, which is the only meaningful standard to hold
// an implementation of this to.
TEST(Weighting, AWeightingMatchesTheStandard) {
    const Case cases[] = {
        {10.0f, -70.4f, 0.6f},   {20.0f, -50.5f, 0.4f},   {50.0f, -30.2f, 0.3f},
        {100.0f, -19.1f, 0.2f},  {200.0f, -10.9f, 0.2f},  {500.0f, -3.2f, 0.2f},
        {1000.0f, 0.0f, 0.05f},  {2000.0f, 1.2f, 0.2f},   {5000.0f, 0.5f, 0.2f},
        {10000.0f, -2.5f, 0.3f}, {20000.0f, -9.3f, 0.5f},
    };
    for (const auto& c : cases) {
        const float got = db_at(Weighting::A, c.hz);
        EXPECT_LT(std::abs(got - c.expected), c.tolerance)
            << "A at " << c.hz << " Hz: got " << got << ", standard says " << c.expected;
    }
}

TEST(Weighting, CWeightingMatchesTheStandard) {
    const Case cases[] = {
        {10.0f, -14.3f, 0.3f},   {20.0f, -6.2f, 0.2f},     {50.0f, -1.3f, 0.1f},
        {100.0f, -0.3f, 0.1f},   {1000.0f, 0.0f, 0.05f},   {5000.0f, -1.3f, 0.1f},
        {10000.0f, -4.4f, 0.2f}, {20000.0f, -11.2f, 0.4f},
    };
    for (const auto& c : cases) {
        const float got = db_at(Weighting::C, c.hz);
        EXPECT_LT(std::abs(got - c.expected), c.tolerance)
            << "C at " << c.hz << " Hz: got " << got << ", standard says " << c.expected;
    }
}

// The defining property of both curves.
TEST(Weighting, BothCurvesAreExactlyZeroAtOneKilohertz) {
    EXPECT_LT(std::abs(db_at(Weighting::A, 1000.0f)), 0.05f);
    EXPECT_LT(std::abs(db_at(Weighting::C, 1000.0f)), 0.05f);
}

TEST(Weighting, ZWeightingIsFlatEverywhere) {
    for (const float hz : {1.0f, 20.0f, 1000.0f, 20000.0f, 96000.0f}) {
        EXPECT_EQ(db_at(Weighting::Z, hz), 0.0f);
    }
}

// A must attenuate the bass far harder than C. Getting the two swapped is
// an easy mistake and this catches it immediately.
TEST(Weighting, AAttenuatesBassMuchHarderThanC) {
    for (const float hz : {10.0f, 20.0f, 50.0f, 100.0f}) {
        const float a = db_at(Weighting::A, hz);
        const float c = db_at(Weighting::C, hz);
        EXPECT_LT(a, c - 5.0f) << "at " << hz << " Hz: A " << a << " should be well below C " << c;
    }
}

TEST(Weighting, DcAndNegativeFrequenciesReturnAFloorNotANan) {
    for (const Weighting weighting : {Weighting::A, Weighting::C, Weighting::Z}) {
        EXPECT_TRUE(std::isfinite(db_at(weighting, 0.0f)));
        EXPECT_TRUE(std::isfinite(db_at(weighting, -100.0f)));
    }
    EXPECT_LT(db_at(Weighting::A, 0.0f), -100.0f);
}

TEST(Weighting, CurvesStayFiniteAcrossTheWholeBand) {
    for (int step = 1; step < 200000; ++step) {
        const float hz = static_cast<float>(step) * 0.5f;
        ASSERT_TRUE(std::isfinite(db_at(Weighting::A, hz))) << "A broke at " << hz;
        ASSERT_TRUE(std::isfinite(db_at(Weighting::C, hz))) << "C broke at " << hz;
    }
}

TEST(Weighting, LabelsAreStable) {
    EXPECT_EQ(label(Weighting::A), "A");
    EXPECT_EQ(label(Weighting::C), "C");
    EXPECT_EQ(label(Weighting::Z), "Z");
}

}  // namespace
}  // namespace analyzer::cal
