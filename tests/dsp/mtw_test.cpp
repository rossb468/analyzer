// Ported from crates/analyzer-dsp/src/mtw.rs.

#include "dsp/mtw.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "support/generated_signals.hpp"

namespace analyzer::dsp {
namespace {

constexpr float kRate = 48'000.0f;

MtwConfig config() {
    MtwConfig c;
    c.sample_rate = kRate;
    c.largest_fft = 16'384;
    c.smallest_fft = 256;
    c.averaging = TransferAveraging::infinite();
    return c;
}

// Pink noise at the amplitude every test here used.
std::vector<float> pink(std::size_t samples, std::uint64_t seed) {
    return test::pink_noise(samples, 0.5f, seed);
}

// `signal` delayed by `delay` samples, zero-filled at the start.
std::vector<float> delayed(const std::vector<float>& signal, std::size_t delay) {
    std::vector<float> out(signal.size(), 0.0f);
    std::copy(signal.begin(), signal.end() - static_cast<std::ptrdiff_t>(delay),
              out.begin() + static_cast<std::ptrdiff_t>(delay));
    return out;
}

// Points with enough reference energy and enough averaging to trust.
std::vector<MtwPoint> usable(std::span<const MtwPoint> points) {
    std::vector<MtwPoint> out;
    std::copy_if(points.begin(), points.end(), std::back_inserter(out), [](const MtwPoint& p) {
        return p.hz >= 100.0f && p.hz <= 10'000.0f && p.fft_size > 0;
    });
    return out;
}

// x mod 360 in [0, 360), with the sign of the divisor.
float rem_euclid_360(float x) {
    const float r = std::fmod(x, 360.0f);
    return (r < 0.0f) ? r + 360.0f : r;
}

TEST(MultiTimeWindow, BandsDoubleInSizeAndCoverTheWholeSpectrum) {
    const MultiTimeWindow mtw(config());
    const auto sizes = mtw.fft_sizes();
    EXPECT_EQ(sizes, (std::vector<std::size_t>{256, 512, 1024, 2048, 4096, 8192, 16'384}));
    EXPECT_LT(std::abs(mtw.finest_resolution_hz() - kRate / 16'384.0f), 1e-3f);
}

// The headline claim: far fewer points than one big FFT, while keeping the big
// FFT's resolution where it matters.
TEST(MultiTimeWindow, ThePointCountCollapsesAgainstASingleLargeFft) {
    const MultiTimeWindow mtw(config());
    constexpr std::size_t kSingleFftBins = 16'384 / 2 + 1;
    EXPECT_LT(mtw.points().size(), kSingleFftBins / 10)
        << mtw.points().size() << " points vs " << kSingleFftBins << " bins";
    // And still finer than 3 Hz down low.
    EXPECT_LT(mtw.finest_resolution_hz(), 3.0f);
}

TEST(MultiTimeWindow, AWireMeasuresFlatAcrossEveryBand) {
    const auto signal = pink(16'384 * 8, 1);
    MultiTimeWindow mtw(config());
    mtw.push(signal, signal);
    mtw.resolve();

    for (const auto& point : usable(mtw.points())) {
        EXPECT_LT(std::abs(point.magnitude_db), 0.2f)
            << point.hz << " Hz (fft " << point.fft_size << "): " << point.magnitude_db << " dB";
        EXPECT_LT(std::abs(point.phase_degrees), 2.0f)
            << point.hz << " Hz: " << point.phase_degrees << " deg";
        EXPECT_GT(point.coherence, 0.98f) << point.hz << " Hz: coherence " << point.coherence;
    }
}

// The test that decides whether splicing works at all. A flat gain must be flat
// *through* the crossovers - a discontinuity there is the classic MTW failure
// and it is immediately visible on a display.
TEST(MultiTimeWindow, AGainIsContinuousAcrossTheSplices) {
    const auto reference = pink(16'384 * 8, 2);
    std::vector<float> measurement(reference.size());
    std::transform(reference.begin(), reference.end(), measurement.begin(),
                   [](float s) { return s * 2.0f; });

    MultiTimeWindow mtw(config());
    mtw.push(reference, measurement);
    mtw.resolve();

    const auto points = usable(mtw.points());
    for (const auto& point : points) {
        EXPECT_LT(std::abs(point.magnitude_db - 6.0206f), 0.3f)
            << point.hz << " Hz (fft " << point.fft_size << "): " << point.magnitude_db << " dB";
    }

    // Specifically check the steps where the FFT size changes.
    for (std::size_t i = 1; i < points.size(); ++i) {
        const auto& before = points[i - 1];
        const auto& after = points[i];
        if (before.fft_size != after.fft_size) {
            const float step = std::abs(after.magnitude_db - before.magnitude_db);
            EXPECT_LT(step, 0.3f) << "discontinuity of " << step << " dB at the " << before.fft_size
                                  << "/" << after.fft_size << " crossover near " << before.hz
                                  << " Hz";
        }
    }
}

// The hardest continuity test. Each engine windows the signal differently, so
// if phase were referenced to the window rather than cancelling in the ratio, a
// delay would produce a visible jump at every crossover.
TEST(MultiTimeWindow, PhaseFromADelayIsContinuousAcrossTheSplices) {
    constexpr std::size_t kDelay = 24;
    const auto reference = pink(16'384 * 8, 3);
    const auto measurement = delayed(reference, kDelay);

    MultiTimeWindow mtw(config());
    mtw.push(reference, measurement);
    mtw.resolve();

    // Expected phase for a pure delay, wrapped the same way.
    const auto expected_at = [](float hz) {
        const float raw = -360.0f * hz * static_cast<float>(kDelay) / kRate;
        const float wrapped = rem_euclid_360(raw);
        return (wrapped > 180.0f) ? wrapped - 360.0f : wrapped;
    };

    for (const auto& point : usable(mtw.points())) {
        const float want = expected_at(point.hz);
        float error = std::abs(point.phase_degrees - want);
        if (error > 180.0f) {
            error = 360.0f - error;
        }
        EXPECT_LT(error, 6.0f) << point.hz << " Hz (fft " << point.fft_size << "): phase "
                               << point.phase_degrees << " deg, expected " << want << " deg";
    }
}

// Interpolating magnitude and phase separately breaks across a +-180 degree
// wrap. This uses a delay big enough to wrap phase repeatedly between adjacent
// bins, but small against the shortest window so the wrap is the only thing
// under test.
TEST(MultiTimeWindow, InterpolationSurvivesPhaseWrappingBetweenBins) {
    constexpr std::size_t kDelay = 64;
    const auto reference = pink(16'384 * 8, 4);
    const auto measurement = delayed(reference, kDelay);

    // Shortest window 1024, so 64 samples is 6% of even the shortest frame.
    auto c = config();
    c.smallest_fft = 1024;
    MultiTimeWindow mtw(c);
    mtw.push(reference, measurement);
    mtw.resolve();

    for (const auto& point : usable(mtw.points())) {
        EXPECT_LT(std::abs(point.magnitude_db), 0.3f)
            << point.hz << " Hz (fft " << point.fft_size << "): magnitude " << point.magnitude_db
            << " dB - interpolation should not lose level to a wrap";
    }
}

// Documents why the delay finder is a prerequisite rather than a nicety.
//
// A delay that is a large fraction of the analysis window means the frame of
// measurement no longer contains the same audio as the frame of reference, so
// the cross-spectrum decorrelates: magnitude reads low and coherence falls.
// That is physics, not a defect - and it is exactly what compensating the delay
// before measuring avoids.
TEST(MultiTimeWindow, UncompensatedDelayCostsMagnitudeAndCoherence) {
    const auto reference = pink(16'384 * 8, 20);

    const auto measure = [&](std::size_t delay) {
        const auto measurement = delayed(reference, delay);
        MultiTimeWindow mtw(config());
        mtw.push(reference, measurement);
        mtw.resolve();
        // Look in the top band, where the window is shortest and the effect is
        // largest.
        float magnitude = 0.0f;
        float coherence = 0.0f;
        std::size_t count = 0;
        for (const auto& p : mtw.points()) {
            if (p.hz >= 13'000.0f && p.hz <= 20'000.0f) {
                magnitude += p.magnitude_db;
                coherence += p.coherence;
                ++count;
            }
        }
        return std::pair{magnitude / static_cast<float>(count),
                         coherence / static_cast<float>(count)};
    };

    const auto [aligned_db, aligned_coherence] = measure(4);
    const auto [delayed_db, delayed_coherence] = measure(200);

    EXPECT_LT(std::abs(aligned_db), 0.3f)
        << "a nearly aligned measurement should read flat, got " << aligned_db;
    EXPECT_LT(delayed_db, aligned_db - 0.3f)
        << "200 samples against a 256-point window should cost level: " << aligned_db << " -> "
        << delayed_db << " dB";
    EXPECT_LT(delayed_coherence, aligned_coherence - 0.05f)
        << "and coherence: " << aligned_coherence << " -> " << delayed_coherence;
}

TEST(MultiTimeWindow, UncorrelatedInputGivesLowCoherenceInEveryBand) {
    const auto reference = pink(16'384 * 8, 5);
    const auto measurement = pink(16'384 * 8, 6);

    MultiTimeWindow mtw(config());
    mtw.push(reference, measurement);
    mtw.resolve();

    const auto points = usable(mtw.points());
    float sum = 0.0f;
    for (const auto& p : points) {
        sum += p.coherence;
    }
    const float mean = sum / static_cast<float>(points.size());
    EXPECT_LT(mean, 0.4f) << "mean coherence " << mean << " for uncorrelated signals";
}

TEST(MultiTimeWindow, EveryPointIsAssignedToExactlyOneBand) {
    const auto signal = pink(16'384 * 4, 7);
    MultiTimeWindow mtw(config());
    mtw.push(signal, signal);
    mtw.resolve();

    for (const auto& point : mtw.points()) {
        EXPECT_GT(point.fft_size, 0u) << point.hz << " Hz was covered by no band";
    }
}

// Band assignment must go the right way round: long windows in the bass.
TEST(MultiTimeWindow, LowFrequenciesUseTheLongestWindow) {
    const auto signal = pink(16'384 * 4, 8);
    MultiTimeWindow mtw(config());
    mtw.push(signal, signal);
    mtw.resolve();

    const auto& lowest = mtw.points().front();
    const auto& highest = mtw.points().back();
    EXPECT_GT(lowest.fft_size, highest.fft_size)
        << "bass used fft " << lowest.fft_size << " and treble used " << highest.fft_size;
    EXPECT_EQ(lowest.fft_size, 16'384u);
}

TEST(MultiTimeWindow, TheGridIsLogarithmic) {
    const MultiTimeWindow mtw(config());
    const auto points = mtw.points();
    // Consecutive ratios must be constant, which is what log spacing means.
    const float first_ratio = points[1].hz / points[0].hz;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const float ratio = points[i].hz / points[i - 1].hz;
        ASSERT_LT(std::abs(ratio - first_ratio), 1e-4f)
            << "ratio drifted: " << ratio << " vs " << first_ratio;
    }
    // 48 points per octave means each step is the 48th root of 2.
    EXPECT_LT(std::abs(first_ratio - std::pow(2.0f, 1.0f / 48.0f)), 1e-5f);
}

TEST(MultiTimeWindow, ResetClearsEveryBand) {
    const auto signal = pink(16'384 * 4, 9);
    MultiTimeWindow mtw(config());
    mtw.push(signal, signal);
    mtw.resolve();
    EXPECT_GT(mtw.frames(), 0u);

    mtw.reset();
    EXPECT_EQ(mtw.frames(), 0u);
    const auto points = mtw.points();
    EXPECT_TRUE(std::all_of(points.begin(), points.end(),
                            [](const MtwPoint& p) { return p.coherence == 0.0f; }));
}

TEST(MultiTimeWindow, WindowLengthAndResolutionAreReportedHonestly) {
    auto c = config();
    c.largest_fft = 32'768;
    const MultiTimeWindow mtw(c);
    // 32768 at 48 kHz is 1.46 Hz and 683 ms - both worth surfacing, because the
    // second is the latency before a bass reading settles.
    EXPECT_LT(std::abs(mtw.finest_resolution_hz() - 1.4648f), 0.01f);
    EXPECT_LT(std::abs(mtw.longest_window_seconds() - 0.6827f), 0.01f);
}

TEST(MultiTimeWindowDeathTest, NonPowerOfTwoSizesAreRejected) {
    auto c = config();
    c.largest_fft = 12'000;
    EXPECT_DEATH(MultiTimeWindow{c}, "FFT sizes must be powers of two");
}

TEST(MultiTimeWindowDeathTest, MismatchedLengthsAreRejected) {
    MultiTimeWindow mtw(config());
    const std::vector<float> reference(100, 0.0f);
    const std::vector<float> measurement(99, 0.0f);
    EXPECT_DEATH(mtw.push(reference, measurement), "sample aligned");
}

}  // namespace
}  // namespace analyzer::dsp
