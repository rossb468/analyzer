// The decibel and angle conversions every module shares.

#include "base/units.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace analyzer {
namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// Power goes as the square of amplitude, so the same ratio is twice as many
// decibels as an amplitude.
TEST(Decibels, AnAmplitudeRatioOfTenIsTwentyDecibelsAndAPowerRatioOfTenIsTen) {
    EXPECT_FLOAT_EQ(amplitude_to_db(10.0f), 20.0f);
    EXPECT_FLOAT_EQ(power_to_db(10.0f), 10.0f);
    EXPECT_DOUBLE_EQ(amplitude_to_db(0.1), -20.0);
}

TEST(Decibels, ConvertingThereAndBackIsTheIdentity) {
    for (const float db : {-90.0f, -6.0f, 0.0f, 3.0f, 12.5f}) {
        EXPECT_NEAR(amplitude_to_db(db_to_amplitude(db)), db, 1e-4f);
        EXPECT_NEAR(power_to_db(db_to_power(db)), db, 1e-4f);
    }
    EXPECT_FLOAT_EQ(db_to_amplitude(0.0f), 1.0f);
    EXPECT_NEAR(db_to_amplitude(-6.0206f), 0.5f, 1e-4f);
    EXPECT_NEAR(db_to_power(-3.0103f), 0.5f, 1e-4f);
}

TEST(Decibels, AnUnflooredZeroIsMinusInfinity) {
    EXPECT_EQ(amplitude_to_db(0.0f), -std::numeric_limits<float>::infinity());
    EXPECT_TRUE(std::isnan(power_to_db(-1.0f)));
}

TEST(Decibels, AFloorCatchesSilenceNegativesAndNaN) {
    EXPECT_EQ(amplitude_to_db(0.0f, -200.0f), -200.0f);
    EXPECT_EQ(power_to_db(-1.0f, -200.0f), -200.0f);
    EXPECT_EQ(amplitude_to_db(kNaN, -200.0f), -200.0f);
    // Above the floor the floor changes nothing; below it, the floor wins.
    EXPECT_FLOAT_EQ(amplitude_to_db(10.0f, -200.0f), 20.0f);
    EXPECT_EQ(power_to_db(1e-30f, -200.0f), -200.0f);
}

TEST(Angles, DegreesAndRadiansAreReciprocal) {
    EXPECT_NEAR(kDegreesPerRadian<float> * kRadiansPerDegree<float>, 1.0f, 1e-6f);
    EXPECT_NEAR(std::numbers::pi_v<float> * kDegreesPerRadian<float>, 180.0f, 1e-4f);
    EXPECT_NEAR(kDegreesPerRadian<double> * std::numbers::pi, 180.0, 1e-12);
    EXPECT_FLOAT_EQ(kTau<float>, 2.0f * std::numbers::pi_v<float>);
}

}  // namespace
}  // namespace analyzer
