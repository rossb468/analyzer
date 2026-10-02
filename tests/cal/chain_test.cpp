// Ported from crates/analyzer-cal/src/chain.rs.

#include "cal/chain.hpp"

#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::cal {
namespace {

// The golden test the plan calls for: inject a known reference tone and
// check the reported level. If this drifts, every displayed number is wrong
// by a constant and nothing else will notice.
TEST(Calibration, ANinetyFourDecibelReferenceToneReadsBackAsNinetyFour) {
    // A calibrator read -40 dBFS through this rig.
    const Calibration cal = Calibration::from_reference_tone(-40.0f, kCalibratorSplDb);
    EXPECT_NEAR(cal.offset_db(), 134.0f, 1e-4f);
    EXPECT_NEAR(cal.to_spl(-40.0f, 1000.0f), 94.0f, 1e-4f);
}

// The same rig computed from datasheet numbers instead of a calibrator must
// land in the same place.
TEST(Calibration, ThePhysicalChainAgreesWithAReferenceTone) {
    // 12.6 mV/Pa capsule, 20 dB of preamp gain, 1.0 V RMS full scale.
    const auto cal = Calibration::from_signal_chain(12.6f, 20.0f, 1.0f);
    ASSERT_TRUE(cal.has_value());

    // 1 Pa gives 0.0126 V * 10 = 0.126 V, which is -18.0 dBFS against 1 V.
    const float expected_dbfs = 20.0f * std::log10(0.126f);
    const Calibration equivalent =
        Calibration::from_reference_tone(expected_dbfs, kCalibratorSplDb);
    EXPECT_NEAR(cal->offset_db(), equivalent.offset_db(), 1e-3f)
        << "chain " << cal->offset_db() << " vs tone " << equivalent.offset_db();
    // And a 94 dB source still reads 94.
    EXPECT_NEAR(cal->to_spl(expected_dbfs, 1000.0f), 94.0f, 1e-3f);
}

TEST(Calibration, MorePreampGainMeansASmallerOffset) {
    const auto low = Calibration::from_signal_chain(12.6f, 0.0f, 1.0f);
    const auto high = Calibration::from_signal_chain(12.6f, 20.0f, 1.0f);
    ASSERT_TRUE(low && high);
    // 20 dB more gain means the same SPL reads 20 dB hotter, so the offset
    // needed to reach SPL drops by exactly 20.
    EXPECT_NEAR(low->offset_db() - high->offset_db(), 20.0f, 1e-3f);
}

TEST(Calibration, AMoreSensitiveCapsuleMeansASmallerOffset) {
    const auto quiet = Calibration::from_signal_chain(1.0f, 0.0f, 1.0f);
    const auto loud = Calibration::from_signal_chain(10.0f, 0.0f, 1.0f);
    ASSERT_TRUE(quiet && loud);
    EXPECT_NEAR(quiet->offset_db() - loud->offset_db(), 20.0f, 1e-3f);
}

TEST(Calibration, DegenerateChainValuesAreRejected) {
    EXPECT_FALSE(Calibration::from_signal_chain(0.0f, 0.0f, 1.0f).has_value());
    EXPECT_FALSE(Calibration::from_signal_chain(-1.0f, 0.0f, 1.0f).has_value());
    EXPECT_FALSE(Calibration::from_signal_chain(12.6f, 0.0f, 0.0f).has_value());
    EXPECT_FALSE(Calibration::from_signal_chain(std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f)
                     .has_value());
}

// A rig that genuinely needs no correction has still been calibrated. The Rust
// original decided this by comparing the offset with zero, which made the two
// indistinguishable; the rule that they are different facts is in CLAUDE.md.
TEST(Calibration, AMeasuredOffsetOfZeroStillCountsAsCalibrated) {
    const Calibration cal = Calibration::from_reference_tone(94.0f, 94.0f);
    EXPECT_TRUE(cal.is_calibrated());
    EXPECT_EQ(cal.offset_db(), 0.0f);
}

TEST(Calibration, TheDefaultIsUncalibratedAndSaysSo) {
    const Calibration cal;
    EXPECT_FALSE(cal.is_calibrated());
    // Uncalibrated must pass dBFS straight through, not invent an SPL.
    EXPECT_NEAR(cal.to_spl(-40.0f, 1000.0f), -40.0f, 1e-6f);
}

TEST(Calibration, TheMicrophoneCurveIsAddedOnTop) {
    const ResponseCurve curve({{100.0f, -3.0f}, {1000.0f, 0.0f}, {10000.0f, 2.0f}});
    const Calibration cal = Calibration::from_reference_tone(-40.0f, 94.0f).with_microphone(curve);

    EXPECT_NEAR(cal.to_spl(-40.0f, 1000.0f), 94.0f, 1e-3f);
    EXPECT_NEAR(cal.to_spl(-40.0f, 100.0f), 91.0f, 1e-3f);
    EXPECT_NEAR(cal.to_spl(-40.0f, 10000.0f), 96.0f, 1e-3f);
}

TEST(Calibration, WeightingIsAddedOnTopOfEverythingElse) {
    const Calibration cal =
        Calibration::from_reference_tone(-40.0f, 94.0f).with_weighting(Weighting::A);
    // A is 0 dB at 1 kHz by definition.
    EXPECT_NEAR(cal.to_spl(-40.0f, 1000.0f), 94.0f, 0.05f);
    // And about -19.1 dB at 100 Hz.
    EXPECT_NEAR(cal.to_spl(-40.0f, 100.0f), 94.0f - 19.1f, 0.2f);
}

// All three stages compose, and the order does not silently drop one.
TEST(Calibration, OffsetCurveAndWeightingAllApplyTogether) {
    const ResponseCurve curve({{100.0f, -3.0f}});
    const Calibration cal = Calibration::from_reference_tone(-40.0f, 94.0f)
                                .with_microphone(curve)
                                .with_weighting(Weighting::A);

    // 134 offset, -3 capsule, -19.1 A-weighting.
    const float expected = -40.0f + 134.0f - 3.0f - 19.1f;
    EXPECT_NEAR(cal.to_spl(-40.0f, 100.0f), expected, 0.2f)
        << "got " << cal.to_spl(-40.0f, 100.0f) << ", expected about " << expected;
}

TEST(Calibration, ApplyTransformsAWholeSpectrumAndLeavesDcAlone) {
    const Calibration cal = Calibration::from_reference_tone(-40.0f, 94.0f);
    std::vector<float> bins(5, -40.0f);
    cal.apply(bins, 1000.0f);

    EXPECT_NEAR(bins[0], -40.0f, 1e-6f) << "DC must not be shifted";
    for (std::size_t i = 1; i < bins.size(); ++i) {
        EXPECT_NEAR(bins[i], 94.0f, 1e-3f) << "bin " << i;
    }
}

TEST(Calibration, ApplyWithABadSpacingDoesNothing) {
    const Calibration cal = Calibration::from_reference_tone(-40.0f, 94.0f);
    std::vector<float> bins(4, -40.0f);
    cal.apply(bins, 0.0f);
    for (const float level : bins) {
        EXPECT_NEAR(level, -40.0f, 1e-6f);
    }
}

}  // namespace
}  // namespace analyzer::cal
