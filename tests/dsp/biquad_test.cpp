// Ported from crates/analyzer-dsp/src/biquad.rs.

#include "dsp/biquad.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>

#include <gtest/gtest.h>

#include "support/signals.hpp"

namespace analyzer::dsp {
namespace {

using analyzer::test::kTau;

constexpr float kRate = 48'000.0f;
constexpr float kFrac1Sqrt2 = std::numbers::sqrt2_v<float> / 2.0f;

float db(float linear) {
    return 20.0f * std::log10(std::max(linear, 1e-12f));
}

// The defining property: a peaking filter hits its gain at the centre and
// leaves the ends alone.
TEST(Biquad, APeakingFilterHitsItsGainAtTheCentre) {
    for (const float gain : {-12.0f, -6.0f, 3.0f, 9.0f}) {
        const Biquad filter = Biquad::peaking(1000.0f, 2.0f, gain, kRate);
        const float centre = db(filter.magnitude_at(1000.0f, kRate));
        EXPECT_LT(std::abs(centre - gain), 0.01f)
            << "asked for " << gain << " dB, measured " << centre;
        EXPECT_LT(std::abs(db(filter.magnitude_at(20.0f, kRate))), 0.2f);
        EXPECT_LT(std::abs(db(filter.magnitude_at(18'000.0f, kRate))), 0.2f);
    }
}

// Q sets the width, and a higher Q must be narrower.
TEST(Biquad, AHigherQIsNarrower) {
    const Biquad wide = Biquad::peaking(1000.0f, 0.7f, 12.0f, kRate);
    const Biquad narrow = Biquad::peaking(1000.0f, 8.0f, 12.0f, kRate);
    // An octave away from the centre.
    const float at_wide = db(wide.magnitude_at(2000.0f, kRate));
    const float at_narrow = db(narrow.magnitude_at(2000.0f, kRate));
    EXPECT_GT(at_wide, at_narrow + 3.0f)
        << "wide " << at_wide << " should still be lifted where narrow " << at_narrow
        << " has decayed";
}

// The half-gain point defines Q for a peaking filter: at the edges of the
// bandwidth the response should be half the peak gain in decibels.
TEST(Biquad, TheBandwidthEdgesSitAtHalfGain) {
    constexpr float gain = 12.0f;
    constexpr float q = 1.414f;
    const Biquad filter = Biquad::peaking(1000.0f, q, gain, kRate);
    // Bandwidth in octaves for this Q, from the cookbook's own relation
    // 1/Q = 2*sinh(ln(2)/2 * BW). Q of 1.414 is one octave, which is what
    // makes it the conventional choice for a graphic EQ.
    const float bw = 2.0f * std::asinh(1.0f / (2.0f * q)) / std::log(2.0f);
    EXPECT_LT(std::abs(bw - 1.0f), 0.01f) << "Q " << q << " should be one octave, got " << bw;
    const float upper = 1000.0f * std::pow(2.0f, bw / 2.0f);
    const float measured = db(filter.magnitude_at(upper, kRate));
    EXPECT_LT(std::abs(measured - gain / 2.0f), 0.5f)
        << "at the band edge expected " << gain / 2.0f << " dB, measured " << measured;
}

// Shelves must reach their gain in the shelf and unity in the passband.
TEST(Biquad, ShelvesReachTheirGain) {
    const Biquad low = Biquad::low_shelf(200.0f, 0.707f, 6.0f, kRate);
    EXPECT_LT(std::abs(db(low.magnitude_at(20.0f, kRate)) - 6.0f), 0.3f);
    EXPECT_LT(std::abs(db(low.magnitude_at(10'000.0f, kRate))), 0.3f);
    // The corner of a shelf is the half-gain point, by definition.
    EXPECT_LT(std::abs(db(low.magnitude_at(200.0f, kRate)) - 3.0f), 0.3f);

    const Biquad high = Biquad::high_shelf(4000.0f, 0.707f, -8.0f, kRate);
    EXPECT_LT(std::abs(db(high.magnitude_at(20'000.0f, kRate)) + 8.0f), 0.5f);
    EXPECT_LT(std::abs(db(high.magnitude_at(50.0f, kRate))), 0.3f);
}

// A lowpass at Q = 1/sqrt(2) is Butterworth: -3 dB at the corner.
TEST(Biquad, AButterworthLowpassIsThreeDecibelsDownAtTheCorner) {
    const Biquad filter = Biquad::low_pass(1000.0f, kFrac1Sqrt2, kRate);
    EXPECT_LT(std::abs(db(filter.magnitude_at(1000.0f, kRate)) + 3.01f), 0.1f);
    EXPECT_LT(std::abs(db(filter.magnitude_at(20.0f, kRate))), 0.01f);
    // Two poles: 12 dB per octave above the corner. Measured well below
    // Nyquist, because the double zero there steepens the digital slope -
    // at 8 kHz against a 48 kHz rate it already reads over 13 dB.
    const float one = db(filter.magnitude_at(2000.0f, kRate));
    const float two = db(filter.magnitude_at(4000.0f, kRate));
    EXPECT_LT(std::abs(one - two - 12.0f), 0.5f) << one << " then " << two;
}

TEST(Biquad, AHighpassMirrorsTheLowpass) {
    const Biquad filter = Biquad::high_pass(1000.0f, kFrac1Sqrt2, kRate);
    EXPECT_LT(std::abs(db(filter.magnitude_at(1000.0f, kRate)) + 3.01f), 0.1f);
    EXPECT_LT(std::abs(db(filter.magnitude_at(20'000.0f, kRate))), 0.1f);
}

TEST(Biquad, ANotchNullsItsCentre) {
    const Biquad filter = Biquad::notch(1000.0f, 4.0f, kRate);
    EXPECT_LT(db(filter.magnitude_at(1000.0f, kRate)), -60.0f);
    EXPECT_LT(std::abs(db(filter.magnitude_at(200.0f, kRate))), 0.5f);
    EXPECT_LT(std::abs(db(filter.magnitude_at(5000.0f, kRate))), 0.5f);
}

TEST(Biquad, ABandpassPeaksAtUnity) {
    const Biquad filter = Biquad::band_pass(1000.0f, 2.0f, kRate);
    EXPECT_LT(std::abs(db(filter.magnitude_at(1000.0f, kRate))), 0.01f);
    EXPECT_LT(db(filter.magnitude_at(50.0f, kRate)), -20.0f);
    EXPECT_LT(db(filter.magnitude_at(20'000.0f, kRate)), -20.0f);
}

// An allpass must be exactly flat and must still rotate phase.
TEST(Biquad, AnAllpassIsFlatButTurnsThePhase) {
    const Biquad filter = Biquad::all_pass(1000.0f, 0.707f, kRate);
    for (const float hz : {20.0f, 200.0f, 1000.0f, 5000.0f, 20'000.0f}) {
        EXPECT_LT(std::abs(db(filter.magnitude_at(hz, kRate))), 0.01f)
            << "allpass was not flat at " << hz << " Hz";
    }
    const float phase =
        std::arg(filter.response_at(1000.0f, kRate)) * 180.0f / std::numbers::pi_v<float>;
    EXPECT_LT(std::abs(std::abs(phase) - 180.0f), 1.0f)
        << "expected half a turn at the centre, got " << phase;
}

// Zero gain must produce an exact pass-through, not something close to one.
// A band left at 0 dB is the common case and must cost nothing.
TEST(Biquad, ZeroGainIsAPassThrough) {
    const Biquad filter = Biquad::peaking(1000.0f, 2.0f, 0.0f, kRate);
    for (const float hz : {20.0f, 1000.0f, 20'000.0f}) {
        EXPECT_LT(std::abs(filter.magnitude_at(hz, kRate) - 1.0f), 1e-5f);
    }
}

// A frequency above Nyquist is unrealisable, and the answer must be a
// pass-through rather than a filter full of NaNs.
TEST(Biquad, AnUnrealisableDesignBecomesAPassThrough) {
    const Biquad filters[] = {
        Biquad::peaking(30'000.0f, 2.0f, 6.0f, kRate),
        Biquad::peaking(0.0f, 2.0f, 6.0f, kRate),
        Biquad::peaking(std::numeric_limits<float>::quiet_NaN(), 2.0f, 6.0f, kRate),
        Biquad::low_pass(1000.0f, 2.0f, 0.0f),
    };
    for (const Biquad& filter : filters) {
        EXPECT_EQ(filter, Biquad::identity());
    }
}

// The measured response of the running filter must match the computed one.
// This is the check that catches a coefficient typo, which a magnitude
// formula sharing the same typo would not.
TEST(Biquad, TheRunningFilterMatchesItsComputedResponse) {
    struct Case {
        const char* name;
        Biquad filter;
        float hz;
    };
    const Case cases[] = {
        {"peak", Biquad::peaking(1000.0f, 1.5f, 9.0f, kRate), 1000.0f},
        {"shelf", Biquad::low_shelf(300.0f, 0.7f, -6.0f, kRate), 60.0f},
        {"lowpass", Biquad::low_pass(2000.0f, 0.707f, kRate), 4000.0f},
        {"highpass", Biquad::high_pass(500.0f, 0.707f, kRate), 200.0f},
    };

    for (Case c : cases) {
        const float predicted = db(c.filter.magnitude_at(c.hz, kRate));

        // Run a tone through and measure the settled level.
        //
        // RMS, not peak. Sampling a sine rarely lands on its crest, so the
        // largest sample understates the amplitude by up to
        // 1 - cos(pi/samples_per_cycle) - 0.3 dB at 4 kHz here, which is
        // twice the tolerance this test is trying to enforce.
        constexpr int frames = 48'000;
        double sum = 0.0;
        std::uint32_t counted = 0;
        for (int frame = 0; frame < frames; ++frame) {
            const float x = std::sin(kTau * c.hz * static_cast<float>(frame) / kRate);
            const float y = c.filter.process(x);
            // Skip the transient; the tail is the steady state.
            if (frame > frames / 2) {
                sum += static_cast<double>(y) * static_cast<double>(y);
                ++counted;
            }
        }
        const auto rms = static_cast<float>(std::sqrt(sum / static_cast<double>(counted)));
        // Referenced to the input's RMS, which for a unit sine is 1/sqrt(2).
        const float measured = db(rms / kFrac1Sqrt2);
        EXPECT_LT(std::abs(measured - predicted), 0.15f)
            << c.name << ": computed " << predicted << " dB but measured " << measured << " dB";
    }
}

// Resetting must clear the tail without changing the filter.
TEST(Biquad, ResetClearsTheStateButNotTheCoefficients) {
    Biquad filter = Biquad::low_pass(500.0f, 0.707f, kRate);
    const Biquad before = filter;
    for (int i = 0; i < 100; ++i) {
        filter.process(1.0f);
    }
    filter.reset();
    EXPECT_EQ(filter.process(0.0f), 0.0f) << "a cleared filter must output zero";
    EXPECT_EQ(filter.b0, before.b0);
    EXPECT_EQ(filter.a1, before.a1);
}

}  // namespace
}  // namespace analyzer::dsp
