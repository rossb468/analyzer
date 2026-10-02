// Ported from crates/analyzer-dsp/src/eq.rs.

#include "dsp/eq.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::dsp {
namespace {

constexpr float kRate = 48'000.0f;
constexpr float kTau = 2.0f * std::numbers::pi_v<float>;
constexpr float kFrac1Sqrt2 = std::numbers::sqrt2_v<float> / 2.0f;

float decibels(float linear) {
    return 20.0f * std::log10(std::max(linear, 1e-12f));
}

TEST(Equaliser, AFlatGraphicEqualiserChangesNothing) {
    const Equaliser eq = Equaliser::graphic(kRate);
    EXPECT_EQ(eq.bands().size(), 10u);
    for (const float hz : {20.0f, 100.0f, 1000.0f, 10'000.0f, 20'000.0f}) {
        EXPECT_LT(std::abs(eq.magnitude_db_at(hz)), 1e-4f)
            << "flat EQ was not flat at " << hz << " Hz";
    }
}

// A fader moved is a bump at that frequency and nothing elsewhere.
TEST(Equaliser, AGraphicBandLiftsItsOwnOctave) {
    Equaliser eq = Equaliser::graphic(kRate);
    // The 1 kHz fader, index 5.
    eq.set_gain_db(5, 6.0f);

    EXPECT_LT(std::abs(eq.magnitude_db_at(1000.0f) - 6.0f), 0.05f);
    // Two octaves away the neighbours should be essentially untouched.
    EXPECT_LT(std::abs(eq.magnitude_db_at(250.0f)), 0.6f);
    EXPECT_LT(std::abs(eq.magnitude_db_at(4000.0f)), 0.6f);
}

// Ganged faders overshoot, and the overshoot must stay bounded.
//
// Octave-spaced peaking filters overlap, so decibels add and every fader at
// +4 dB reads about +5.8 dB rather than +4. That is what a constant-Q
// graphic equaliser does; the test pins the magnitude of the effect so a
// change in kOctaveQ cannot quietly make it worse.
TEST(Equaliser, GangedFadersOvershootByABoundedAmount) {
    Equaliser eq = Equaliser::graphic(kRate);
    for (std::size_t index = 0; index < 10; ++index) {
        eq.set_gain_db(index, 4.0f);
    }
    for (const float hz : {125.0f, 500.0f, 1000.0f, 4000.0f}) {
        const float combined = eq.magnitude_db_at(hz);
        EXPECT_TRUE(combined > 4.0f && combined < 6.0f)
            << "all faders at 4 dB read " << combined << " dB at " << hz << " Hz";
    }
}

// kOctaveQ must be the cookbook's exact one-octave value, since that is
// what makes a band mean the same thing in another tool.
TEST(Equaliser, TheOctaveQIsExactlyOneOctaveWide) {
    const float bandwidth = 2.0f * std::asinh(1.0f / (2.0f * kOctaveQ)) / std::log(2.0f);
    EXPECT_LT(std::abs(bandwidth - 1.0f), 1e-4f)
        << "kOctaveQ describes " << bandwidth << " octaves, not 1";
}

// And it must be near the flattest choice, even if not exactly it.
//
// Q 1.5 is marginally flatter when every fader is ganged, but only by a
// fraction of a decibel. This pins that the standard value stays close to
// the best, so a future change to it cannot quietly cost a decibel.
TEST(Equaliser, TheOctaveQIsCloseToTheFlattestChoice) {
    const auto worst = [](float q) {
        std::vector<FilterBand> bands;
        for (const float hz : kOctaveCentres) {
            bands.push_back(FilterBand::peaking(hz, 4.0f, q));
        }
        const Equaliser eq(kRate, std::move(bands));
        // Over the whole curve, not only the band centres: a Q that is flat
        // at the centres can sag badly between them.
        constexpr float low = 45.0f;
        constexpr float high = 11'000.0f;
        float deviation = 0.0f;
        for (int i = 0; i < 400; ++i) {
            const float hz =
                low * std::exp(std::log(high / low) * (static_cast<float>(i) / 399.0f));
            deviation = std::max(deviation, std::abs(eq.magnitude_db_at(hz) - 4.0f));
        }
        return deviation;
    };
    const float chosen = worst(kOctaveQ);
    float best = std::numeric_limits<float>::infinity();
    for (const float q : {1.0f, 1.2f, 1.5f, 1.8f, 2.5f, 4.1f}) {
        best = std::min(best, worst(q));
    }
    EXPECT_LE(chosen, best + 0.5f)
        << "the chosen Q deviates " << chosen << " dB against a best of " << best;
}

// Cascaded sections multiply, so decibels add.
TEST(Equaliser, BandsAddInDecibels) {
    Equaliser eq = Equaliser::parametric(kRate);
    eq.push_band(FilterBand::peaking(1000.0f, 6.0f, 4.0f));
    eq.push_band(FilterBand::peaking(1000.0f, 6.0f, 4.0f));
    EXPECT_LT(std::abs(eq.magnitude_db_at(1000.0f) - 12.0f), 0.05f)
        << "two 6 dB bands should make 12, got " << eq.magnitude_db_at(1000.0f);
}

// A disabled band keeps its settings and stops contributing.
TEST(Equaliser, DisablingABandRemovesItFromTheCurveButNotTheList) {
    Equaliser eq = Equaliser::parametric(kRate);
    const std::size_t index = eq.push_band(FilterBand::peaking(1000.0f, 9.0f, 2.0f));
    EXPECT_LT(std::abs(eq.magnitude_db_at(1000.0f) - 9.0f), 0.05f);

    FilterBand disabled = eq.bands()[index];
    disabled.enabled = false;
    eq.set_band(index, disabled);
    EXPECT_LT(std::abs(eq.magnitude_db_at(1000.0f)), 1e-4f);
    EXPECT_EQ(eq.bands().size(), 1u) << "the band is still there";
    EXPECT_EQ(eq.bands()[index].gain_db, 9.0f) << "and keeps its gain";
}

TEST(Equaliser, RemovingABandRemovesItsSection) {
    Equaliser eq = Equaliser::parametric(kRate);
    eq.push_band(FilterBand::peaking(1000.0f, 6.0f, 2.0f));
    eq.push_band(FilterBand::peaking(100.0f, 6.0f, 2.0f));
    eq.remove_band(0);
    EXPECT_EQ(eq.bands().size(), 1u);
    EXPECT_LT(std::abs(eq.magnitude_db_at(1000.0f)), 0.5f);
    EXPECT_LT(std::abs(eq.magnitude_db_at(100.0f) - 6.0f), 0.05f);
    // Out of range must be ignored, not abort.
    eq.remove_band(99);
    EXPECT_EQ(eq.bands().size(), 1u);
}

// The headroom figure has to be the real worst case, since it is what a
// caller trims by.
TEST(Equaliser, PeakGainFindsTheWorstCase) {
    Equaliser eq = Equaliser::parametric(kRate);
    eq.push_band(FilterBand::peaking(1000.0f, 6.0f, 1.0f));
    eq.push_band(FilterBand::peaking(1200.0f, 6.0f, 1.0f));
    const float peak = eq.peak_gain_db();
    EXPECT_GT(peak, 10.0f) << "two overlapping 6 dB bands must exceed 10 dB somewhere, got "
                           << peak;
    EXPECT_LT(peak, 12.5f) << "and cannot exceed their sum, got " << peak;
}

TEST(Equaliser, TrimmingBringsThePeakToUnity) {
    Equaliser eq = Equaliser::parametric(kRate);
    eq.push_band(FilterBand::peaking(1000.0f, 12.0f, 1.0f));
    eq.trim_to_unity();
    const float peak = eq.peak_gain_db();
    EXPECT_LT(std::abs(peak), 0.1f) << "after trimming the peak was " << peak << " dB";
    EXPECT_LT(eq.preamp_db(), -11.0f);
}

// Cutting rather than boosting must not be trimmed at all.
TEST(Equaliser, TrimmingLeavesACutOnlyEqualiserAlone) {
    Equaliser eq = Equaliser::parametric(kRate);
    eq.push_band(FilterBand::peaking(1000.0f, -12.0f, 1.0f));
    eq.trim_to_unity();
    EXPECT_EQ(eq.preamp_db(), 0.0f);
}

// The running filter must match the predicted curve. This is the check
// that the drawn curve is the truth, which is the whole point of the
// prediction path being separate from the application path.
TEST(Equaliser, TheRunningEqualiserMatchesItsPredictedCurve) {
    Equaliser eq = Equaliser::parametric(kRate);
    eq.push_band(FilterBand::peaking(1000.0f, 8.0f, 2.0f));
    eq.push_band(FilterBand{
        .kind = FilterKind::HighShelf,
        .hz = 5000.0f,
        .gain_db = -6.0f,
        .q = 0.707f,
        .enabled = true,
    });
    eq.set_preamp_db(-3.0f);

    for (const float hz : {100.0f, 1000.0f, 3000.0f, 10'000.0f}) {
        const float predicted = eq.magnitude_db_at(hz);

        eq.reset();
        constexpr std::size_t frames = 48'000;
        std::vector<float> input(frames, 0.0f);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            input[frame] = std::sin(kTau * hz * static_cast<float>(frame) / kRate);
        }
        eq.process(input);

        // RMS over the settled tail; a peak reading would be biased by
        // wherever the samples happen to fall on the waveform.
        const std::size_t tail_start = frames / 2;
        double sum = 0.0;
        for (std::size_t i = tail_start; i < frames; ++i) {
            sum += static_cast<double>(input[i]) * static_cast<double>(input[i]);
        }
        const auto rms =
            static_cast<float>(std::sqrt(sum / static_cast<double>(frames - tail_start)));
        const float measured = decibels(rms / kFrac1Sqrt2);

        EXPECT_LT(std::abs(measured - predicted), 0.2f)
            << "at " << hz << " Hz predicted " << predicted << " dB but measured " << measured
            << " dB";
    }
}

// A preset written at one rate must survive being loaded at another.
TEST(Equaliser, ChangingTheSampleRateRedesignsTheBands) {
    Equaliser eq = Equaliser::parametric(kRate);
    eq.push_band(FilterBand::peaking(1000.0f, 6.0f, 2.0f));
    EXPECT_LT(std::abs(eq.magnitude_db_at(1000.0f) - 6.0f), 0.05f);

    eq.set_sample_rate(44'100.0f);
    EXPECT_LT(std::abs(eq.magnitude_db_at(1000.0f) - 6.0f), 0.05f)
        << "the band moved when the rate changed";
}

// A band above the new Nyquist must go quiet, not produce NaNs.
TEST(Equaliser, ABandAboveNyquistBecomesAPassThrough) {
    Equaliser eq = Equaliser::parametric(96'000.0f);
    eq.push_band(FilterBand::peaking(30'000.0f, 9.0f, 2.0f));
    eq.set_sample_rate(44'100.0f);
    for (const float hz : {100.0f, 1000.0f, 20'000.0f}) {
        const float db = eq.magnitude_db_at(hz);
        EXPECT_TRUE(std::isfinite(db)) << hz << " Hz produced " << db;
        EXPECT_LT(std::abs(db), 0.01f);
    }
}

// Flattening clears the gains and the trim but keeps the layout.
TEST(Equaliser, FlatteningClearsGainsButKeepsBands) {
    Equaliser eq = Equaliser::graphic(kRate);
    eq.set_gain_db(3, 8.0f);
    eq.set_preamp_db(-8.0f);
    eq.flatten();
    EXPECT_EQ(eq.bands().size(), 10u);
    EXPECT_EQ(eq.preamp_db(), 0.0f);
    EXPECT_LT(std::abs(eq.magnitude_db_at(250.0f)), 1e-4f);
    EXPECT_EQ(eq.bands()[3].hz, 250.0f) << "the band is still where it was";
}

// Gain is meaningless for the pass and reject shapes, and a UI needs to
// know that rather than offering a control that does nothing.
TEST(Equaliser, OnlyTheGainShapesUseGain) {
    EXPECT_TRUE(uses_gain(FilterKind::Peaking));
    EXPECT_TRUE(uses_gain(FilterKind::LowShelf));
    EXPECT_TRUE(uses_gain(FilterKind::HighShelf));
    for (const FilterKind kind : {FilterKind::LowPass, FilterKind::HighPass, FilterKind::BandPass,
                                  FilterKind::Notch, FilterKind::AllPass}) {
        EXPECT_FALSE(uses_gain(kind)) << to_string(kind) << " should not use gain";
    }
}

// A highpass must filter even though its gain is zero, which the
// transparency shortcut must not skip.
TEST(Equaliser, AZeroGainHighpassStillFilters) {
    Equaliser eq = Equaliser::parametric(kRate);
    eq.push_band(FilterBand{
        .kind = FilterKind::HighPass,
        .hz = 1000.0f,
        .gain_db = 0.0f,
        .q = 0.707f,
        .enabled = true,
    });
    EXPECT_LT(eq.magnitude_db_at(100.0f), -30.0f)
        << "a highpass with no gain still has to roll off";
}

// Moving a fader must not restart the filter's state, or every move would
// click.
TEST(Equaliser, ChangingAGainPreservesFilterState) {
    Equaliser eq = Equaliser::graphic(kRate);
    eq.set_gain_db(5, 6.0f);
    std::vector<float> ramp(512);
    for (std::size_t i = 0; i < ramp.size(); ++i) {
        ramp[i] = (static_cast<float>(i) / 512.0f) * 0.5f;
    }
    eq.process(ramp);

    // Change a different band; the running state must survive.
    eq.set_gain_db(2, 3.0f);
    std::vector<float> next(8, 0.0f);
    eq.process(next);
    EXPECT_TRUE(std::any_of(next.begin(), next.end(), [](float s) { return std::abs(s) > 1e-6f; }))
        << "the tail of the previous block was thrown away";
}

// Out-of-range indices are a UI race, not a bug worth crashing over.
TEST(Equaliser, OutOfRangeIndicesAreIgnored) {
    Equaliser eq = Equaliser::graphic(kRate);
    eq.set_gain_db(99, 12.0f);
    eq.set_band(99, FilterBand::peaking(100.0f, 12.0f, 1.0f));
    EXPECT_EQ(eq.band_magnitude_db_at(99, 1000.0f), 0.0f);
    for (const float hz : {100.0f, 1000.0f}) {
        EXPECT_LT(std::abs(eq.magnitude_db_at(hz)), 1e-4f);
    }
}

}  // namespace
}  // namespace analyzer::dsp
