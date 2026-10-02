// Ported from crates/analyzer-model/src/measurement.rs.

#include "model/measurement.hpp"

#include <cmath>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::model {
namespace {

MeasurementData spectrum() {
    return SpectrumData{
        {Complex64(1.0, 0.0), Complex64(0.5, 0.5), Complex64(0.0, -1.0)},
        10.0,
    };
}

TEST(MeasurementData, MagnitudeAndPhaseAreDerivedNotStored) {
    const MeasurementData data = spectrum();
    const auto magnitude = magnitude_db(data);
    const auto phase = phase_degrees(data);
    ASSERT_TRUE(magnitude.has_value());
    ASSERT_TRUE(phase.has_value());

    EXPECT_LT(std::abs((*magnitude)[0] - 0.0), 1e-9) << "unit magnitude is 0 dB";
    EXPECT_LT(std::abs((*phase)[0] - 0.0), 1e-9);
    // 0 - j means -90 degrees.
    EXPECT_LT(std::abs((*phase)[2] - -90.0), 1e-9) << "got " << (*phase)[2];
}

TEST(MeasurementData, AZeroBinFloorsRatherThanProducingInfinity) {
    const MeasurementData data = SpectrumData{{Complex64(0.0, 0.0)}, 1.0};
    const auto magnitude = magnitude_db(data);
    ASSERT_TRUE(magnitude.has_value());
    EXPECT_TRUE(std::isfinite((*magnitude)[0]));
    EXPECT_LE((*magnitude)[0], -200.0);
}

TEST(MeasurementData, TimeDomainDataHasNoSpectrumViews) {
    const MeasurementData data = ImpulseResponseData{std::vector<double>(16, 0.0), 4.5};
    EXPECT_FALSE(magnitude_db(data).has_value());
    EXPECT_FALSE(phase_degrees(data).has_value());
    EXPECT_FALSE(bin_spacing_hz(data).has_value());
}

TEST(MeasurementData, TimeZeroIsFractional) {
    // At 48 kHz one sample is 7 mm of path, so rounding this away loses
    // sub-sample alignment that the delay finder worked to establish.
    const MeasurementData data = ImpulseResponseData{std::vector<double>(8, 0.0), 3.25};
    const auto* ir = std::get_if<ImpulseResponseData>(&data);
    ASSERT_NE(ir, nullptr) << "wrong variant";
    EXPECT_LT(std::abs(ir->time_zero_samples - 3.25), 1e-12);
}

TEST(MeasurementData, KindsAreStableStrings) {
    EXPECT_EQ(kind(spectrum()), "spectrum");
    EXPECT_EQ(kind(ImpulseResponseData{{}, 0.0}), "impulse_response");
    EXPECT_EQ(kind(TransferFunctionData{{}, {}, 1.0}), "transfer_function");
}

TEST(Measurement, BinFrequencyFollowsTheSpacing) {
    const Measurement m(MeasurementId{1}, "test", 48'000.0, spectrum());
    EXPECT_EQ(m.bin_frequency(0), std::optional(0.0));
    EXPECT_EQ(m.bin_frequency(5), std::optional(50.0));
}

TEST(Measurement, DurationOnlyAppliesToTimeDomainData) {
    const Measurement ir(MeasurementId{1}, "ir", 48'000.0,
                         ImpulseResponseData{std::vector<double>(48'000, 0.0), 0.0});
    ASSERT_TRUE(ir.duration_seconds().has_value());
    EXPECT_LT(std::abs(*ir.duration_seconds() - 1.0), 1e-9);

    const Measurement sp(MeasurementId{2}, "sp", 48'000.0, spectrum());
    EXPECT_FALSE(sp.duration_seconds().has_value());
}

// None and zero are different. An unknown SPL offset must not read as a
// calibrated measurement that happens to need no correction.
TEST(Measurement, UnknownReferencesAreDistinctFromZero) {
    Measurement m(MeasurementId{1}, "test", 48'000.0, spectrum());
    EXPECT_FALSE(m.is_spl_calibrated());
    EXPECT_TRUE(m.references.is_empty());

    m.references.spl_offset_db = 0.0;
    EXPECT_TRUE(m.is_spl_calibrated()) << "an offset of zero is still an offset";
    EXPECT_FALSE(m.references.is_empty());
}

MeasurementData power_spectrum() {
    return PowerSpectrumData{{-40.0, -35.0, -60.0}, 10.0};
}

TEST(PowerSpectrumData, MagnitudePassesStraightThrough) {
    const auto magnitude = magnitude_db(power_spectrum());
    ASSERT_TRUE(magnitude.has_value());
    EXPECT_EQ(*magnitude, (std::vector<double>{-40.0, -35.0, -60.0}));
}

// The reason the type exists: an RTA never measured phase, so it must not
// report any. Zeros here would be indistinguishable from a genuine zero-phase
// measurement.
TEST(PowerSpectrumData, NoPhaseIsReported) {
    EXPECT_FALSE(phase_degrees(power_spectrum()).has_value());
}

TEST(PowerSpectrumData, ItIsStillAFrequencyDomainMeasurement) {
    EXPECT_EQ(bin_spacing_hz(power_spectrum()), std::optional(10.0));
    EXPECT_EQ(point_count(power_spectrum()), 3u);
    EXPECT_EQ(kind(power_spectrum()), "power_spectrum");
}

}  // namespace
}  // namespace analyzer::model
