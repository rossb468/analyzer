// Ported from crates/analyzer-plot/src/reduce.rs, plus one test for the
// allocation-free reuse the header promises.

#include "plot/reduce.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::plot {
namespace {

constexpr float kSpacing = 48000.0f / 4096.0f;  // 11.71875 Hz

std::vector<float> flat_bins(float level) {
    return std::vector<float>(2049, level);
}

// Coherence must show the worst value in a column, not the best.
TEST(ReduceLinear, LinearMinKeepsTheDropout) {
    const FrequencyAxis axis = FrequencyAxis::audible(1000.0f);
    std::vector<float> values(2049, 1.0f);
    values[1000] = 0.2f;
    Trace trace;
    // Few columns, so many bins land in each and the dropout must survive.
    reduce_linear(values, kSpacing, axis, 64, LinearReduction::Min, 0.0f, trace);
    const float lowest = *std::min_element(trace.points.begin(), trace.points.end());
    EXPECT_NEAR(lowest, 0.2f, 1e-6f) << "dropout was lost, got " << lowest;
}

// The wrap case that a plain arithmetic mean gets exactly backwards.
TEST(CircularMean, CircularMeanAveragesAcrossTheWrap) {
    using detail::circular_mean_degrees;
    const float across_wrap[] = {179.0f, -179.0f};
    const float symmetric[] = {10.0f, -10.0f};
    const float quarter[] = {90.0f, 0.0f};
    EXPECT_NEAR(std::abs(circular_mean_degrees(across_wrap)), 180.0f, 0.01f);
    EXPECT_LT(std::abs(circular_mean_degrees(symmetric)), 0.01f);
    EXPECT_NEAR(circular_mean_degrees(quarter), 45.0f, 0.01f);
}

// Opposed angles have no mean direction; the answer must still be a number.
TEST(CircularMean, CircularMeanOfOpposedAnglesIsNotNan) {
    const float opposed[] = {0.0f, 180.0f};
    EXPECT_TRUE(std::isfinite(detail::circular_mean_degrees(opposed)));
}

// An empty column reads the fill value, because zero coherence is a real
// reading and negative infinity is not.
TEST(ReduceLinear, LinearReductionFillsRatherThanUsingNegativeInfinity) {
    const FrequencyAxis axis = FrequencyAxis::audible(1000.0f);
    Trace trace;
    reduce_linear({}, kSpacing, axis, 32, LinearReduction::Min, 0.0f, trace);
    EXPECT_EQ(trace.size(), 32u);
    for (const float v : trace.points) {
        EXPECT_EQ(v, 0.0f);
    }
}

// A constant field survives the round trip regardless of column density.
TEST(ReduceLinear, AFlatLinearFieldStaysFlat) {
    const FrequencyAxis axis = FrequencyAxis::audible(1000.0f);
    Trace trace;
    for (const std::size_t columns : {37u, 512u, 4000u}) {
        reduce_linear(std::vector<float>(2049, 0.75f), kSpacing, axis, columns,
                      LinearReduction::Mean, 0.0f, trace);
        for (std::size_t i = 0; i < trace.points.size(); ++i) {
            ASSERT_NEAR(trace.points[i], 0.75f, 1e-4f)
                << columns << " columns, column " << i << " read " << trace.points[i];
        }
    }
}

TEST(Reduce, AFlatSpectrumStaysFlat) {
    const FrequencyAxis axis = FrequencyAxis::audible(1000.0f);
    Trace trace;
    reduce(flat_bins(-40.0f), kSpacing, axis, 1000, Reduction::Max, trace);

    ASSERT_EQ(trace.size(), 1000u);
    for (std::size_t i = 0; i < trace.points.size(); ++i) {
        ASSERT_NEAR(trace.points[i], -40.0f, 0.01f)
            << "column " << i << " read " << trace.points[i] << ", expected -40";
    }
}

// The property the whole module exists for: no column may be left unfilled,
// including down in the bass where bins are sparser than pixels.
TEST(Reduce, EveryColumnIsFilledEvenWhereBinsAreSparse) {
    const FrequencyAxis axis = FrequencyAxis::audible(1600.0f);
    Trace trace;
    reduce(flat_bins(-30.0f), kSpacing, axis, 1600, Reduction::Max, trace);

    for (std::size_t i = 0; i < trace.points.size(); ++i) {
        ASSERT_TRUE(std::isfinite(trace.points[i]))
            << "column " << i << " (" << axis.x_to_freq(static_cast<float>(i))
            << " Hz) was never filled";
    }
}

// A single loud bin must survive reduction at any width. Losing it is the
// failure mode that makes a display untrustworthy.
TEST(Reduce, ANarrowPeakSurvivesAtEveryWidth) {
    std::vector<float> bins = flat_bins(-90.0f);
    // Bin 853 is about 10 kHz, deep in the over-sampled region.
    bins[853] = 0.0f;

    for (const float width : {200.0f, 640.0f, 1000.0f, 1440.0f, 3000.0f}) {
        const FrequencyAxis axis = FrequencyAxis::audible(width);
        Trace trace;
        reduce(bins, kSpacing, axis, static_cast<std::size_t>(width), Reduction::Max, trace);

        const float loudest = *std::max_element(trace.points.begin(), trace.points.end());
        EXPECT_GT(loudest, -1.0f) << "peak lost at width " << width << ": loudest was " << loudest;
    }
}

// Mean must average power, not decibels. A column holding 0 dB and -20 dB
// averages to -2.4 dB in power terms; averaging the decibels would give -10.
TEST(MeanDb, MeanAveragesPowerNotDecibels) {
    const float levels[] = {0.0f, -20.0f};
    const float mean = detail::mean_db(levels);
    const float expected = 10.0f * std::log10((1.0f + 0.01f) / 2.0f);
    EXPECT_NEAR(mean, expected, 1e-4f) << "got " << mean << ", want " << expected;
    EXPECT_GT(mean, -10.0f) << "power mean must exceed the decibel mean";
}

TEST(Reduce, MaxAndMeanAgreeOnAFlatSpectrum) {
    const FrequencyAxis axis = FrequencyAxis::audible(800.0f);
    Trace peak;
    Trace mean;
    reduce(flat_bins(-55.0f), kSpacing, axis, 800, Reduction::Max, peak);
    reduce(flat_bins(-55.0f), kSpacing, axis, 800, Reduction::Mean, mean);

    ASSERT_EQ(peak.size(), mean.size());
    for (std::size_t i = 0; i < peak.size(); ++i) {
        ASSERT_NEAR(peak.points[i], mean.points[i], 0.01f) << "column " << i;
    }
}

// Max must dominate mean wherever a column holds a peak among quiet bins.
TEST(Reduce, MaxReadsHigherThanMeanOnAPeakySpectrum) {
    std::vector<float> bins = flat_bins(-90.0f);
    for (std::size_t k = 100; k < 2000; k += 37) {
        bins[k] = -10.0f;
    }
    const FrequencyAxis axis = FrequencyAxis::audible(400.0f);
    Trace peak;
    Trace mean;
    reduce(bins, kSpacing, axis, 400, Reduction::Max, peak);
    reduce(bins, kSpacing, axis, 400, Reduction::Mean, mean);

    const float peak_total = std::accumulate(peak.points.begin(), peak.points.end(), 0.0f);
    const float mean_total = std::accumulate(mean.points.begin(), mean.points.end(), 0.0f);
    EXPECT_GT(peak_total, mean_total) << peak_total << " vs " << mean_total;
}

TEST(Reduce, ARisingSpectrumStaysMonotonicThroughReduction) {
    // Level rising with bin index must not develop dips.
    std::vector<float> bins(2049);
    for (std::size_t k = 0; k < bins.size(); ++k) {
        bins[k] = -100.0f + static_cast<float>(k) * 0.04f;
    }
    const FrequencyAxis axis = FrequencyAxis::audible(900.0f);
    Trace trace;
    reduce(bins, kSpacing, axis, 900, Reduction::Max, trace);

    for (std::size_t i = 1; i < trace.size(); ++i) {
        ASSERT_GE(trace.points[i], trace.points[i - 1] - 0.01f)
            << "reduction introduced a dip: " << trace.points[i - 1] << " -> " << trace.points[i];
    }
}

TEST(Reduce, DcIsExcluded) {
    std::vector<float> bins = flat_bins(-80.0f);
    // A huge DC offset must not leak into the visible bass.
    bins[0] = 40.0f;
    const FrequencyAxis axis = FrequencyAxis::audible(500.0f);
    Trace trace;
    reduce(bins, kSpacing, axis, 500, Reduction::Max, trace);

    const float loudest = *std::max_element(trace.points.begin(), trace.points.end());
    EXPECT_LT(loudest, -70.0f) << "DC leaked into the trace: " << loudest;
}

TEST(Reduce, DegenerateInputsProduceAnEmptyButSizedTrace) {
    const FrequencyAxis axis = FrequencyAxis::audible(100.0f);
    Trace trace;

    reduce({}, kSpacing, axis, 100, Reduction::Max, trace);
    EXPECT_EQ(trace.size(), 100u);

    reduce(flat_bins(-20.0f), 0.0f, axis, 100, Reduction::Max, trace);
    EXPECT_EQ(trace.size(), 100u);

    reduce(flat_bins(-20.0f), kSpacing, axis, 0, Reduction::Max, trace);
    EXPECT_TRUE(trace.empty());
}

// Reuse must not leave stale values behind - the trace is recycled every
// frame, so a shrinking width could otherwise show last frame's tail.
TEST(Reduce, ReusingATraceDoesNotLeakOldData) {
    const FrequencyAxis axis = FrequencyAxis::audible(1000.0f);
    Trace trace;
    reduce(flat_bins(0.0f), kSpacing, axis, 1000, Reduction::Max, trace);
    reduce(flat_bins(-80.0f), kSpacing, axis, 40, Reduction::Max, trace);

    ASSERT_EQ(trace.size(), 40u);
    for (const float level : trace.points) {
        EXPECT_NEAR(level, -80.0f, 0.5f);
    }
}

// Not in the Rust suite. The header promises a recycled trace does not
// allocate at a steady or shrinking width; the vector's storage staying put is
// the observable form of that.
TEST(Reduce, ReusingATraceKeepsItsStorage) {
    const FrequencyAxis axis = FrequencyAxis::audible(1000.0f);
    const std::vector<float> bins = flat_bins(-40.0f);
    Trace trace = Trace::with_width(1000);
    const float* storage = trace.points.data();

    reduce(bins, kSpacing, axis, 1000, Reduction::Max, trace);
    EXPECT_EQ(trace.points.data(), storage);
    reduce(bins, kSpacing, axis, 400, Reduction::Mean, trace);
    EXPECT_EQ(trace.points.data(), storage);
    reduce_linear(bins, kSpacing, axis, 1000, LinearReduction::Min, 0.0f, trace);
    EXPECT_EQ(trace.points.data(), storage);
}

// Not in the Rust suite. Rust's `as` saturates; a plain static_cast of an
// out-of-range float is undefined behaviour, which the sanitizer build would
// flag. A bin spacing this small sends every column's bin index far past the
// end of the spectrum.
TEST(Reduce, AnAbsurdlySmallBinSpacingStaysDefined) {
    const FrequencyAxis axis = FrequencyAxis::audible(100.0f);
    Trace trace;
    reduce(flat_bins(-20.0f), 1e-30f, axis, 100, Reduction::Max, trace);
    ASSERT_EQ(trace.size(), 100u);
    for (const float level : trace.points) {
        EXPECT_NEAR(level, -20.0f, 1e-4f);
    }
}

}  // namespace
}  // namespace analyzer::plot
