// Ported from crates/analyzer-dsp/src/optimise.rs.

#include "dsp/optimise.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "dsp/biquad.hpp"
#include "dsp/target.hpp"

namespace analyzer::dsp {
namespace {

constexpr float kRate = 48'000.0f;

std::vector<float> log_sweep(float from, float to, std::size_t count) {
    std::vector<float> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count - 1);
        out[i] = from * std::pow(to / from, t);
    }
    return out;
}

// A measurement that is flat except for one resonance.
std::vector<float> with_peak(const std::vector<float>& frequencies, float hz, float gain_db,
                             float q) {
    const Biquad section = Biquad::peaking(hz, q, gain_db, kRate);
    std::vector<float> out;
    out.reserve(frequencies.size());
    for (const float f : frequencies) {
        out.push_back(20.0f * std::log10(section.magnitude_at(f, kRate)));
    }
    return out;
}

std::vector<float> sum(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<float> out(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        out[i] = a[i] + b[i];
    }
    return out;
}

TargetCurve flat_target() {
    return TargetCurve(TargetShape::flat());
}

// The bands, for a failure message.
std::string describe(std::span<const FilterBand> bands) {
    std::ostringstream out;
    for (const FilterBand& band : bands) {
        out << "{" << to_string(band.kind) << " " << band.hz << " Hz " << band.gain_db << " dB Q "
            << band.q << (band.enabled ? "" : " disabled") << "} ";
    }
    return out.str();
}

TEST(Optimise, ASingleResonanceIsCorrectedNearlyFlat) {
    const auto frequencies = log_sweep(20.0f, 500.0f, 400);
    const auto measured = with_peak(frequencies, 63.0f, 8.0f, 4.0f);

    const Optimisation result = optimise(frequencies, measured, flat_target());

    EXPECT_FALSE(result.bands.empty());
    EXPECT_LT(result.final_error_db, 0.5f)
        << "residual " << result.final_error_db << " dB from " << result.initial_error_db;
    EXPECT_GT(result.improvement_db(), 1.0f);
}

TEST(Optimise, TheFirstFilterLandsOnTheResonance) {
    const auto frequencies = log_sweep(20.0f, 500.0f, 400);
    const auto measured = with_peak(frequencies, 63.0f, 8.0f, 4.0f);

    const Optimisation result = optimise(frequencies, measured, flat_target());

    ASSERT_FALSE(result.bands.empty());
    const FilterBand& first = result.bands.front();
    EXPECT_LT(std::abs(std::log2(first.hz / 63.0f)), 0.25f) << "at " << first.hz << " Hz";
    EXPECT_LT(first.gain_db, 0.0f) << "a peak wants a cut";
}

TEST(Optimise, TwoResonancesGetTwoFilters) {
    const auto frequencies = log_sweep(20.0f, 500.0f, 600);
    const auto first = with_peak(frequencies, 45.0f, 7.0f, 6.0f);
    const auto second = with_peak(frequencies, 180.0f, -6.0f, 5.0f);
    const auto measured = sum(first, second);

    const Optimisation result = optimise(frequencies, measured, flat_target());

    EXPECT_GE(result.bands.size(), 2u) << describe(result.bands);
    EXPECT_GT(result.improvement_db(), 1.0f);
}

// The rule the module exists to enforce. A null is a cancellation, and
// boosting it burns headroom without filling it.
TEST(Optimise, ADeepNullIsNotBoostedPastTheCap) {
    const auto frequencies = log_sweep(20.0f, 500.0f, 400);
    const auto measured = with_peak(frequencies, 80.0f, -20.0f, 8.0f);

    OptimiserConfig config;
    config.max_boost_db = 3.0f;
    const Optimisation result = optimise(frequencies, measured, flat_target(), config);

    for (const FilterBand& band : result.bands) {
        EXPECT_LE(band.gain_db, 3.0f + 1e-4f) << "boosted " << band.gain_db << " dB into a null";
    }
}

TEST(Optimise, CutsAndBoostsRespectTheirSeparateCaps) {
    const auto frequencies = log_sweep(20.0f, 500.0f, 400);
    const auto peak = with_peak(frequencies, 60.0f, 18.0f, 5.0f);
    const auto dip = with_peak(frequencies, 200.0f, -18.0f, 5.0f);
    const auto measured = sum(peak, dip);

    OptimiserConfig config;
    config.max_boost_db = 2.0f;
    config.max_cut_db = 9.0f;
    const Optimisation result = optimise(frequencies, measured, flat_target(), config);

    for (const FilterBand& band : result.bands) {
        EXPECT_LE(band.gain_db, 2.0f + 1e-4f) << band.gain_db;
        EXPECT_GE(band.gain_db, -9.0f - 1e-4f) << band.gain_db;
    }
}

TEST(Optimise, FiltersStayInsideTheQLimits) {
    const auto frequencies = log_sweep(20.0f, 500.0f, 400);
    const auto measured = with_peak(frequencies, 63.0f, 10.0f, 20.0f);

    OptimiserConfig config;
    config.min_q = 1.0f;
    config.max_q = 6.0f;
    const Optimisation result = optimise(frequencies, measured, flat_target(), config);

    for (const FilterBand& band : result.bands) {
        EXPECT_TRUE(band.q >= 1.0f - 1e-4f && band.q <= 6.0f + 1e-4f) << "q " << band.q;
    }
}

TEST(Optimise, NothingOutsideTheBandIsCorrected) {
    const auto frequencies = log_sweep(20.0f, 20'000.0f, 800);
    const auto measured = with_peak(frequencies, 5000.0f, 10.0f, 4.0f);

    const Optimisation result = optimise(frequencies, measured, flat_target());
    EXPECT_TRUE(result.bands.empty()) << "corrected above the band: " << describe(result.bands);
}

// A flat measurement needs no filters, and producing some anyway would be
// the optimiser inventing work.
TEST(Optimise, AnAlreadyFlatMeasurementGetsNoFilters) {
    const auto frequencies = log_sweep(20.0f, 500.0f, 400);
    const std::vector<float> measured(frequencies.size(), 0.0f);
    const Optimisation result = optimise(frequencies, measured, flat_target());
    EXPECT_TRUE(result.bands.empty());
}

TEST(Optimise, TheFitNeverMakesTheErrorWorse) {
    const auto frequencies = log_sweep(20.0f, 500.0f, 400);
    struct Case {
        float hz;
        float gain;
        float q;
    };
    for (const Case c :
         {Case{40.0f, 9.0f, 3.0f}, Case{120.0f, -7.0f, 6.0f}, Case{300.0f, 4.0f, 1.5f}}) {
        const auto measured = with_peak(frequencies, c.hz, c.gain, c.q);
        const Optimisation result = optimise(frequencies, measured, flat_target());
        EXPECT_LE(result.final_error_db, result.initial_error_db + 1e-4f)
            << c.hz << " Hz: " << result.initial_error_db << " -> " << result.final_error_db;
    }
}

// Comparing two corrections is impossible if the same input can produce
// different filters.
TEST(Optimise, TheFitIsDeterministic) {
    const auto frequencies = log_sweep(20.0f, 500.0f, 400);
    const auto measured = with_peak(frequencies, 63.0f, 8.0f, 4.0f);
    const auto run = [&] { return optimise(frequencies, measured, flat_target()); };
    EXPECT_TRUE(run() == run());
}

TEST(Optimise, ItHonoursANonFlatTarget) {
    const auto frequencies = log_sweep(20.0f, 500.0f, 400);
    // The measurement already has the bass lift the target asks for, so
    // there is nothing to correct.
    const TargetCurve target(TargetShape::room());
    std::vector<float> measured;
    for (const float hz : frequencies) {
        measured.push_back(target.db_at(hz));
    }

    const Optimisation result = optimise(frequencies, measured, target);
    EXPECT_TRUE(result.bands.empty())
        << "corrected a measurement that already matched: " << describe(result.bands);
}

TEST(Optimise, AMeasurementMissingTheTargetsBassLiftGetsBoosted) {
    const auto frequencies = log_sweep(20.0f, 500.0f, 400);
    const TargetCurve target(TargetShape::room());
    const std::vector<float> measured(frequencies.size(), 0.0f);

    const Optimisation result = optimise(frequencies, measured, target);
    ASSERT_FALSE(result.bands.empty());
    bool boosted = false;
    for (const FilterBand& band : result.bands) {
        boosted = boosted || band.gain_db > 0.0f;
    }
    EXPECT_TRUE(boosted) << "expected a boost toward the target's bass lift";
}

TEST(Optimise, MaxFiltersIsRespected) {
    const auto frequencies = log_sweep(20.0f, 500.0f, 600);
    std::vector<float> measured(frequencies.size(), 0.0f);
    const float centres[] = {30.0f, 55.0f, 90.0f, 140.0f, 220.0f, 350.0f};
    for (std::size_t index = 0; index < std::size(centres); ++index) {
        const float gain = index % 2 == 0 ? 8.0f : -8.0f;
        const auto peak = with_peak(frequencies, centres[index], gain, 6.0f);
        for (std::size_t slot = 0; slot < peak.size(); ++slot) {
            measured[slot] += peak[slot];
        }
    }

    OptimiserConfig config;
    config.max_filters = 3;
    const Optimisation result = optimise(frequencies, measured, flat_target(), config);
    EXPECT_LE(result.bands.size(), 3u);
}

TEST(Optimise, EmptyAndDegenerateInputIsSurvivable) {
    const OptimiserConfig config;
    EXPECT_TRUE(optimise({}, {}, flat_target(), config) == Optimisation{});

    // Mismatched lengths truncate to the shorter side.
    const std::vector<float> two_frequencies = {100.0f, 200.0f};
    const std::vector<float> one_level = {0.0f};
    Optimisation result = optimise(two_frequencies, one_level, flat_target(), config);
    EXPECT_TRUE(result.bands.empty());

    // Everything filtered out by the band.
    const std::vector<float> below_band = {5.0f};
    const std::vector<float> loud = {10.0f};
    result = optimise(below_band, loud, flat_target(), config);
    EXPECT_TRUE(result == Optimisation{});
}

TEST(Optimise, ANonsenseConfigurationIsRepairedRatherThanObeyed) {
    const auto frequencies = log_sweep(20.0f, 500.0f, 200);
    const auto measured = with_peak(frequencies, 63.0f, 8.0f, 4.0f);
    OptimiserConfig config;
    config.from_hz = 0.0f;
    config.to_hz = -1.0f;
    config.min_q = -3.0f;
    config.max_q = std::numeric_limits<float>::quiet_NaN();
    config.threshold_db = std::numeric_limits<float>::quiet_NaN();
    config.sample_rate = 0.0f;
    const Optimisation result = optimise(frequencies, measured, flat_target(), config);
    for (const FilterBand& band : result.bands) {
        EXPECT_TRUE(std::isfinite(band.hz) && band.hz > 0.0f);
        EXPECT_TRUE(std::isfinite(band.q) && band.q > 0.0f);
        EXPECT_TRUE(std::isfinite(band.gain_db));
    }
}

TEST(Optimise, NonFiniteMeasurementPointsAreSkipped) {
    const auto frequencies = log_sweep(20.0f, 500.0f, 400);
    auto measured = with_peak(frequencies, 63.0f, 8.0f, 4.0f);
    measured[10] = std::numeric_limits<float>::quiet_NaN();
    measured[20] = -std::numeric_limits<float>::infinity();

    const Optimisation result = optimise(frequencies, measured, flat_target());
    EXPECT_TRUE(std::isfinite(result.final_error_db));
    for (const FilterBand& band : result.bands) {
        EXPECT_TRUE(std::isfinite(band.gain_db));
    }
}

}  // namespace
}  // namespace analyzer::dsp
