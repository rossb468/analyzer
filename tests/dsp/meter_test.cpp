// Ported from crates/analyzer-dsp/src/meter.rs.

#include "dsp/meter.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

#include "support/signals.hpp"

namespace analyzer::dsp {
namespace {

constexpr float kRate = 48'000.0f;

// `seconds` of a sine at `hz`, at the test sample rate.
std::vector<float> sine(float hz, float amplitude, float seconds) {
    const auto count = static_cast<std::size_t>(kRate * seconds);
    return test::sine(count, hz, static_cast<std::size_t>(kRate), amplitude);
}

LevelMeter settled(MeterWeighting weighting, float hz, float amplitude) {
    LevelMeter meter(kRate, weighting, Integration::fast());
    // Two seconds is many Fast time constants, so the detector has settled.
    meter.push(sine(hz, amplitude, 2.0f));
    return meter;
}

// Meters and spectrum must agree about the same signal. A full-scale sine
// is 0 dBFS in both, which needs the +3.01 dB RMS correction.
TEST(LevelMeter, AFullScaleSineReadsZeroDbfs) {
    const LevelMeter meter = settled(MeterWeighting::Z, 1000.0f, 1.0f);
    EXPECT_LT(std::abs(meter.rms_db()), 0.1f) << "rms " << meter.rms_db();
    EXPECT_LT(std::abs(meter.peak_db()), 0.1f) << "peak " << meter.peak_db();
}

TEST(LevelMeter, HalvingAmplitudeDropsSixDecibels) {
    const float full = settled(MeterWeighting::Z, 1000.0f, 1.0f).rms_db();
    const float half = settled(MeterWeighting::Z, 1000.0f, 0.5f).rms_db();
    EXPECT_LT(std::abs(full - half - 6.0206f), 0.05f) << full << " -> " << half;
}

// The cross-check that justifies having two weighting implementations: the
// biquad cascade must agree with the per-bin curve in analyzer-cal, which
// is itself checked against IEC 61672.
TEST(LevelMeter, TheWeightingFiltersMatchThePublishedCurves) {
    struct Case {
        float hz;
        float expected;
        float tolerance;
    };

    // Values from IEC 61672-1 Table 3, the same table analyzer-cal uses.
    //
    // Tolerances follow the class 1 limits, which widen at the band edges,
    // rather than being picked to fit. A bilinear-transformed filter cannot
    // track the analog prototype right up to Nyquist - the 12194 Hz pole is
    // halfway there at 48 kHz - so 8 kHz gets the standard's ±1.5 rather
    // than a figure this implementation could not honestly meet.
    const Case a_cases[] = {
        {31.5f, -39.4f, 1.0f}, {63.0f, -26.2f, 0.7f}, {125.0f, -16.1f, 0.5f},
        {250.0f, -8.6f, 0.4f}, {500.0f, -3.2f, 0.3f}, {1000.0f, 0.0f, 0.1f},
        {2000.0f, 1.2f, 0.3f}, {4000.0f, 1.0f, 0.4f}, {8000.0f, -1.1f, 1.5f},
    };
    for (const auto& c : a_cases) {
        const float reference = settled(MeterWeighting::Z, c.hz, 0.5f).rms_db();
        const float weighted = settled(MeterWeighting::A, c.hz, 0.5f).rms_db();
        const float measured = weighted - reference;
        EXPECT_LT(std::abs(measured - c.expected), c.tolerance)
            << "A at " << c.hz << " Hz: filter gives " << measured << " dB, standard says "
            << c.expected;
    }

    const Case c_cases[] = {
        {31.5f, -3.0f, 0.5f},
        {125.0f, -0.2f, 0.3f},
        {1000.0f, 0.0f, 0.1f},
        {8000.0f, -3.0f, 1.5f},
    };
    for (const auto& c : c_cases) {
        const float reference = settled(MeterWeighting::Z, c.hz, 0.5f).rms_db();
        const float weighted = settled(MeterWeighting::C, c.hz, 0.5f).rms_db();
        const float measured = weighted - reference;
        EXPECT_LT(std::abs(measured - c.expected), c.tolerance)
            << "C at " << c.hz << " Hz: filter gives " << measured << " dB, standard says "
            << c.expected;
    }
}

TEST(LevelMeter, WeightingIsUnityAtOneKilohertz) {
    const float flat = settled(MeterWeighting::Z, 1000.0f, 0.5f).rms_db();
    for (const MeterWeighting weighting : {MeterWeighting::A, MeterWeighting::C}) {
        const float weighted = settled(weighting, 1000.0f, 0.5f).rms_db();
        EXPECT_LT(std::abs(weighted - flat), 0.1f)
            << to_string(weighting) << " at 1 kHz should be unity, differed by " << weighted - flat;
    }
}

TEST(LevelMeter, AWeightingCutsBassHarderThanC) {
    const float reference = settled(MeterWeighting::Z, 50.0f, 0.5f).rms_db();
    const float a = settled(MeterWeighting::A, 50.0f, 0.5f).rms_db() - reference;
    const float c = settled(MeterWeighting::C, 50.0f, 0.5f).rms_db() - reference;
    EXPECT_LT(a, c - 20.0f) << "A " << a << " should be far below C " << c << " at 50 Hz";
}

// Slow must lag Fast. Feeding a burst and stopping, the Slow meter should
// still be climbing when Fast has already arrived.
TEST(LevelMeter, SlowIntegrationLagsFast) {
    const auto burst = sine(1000.0f, 1.0f, 0.06f);
    LevelMeter fast(kRate, MeterWeighting::Z, Integration::fast());
    LevelMeter slow(kRate, MeterWeighting::Z, Integration::slow());
    fast.push(burst);
    slow.push(burst);

    EXPECT_GT(fast.rms_db(), slow.rms_db() + 5.0f)
        << "after 60 ms fast should be well ahead: " << fast.rms_db() << " vs " << slow.rms_db();
}

// Impulse rises fast and falls slowly, which is the whole point of it.
TEST(LevelMeter, ImpulseIntegrationRisesQuicklyAndFallsSlowly) {
    LevelMeter meter(kRate, MeterWeighting::Z, Integration::impulse());
    meter.push(sine(1000.0f, 1.0f, 0.15f));
    const float after_burst = meter.rms_db();
    EXPECT_GT(after_burst, -2.0f) << "should have risen quickly: " << after_burst;

    // Then 200 ms of silence. A 1.5 s decay leaves most of the level intact.
    meter.push(std::vector<float>(static_cast<std::size_t>(kRate * 0.2f), 0.0f));
    const float after_silence = meter.rms_db();
    EXPECT_GT(after_silence, after_burst - 6.0f)
        << "impulse should decay slowly: " << after_burst << " -> " << after_silence;
}

TEST(LevelMeter, PeakHoldsUntilReset) {
    LevelMeter meter(kRate, MeterWeighting::Z, Integration::fast());
    meter.push(sine(1000.0f, 1.0f, 0.1f));
    const float loud = meter.peak_db();
    EXPECT_LT(std::abs(loud), 0.1f);

    meter.push(sine(1000.0f, 0.01f, 0.5f));
    EXPECT_LT(std::abs(meter.peak_db() - loud), 0.01f) << "peak must hold";

    meter.reset_peak();
    meter.push(sine(1000.0f, 0.01f, 0.1f));
    EXPECT_LT(meter.peak_db(), -30.0f) << "peak should have been cleared";
}

// LEQ of a steady signal is that signal's level, by definition.
TEST(LevelMeter, LeqOfASteadyToneEqualsItsLevel) {
    LevelMeter meter(kRate, MeterWeighting::Z, Integration::fast());
    meter.push(sine(1000.0f, 0.5f, 2.0f));
    EXPECT_LT(std::abs(meter.leq_db() - -6.0206f), 0.05f) << "leq " << meter.leq_db();
}

// LEQ integrates energy, so half a period of signal is 3 dB down on the
// signal's own level.
TEST(LevelMeter, LeqAveragesEnergyOverSilence) {
    LevelMeter meter(kRate, MeterWeighting::Z, Integration::fast());
    meter.push(sine(1000.0f, 1.0f, 1.0f));
    meter.push(std::vector<float>(static_cast<std::size_t>(kRate), 0.0f));
    EXPECT_LT(std::abs(meter.leq_db() + 3.01f), 0.1f)
        << "half duty should be -3 dB, got " << meter.leq_db();
    EXPECT_LT(std::abs(meter.elapsed_seconds() - 2.0f), 0.01f);
}

TEST(LevelMeter, SilenceReadsAtTheFloorAndStaysFinite) {
    LevelMeter meter(kRate, MeterWeighting::Z, Integration::fast());
    meter.push(std::vector<float>(4096, 0.0f));
    EXPECT_LE(meter.rms_db(), kMeterFloorDb + 1e-3f);
    EXPECT_LE(meter.peak_db(), kMeterFloorDb + 1e-3f);
    EXPECT_TRUE(std::isfinite(meter.rms_db()) && std::isfinite(meter.peak_db()));
    EXPECT_TRUE(std::isfinite(meter.leq_db()));
}

TEST(LevelMeter, AFreshMeterReadsTheFloor) {
    const LevelMeter meter(kRate, MeterWeighting::Z, Integration::fast());
    EXPECT_LE(meter.leq_db(), kMeterFloorDb + 1e-3f);
    EXPECT_EQ(meter.elapsed_seconds(), 0.0f);
}

TEST(LevelMeter, ResetClearsTheFilterHistoryToo) {
    LevelMeter meter(kRate, MeterWeighting::A, Integration::fast());
    meter.push(sine(1000.0f, 1.0f, 1.0f));
    meter.reset();

    EXPECT_EQ(meter.elapsed_seconds(), 0.0f);
    EXPECT_LE(meter.rms_db(), kMeterFloorDb + 1e-3f);

    // If filter state survived, the first samples after reset would ring.
    LevelMeter fresh(kRate, MeterWeighting::A, Integration::fast());
    meter.push(sine(1000.0f, 0.5f, 1.0f));
    fresh.push(sine(1000.0f, 0.5f, 1.0f));
    EXPECT_LT(std::abs(meter.rms_db() - fresh.rms_db()), 0.01f);
}

TEST(LevelMeter, ChunkingDoesNotChangeTheReading) {
    const auto signal = sine(997.0f, 0.5f, 1.0f);
    LevelMeter whole(kRate, MeterWeighting::A, Integration::fast());
    whole.push(signal);

    LevelMeter chunked(kRate, MeterWeighting::A, Integration::fast());
    const std::span<const float> all(signal);
    for (std::size_t start = 0; start < all.size(); start += 137) {
        chunked.push(all.subspan(start, std::min<std::size_t>(137, all.size() - start)));
    }
    EXPECT_LT(std::abs(whole.rms_db() - chunked.rms_db()), 1e-3f);
    EXPECT_LT(std::abs(whole.leq_db() - chunked.leq_db()), 1e-3f);
}

TEST(LevelMeter, ACustomTimeConstantSitsBetweenFastAndSlow) {
    const auto burst = sine(1000.0f, 1.0f, 0.06f);
    LevelMeter fast(kRate, MeterWeighting::Z, Integration::fast());
    LevelMeter custom(kRate, MeterWeighting::Z, Integration::custom(0.4f));
    LevelMeter slow(kRate, MeterWeighting::Z, Integration::slow());
    fast.push(burst);
    custom.push(burst);
    slow.push(burst);

    EXPECT_GT(fast.rms_db(), custom.rms_db());
    EXPECT_GT(custom.rms_db(), slow.rms_db());
}

TEST(LevelMeterDeathTest, ANonPositiveSampleRateIsRejected) {
    EXPECT_DEATH(LevelMeter(0.0f, MeterWeighting::Z, Integration::fast()),
                 "sample rate must be positive");
}

}  // namespace
}  // namespace analyzer::dsp
