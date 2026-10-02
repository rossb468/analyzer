// Ported from crates/analyzer-model/src/compare.rs.

#include "model/compare.hpp"

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::model {
namespace {

Response response(std::vector<ResponsePoint> points) {
    return Response{std::move(points)};
}

std::vector<double> frequencies(const Response& r) {
    std::vector<double> out;
    for (const ResponsePoint& point : r.points) {
        out.push_back(point.hz);
    }
    return out;
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

TEST(Response, ParsesOurOwnOutputFormat) {
    const std::string text =
        "# analyzer-cli spectrum\n"
        "# sample rate: 48000 Hz\n"
        "# frequency_hz\tlevel_db\n"
        "20.000000\t-40.1000\n"
        "1000.000000\t-6.0206\n"
        "20000.000000\t-45.0000\n";
    const Response parsed = Response::parse(text);
    ASSERT_EQ(parsed.points.size(), 3u);
    EXPECT_EQ(parsed.points[1], (ResponsePoint{1000.0, -6.0206}));
}

// REW's export uses `*` for comments and carries a third phase column.
TEST(Response, ParsesARewStyleExport) {
    const std::string text =
        "* Measurement data measured by REW V5.19\n"
        "* Freq(Hz) SPL(dB) Phase(degrees)\n"
        "20.000 72.431 -14.210\n"
        "1000.000 80.100 3.400\n";
    const Response parsed = Response::parse(text);
    ASSERT_EQ(parsed.points.size(), 2u);
    EXPECT_EQ(parsed.points[0], (ResponsePoint{20.0, 72.431}));
    EXPECT_EQ(parsed.points[1], (ResponsePoint{1000.0, 80.1}));
}

TEST(Response, SkipsRowsThatAreNotNumbersRatherThanFailing) {
    const std::string text = "20 -40\nlegend goes here\n\n1000 -6\nnot,numbers\n2000 -7\n";
    const Response parsed = Response::parse(text);
    EXPECT_EQ(parsed.points.size(), 3u);
}

TEST(Response, DropsDcAndNonFiniteRows) {
    const Response parsed = Response::parse("0 -100\n-5 -90\n100 NaN\n1000 -6\n");
    EXPECT_EQ(parsed.points, (std::vector<ResponsePoint>{{1000.0, -6.0}}));
}

TEST(Response, UnsortedInputIsSorted) {
    const Response parsed = Response::parse("1000 -6\n20 -40\n500 -10\n");
    EXPECT_EQ(frequencies(parsed), (std::vector<double>{20.0, 500.0, 1000.0}));
}

// Interpolating in linear frequency would put the midpoint of 100 and 1000 at
// 550 Hz; in log frequency it is at about 316, which is what a curve on a log
// axis means.
TEST(Response, InterpolationIsLogarithmic) {
    const Response curve = response({{100.0, 0.0}, {1000.0, 10.0}});
    const auto mid = curve.level_at(316.227'766);
    ASSERT_TRUE(mid.has_value());
    EXPECT_LT(std::abs(*mid - 5.0), 1e-3) << "got " << *mid;
}

// A file that stops at 20 kHz says nothing about 22 kHz, and inventing a value
// would put fabricated data into a parity result.
TEST(Response, OutsideTheCoveredRangeThereIsNoAnswer) {
    const Response curve = response({{100.0, 0.0}, {1000.0, 10.0}});
    EXPECT_FALSE(curve.level_at(50.0).has_value());
    EXPECT_FALSE(curve.level_at(2000.0).has_value());
    EXPECT_EQ(curve.level_at(100.0), std::optional(0.0));
    EXPECT_EQ(curve.level_at(1000.0), std::optional(10.0));
}

TEST(Comparison, IdenticalResponsesAgreeExactly) {
    const Response curve = response({{20.0, -40.0}, {1000.0, -6.0}, {20'000.0, -45.0}});
    const Comparison result = compare(curve, curve, 20.0, 20'000.0);
    EXPECT_EQ(result.compared, 3u);
    EXPECT_EQ(result.interpolated, 0u);
    EXPECT_EQ(result.max_deviation, 0.0);
    EXPECT_EQ(result.mean_offset, 0.0);
    EXPECT_TRUE(result.agrees_within(0.1));
}

// The distinction the whole module exists for: a pure convention difference must
// not read as a failure.
TEST(Comparison, AConstantOffsetIsSeparatedFromTheShape) {
    const Response ours = response({{20.0, -40.0}, {1000.0, -6.0}, {20'000.0, -45.0}});
    const Response theirs = response({{20.0, -43.01}, {1000.0, -9.01}, {20'000.0, -48.01}});

    const Comparison result = compare(ours, theirs, 20.0, 20'000.0);
    EXPECT_LT(std::abs(result.mean_offset - 3.01), 1e-9);
    EXPECT_LT(std::abs(result.max_deviation - 3.01), 1e-9);
    EXPECT_LT(result.max_deviation_after_offset, 1e-9) << "the shapes are identical";
    EXPECT_TRUE(result.agrees_within(0.1))
        << "a 3 dB reference convention must not read as a parity failure";
}

// And the converse: a deviation that varies with frequency must survive offset
// removal, because that is the defect worth finding.
TEST(Comparison, AFrequencyDependentDeviationSurvivesOffsetRemoval) {
    const Response ours = response({{20.0, 0.0}, {1000.0, 0.0}, {20'000.0, 0.0}});
    const Response theirs = response({{20.0, -1.0}, {1000.0, 0.0}, {20'000.0, 1.0}});

    const Comparison result = compare(ours, theirs, 20.0, 20'000.0);
    EXPECT_LT(std::abs(result.mean_offset), 1e-9) << "no net offset";
    EXPECT_LT(std::abs(result.max_deviation_after_offset - 1.0), 1e-9);
    EXPECT_FALSE(result.agrees_within(0.1)) << "a 1 dB tilt must fail";
}

TEST(Comparison, OnlyTheRequestedBandIsCompared) {
    const Response ours = response({{10.0, 50.0}, {1000.0, 0.0}, {30'000.0, 50.0}});
    const Response theirs = response({{10.0, 0.0}, {1000.0, 0.0}, {30'000.0, 0.0}});

    const Comparison result = compare(ours, theirs, 20.0, 20'000.0);
    EXPECT_EQ(result.compared, 1u) << "only the 1 kHz point is in band";
    EXPECT_EQ(result.max_deviation, 0.0);
}

// A run that is mostly interpolation should be recognisable as one.
TEST(Comparison, InterpolatedPointsAreCounted) {
    const Response ours = response({{100.0, 0.0}, {200.0, 0.0}, {1000.0, 0.0}});
    const Response theirs = response({{100.0, 0.0}, {1000.0, 0.0}});

    const Comparison result = compare(ours, theirs, 20.0, 20'000.0);
    EXPECT_EQ(result.compared, 3u);
    EXPECT_EQ(result.interpolated, 1u) << "only 200 Hz needed interpolating";
}

TEST(Comparison, PointsTheReferenceDoesNotCoverAreSkippedNotCountedAsAgreement) {
    const Response ours = response({{20.0, 0.0}, {1000.0, 0.0}});
    const Response theirs = response({{500.0, 0.0}, {2000.0, 0.0}});

    const Comparison result = compare(ours, theirs, 20.0, 20'000.0);
    EXPECT_EQ(result.compared, 1u) << "20 Hz is outside the reference";
}

TEST(Comparison, NoOverlapReportsNothingRatherThanPerfectAgreement) {
    const Response ours = response({{20.0, 0.0}});
    const Response theirs = response({{10'000.0, 0.0}});

    const Comparison result = compare(ours, theirs, 20.0, 20'000.0);
    EXPECT_EQ(result.compared, 0u);
    EXPECT_FALSE(result.agrees_within(100.0)) << "nothing compared is not a pass";
    EXPECT_TRUE(contains(result.report(), "nothing overlapped"));
}

TEST(Comparison, EmptyInputIsSurvivable) {
    const Response empty;
    const Comparison result = compare(empty, empty, 20.0, 20'000.0);
    EXPECT_EQ(result.compared, 0u);
    EXPECT_FALSE(result.report().empty());
}

TEST(Comparison, TheReportNamesBothNumbers) {
    const Response ours = response({{1000.0, 0.0}, {2000.0, 0.0}});
    const Response theirs = response({{1000.0, -3.0}, {2000.0, -3.0}});
    const std::string report = compare(ours, theirs, 20.0, 20'000.0).report();
    EXPECT_TRUE(contains(report, "constant offset")) << report;
    EXPECT_TRUE(contains(report, "+3.0000 dB")) << report;
    EXPECT_TRUE(contains(report, "offset removed")) << report;
}

}  // namespace
}  // namespace analyzer::model
