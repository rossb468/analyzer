// Ported from crates/analyzer-dsp/src/octave.rs.

#include "dsp/octave.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "dsp/generator.hpp"
#include "dsp/window.hpp"
#include "support/spectra.hpp"

namespace analyzer::dsp {
namespace {

constexpr float kRate = 48'000.0f;
constexpr std::size_t kSize = 8192;

std::vector<float> nominal_centres(const OctaveBands& bands) {
    std::vector<float> nominal;
    for (const Band& band : bands.bands()) {
        nominal.push_back(band.nominal_hz());
    }
    return nominal;
}

// Published octave centres. These are the numbers on every analyser and every
// noise report, so they are the real specification.
TEST(OctaveBands, FullOctaveCentresMatchThePublishedSeries) {
    const OctaveBands bands(1, 20.0f, 20'000.0f);
    EXPECT_EQ(nominal_centres(bands),
              (std::vector<float>{31.5f, 63.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f,
                                  8000.0f, 16'000.0f}));
}

TEST(OctaveBands, ThirdOctaveCentresMatchThePublishedSeries) {
    const auto bands = OctaveBands::third_octave();
    EXPECT_EQ(nominal_centres(bands),
              (std::vector<float>{
                  20.0f,   25.0f,   31.5f,   40.0f,     50.0f,     63.0f,     80.0f,    100.0f,
                  125.0f,  160.0f,  200.0f,  250.0f,    315.0f,    400.0f,    500.0f,   630.0f,
                  800.0f,  1000.0f, 1250.0f, 1600.0f,   2000.0f,   2500.0f,   3150.0f,  4000.0f,
                  5000.0f, 6300.0f, 8000.0f, 10'000.0f, 12'500.0f, 16'000.0f, 20'000.0f}));
}

// 1 kHz is the reference, so an odd fraction must land a band exactly on it.
TEST(OctaveBands, OddFractionsCentreABandOnOneKilohertz) {
    for (const std::uint32_t fraction : {1u, 3u, 5u}) {
        const OctaveBands bands(fraction, 900.0f, 1100.0f);
        const auto on_reference =
            std::any_of(bands.bands().begin(), bands.bands().end(),
                        [](const Band& b) { return std::abs(b.centre_hz - 1000.0f) < 0.1f; });
        EXPECT_TRUE(on_reference) << "1/" << fraction << " octave should have a band at 1 kHz";
    }
}

// Even fractions straddle the reference rather than centring on it.
TEST(OctaveBands, EvenFractionsStraddleOneKilohertz) {
    const OctaveBands bands(6, 900.0f, 1100.0f);
    EXPECT_FALSE(std::any_of(bands.bands().begin(), bands.bands().end(), [](const Band& b) {
        return std::abs(b.centre_hz - 1000.0f) < 1.0f;
    })) << "1/6 octave should straddle 1 kHz, not centre on it";
    // But a band edge should fall very close to it.
    EXPECT_TRUE(std::any_of(bands.bands().begin(), bands.bands().end(), [](const Band& b) {
        return std::abs(b.lower_hz - 1000.0f) < 5.0f || std::abs(b.upper_hz - 1000.0f) < 5.0f;
    }));
}

TEST(OctaveBands, AdjacentBandsMeetWithoutGapsOrOverlap) {
    for (const std::uint32_t fraction : {1u, 3u, 6u, 12u, 24u}) {
        const OctaveBands bands(fraction, 50.0f, 10'000.0f);
        const auto all = bands.bands();
        for (std::size_t i = 0; i + 1 < all.size(); ++i) {
            const float gap = std::abs(all[i + 1].lower_hz - all[i].upper_hz);
            ASSERT_LT(gap, all[i].upper_hz * 1e-4f)
                << "1/" << fraction << ": gap of " << gap << " Hz between " << all[i].centre_hz
                << " and " << all[i + 1].centre_hz;
        }
    }
}

TEST(OctaveBands, BandWidthFollowsTheFraction) {
    for (const std::uint32_t fraction : {1u, 3u, 6u, 12u}) {
        const OctaveBands bands(fraction, 900.0f, 1200.0f);
        const Band& band = bands.bands().front();
        const float ratio = band.upper_hz / band.lower_hz;
        const float expected = std::pow(kOctaveRatio, 1.0f / static_cast<float>(fraction));
        EXPECT_LT(std::abs(ratio - expected), 1e-4f)
            << "1/" << fraction << ": ratio " << ratio << ", expected " << expected;
    }
}

// The definitive test. Pink noise is equal energy per octave, so summed into
// octave bands it must come out flat - which simultaneously validates the band
// edges and the generator's pink filter.
TEST(OctaveBands, PinkNoiseIsFlatAcrossOctaveBands) {
    Generator generator(kRate, Signal::pink_noise(0.5f), 1);
    std::vector<float> samples(kSize * 32, 0.0f);
    generator.fill(samples);

    const auto bins = test::average_db_fs(samples, kRate, kSize, WindowKind::hann());

    const OctaveBands bands(1, 63.0f, 8000.0f);
    std::vector<float> levels(bands.size(), 0.0f);
    bands.apply(bins, kRate / static_cast<float>(kSize), levels);

    float sum = 0.0f;
    for (const float level : levels) {
        sum += level;
    }
    const float mean = sum / static_cast<float>(levels.size());
    for (std::size_t index = 0; index < levels.size(); ++index) {
        EXPECT_LT(std::abs(levels[index] - mean), 1.5f)
            << "band " << index << " at " << bands.bands()[index].nominal_hz() << " Hz read "
            << levels[index] << ", mean " << mean;
    }
}

// White noise is equal energy per hertz, so each octave up holds twice the
// bandwidth and reads 3 dB hotter.
TEST(OctaveBands, WhiteNoiseRisesThreeDecibelsPerOctaveBand) {
    Generator generator(kRate, Signal::white_noise(0.5f), 2);
    std::vector<float> samples(kSize * 32, 0.0f);
    generator.fill(samples);

    const auto bins = test::average_db_fs(samples, kRate, kSize, WindowKind::hann());

    const OctaveBands bands(1, 125.0f, 8000.0f);
    std::vector<float> levels(bands.size(), 0.0f);
    bands.apply(bins, kRate / static_cast<float>(kSize), levels);

    for (std::size_t i = 0; i + 1 < levels.size(); ++i) {
        const float step = levels[i + 1] - levels[i];
        EXPECT_LT(std::abs(step - 3.0f), 0.7f) << "expected +3 dB per octave, got " << step;
    }
}

// Power sums, decibels do not. Two equal bins in one band must read 3 dB above
// one of them.
TEST(OctaveBands, BandPowerSumsRatherThanAveragingDecibels) {
    const float spacing = 10.0f;
    // Bins at 100 and 110 Hz, both at -20 dB, inside one wide band.
    std::vector<float> bins(50, kBandFloorDb);
    bins[10] = -20.0f;
    bins[11] = -20.0f;

    const OctaveBands bands(1, 90.0f, 130.0f);
    std::vector<float> levels(bands.size(), 0.0f);
    bands.apply(bins, spacing, levels);

    EXPECT_LT(std::abs(levels[0] - -16.9897f), 0.01f)
        << "two equal bins should sum to +3 dB, got " << levels[0];
}

// Honesty about the resolution limit. A third-octave band at 25 Hz is 5.8 Hz
// wide, narrower than an 11.7 Hz bin, and must be reported as unresolvable
// rather than given a confident number.
TEST(OctaveBands, NarrowBandsAreReportedAsUnresolvable) {
    const auto bands = OctaveBands::third_octave();
    const float spacing = 48'000.0f / 4096.0f;

    EXPECT_FALSE(bands.is_resolvable(0, spacing)) << "20 Hz third-octave is 4.6 Hz wide";
    EXPECT_FALSE(bands.is_resolvable(1, spacing)) << "25 Hz third-octave is 5.8 Hz wide";

    const auto first = bands.first_resolvable(spacing);
    ASSERT_TRUE(first);
    EXPECT_GT(*first, 0u) << "some low bands must be unresolvable at this spacing";
    for (std::size_t index = *first; index < bands.size(); ++index) {
        EXPECT_TRUE(bands.is_resolvable(index, spacing))
            << "band " << index << " should be resolvable once the first one is";
    }
    // A much longer FFT resolves everything.
    EXPECT_TRUE(bands.is_resolvable(0, 48'000.0f / 65'536.0f));
}

TEST(OctaveBands, UnresolvableBandsStillProduceAFiniteNumber) {
    const auto bands = OctaveBands::third_octave();
    const float spacing = 48'000.0f / 4096.0f;
    const std::vector<float> bins(2049, -40.0f);
    std::vector<float> levels(bands.size(), 0.0f);
    bands.apply(bins, spacing, levels);
    EXPECT_TRUE(
        std::all_of(levels.begin(), levels.end(), [](float l) { return std::isfinite(l); }));
    EXPECT_LT(std::abs(levels[0] - -40.0f), 0.1f) << "nearest bin should be used";
}

TEST(OctaveBands, DcIsExcludedFromTheLowestBand) {
    std::vector<float> bins(100, -90.0f);
    bins[0] = 40.0f;
    const OctaveBands bands(1, 20.0f, 200.0f);
    std::vector<float> levels(bands.size(), 0.0f);
    bands.apply(bins, 10.0f, levels);
    for (const float level : levels) {
        EXPECT_LT(level, -60.0f) << "DC leaked into a band";
    }
}

TEST(OctaveBands, DegenerateInputFloorsRatherThanPanicking) {
    const auto bands = OctaveBands::third_octave();
    std::vector<float> levels(bands.size(), 0.0f);

    bands.apply({}, 11.7f, levels);
    EXPECT_TRUE(std::all_of(levels.begin(), levels.end(),
                            [](float l) { return l <= kBandFloorDb + 1e-3f; }));

    bands.apply(std::vector<float>(100, -40.0f), 0.0f, levels);
    EXPECT_TRUE(std::all_of(levels.begin(), levels.end(),
                            [](float l) { return l <= kBandFloorDb + 1e-3f; }));
}

TEST(OctaveBands, FinerFractionsProduceMoreBands) {
    std::size_t previous = 0;
    for (const std::uint32_t fraction : {1u, 3u, 6u, 12u, 24u, 48u}) {
        const std::size_t count = OctaveBands(fraction, 20.0f, 20'000.0f).size();
        EXPECT_GT(count, previous) << "1/" << fraction << " gave " << count << " bands";
        previous = count;
    }
}

TEST(OctaveBandsDeathTest, AZeroFractionIsRejected) {
    EXPECT_DEATH(OctaveBands(0, 20.0f, 20'000.0f), "fraction must be at least 1");
}

}  // namespace
}  // namespace analyzer::dsp
