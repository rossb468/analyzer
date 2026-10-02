#include "cli/report.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

namespace analyzer::cli {
namespace {

Meta meta() {
    return Meta{
        .source = "test",
        .channels = 1,
        .channel = 0,
        .sample_rate = 48'000.0,
        .window = dsp::WindowKind::hann(),
        .overlap = dsp::Overlap::ThreeQuarters,
        .averaging = dsp::Averaging::infinite(),
        .enbw_hz = 17.6f,
        .fft_size = 8,
        .hop = 2,
    };
}

engine::SpectrumFrame frame() {
    engine::SpectrumFrame f;
    f.sequence = 1;
    f.bins = {-90.0f, -6.0f, -70.0f, -80.0f, -95.0f};
    f.bin_spacing_hz = 100.0f;
    f.sample_rate = 48'000.0f;
    f.frames_averaged = 12;
    f.overruns = 0;
    return f;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        out.push_back(line);
    }
    return out;
}

std::vector<std::string> data_rows(const std::string& text) {
    std::vector<std::string> rows;
    for (std::string& line : lines(text)) {
        if (!line.starts_with('#')) {
            rows.push_back(std::move(line));
        }
    }
    return rows;
}

TEST(Report, EmitsOneRowPerBinWithAHeader) {
    const std::string text = render(frame(), meta(), std::nullopt, false);
    EXPECT_NE(text.find("# sample rate: 48000 Hz"), std::string::npos);
    EXPECT_NE(text.find("# frames averaged: 12"), std::string::npos);
    EXPECT_EQ(data_rows(text).size(), 5u);
}

TEST(Report, PeakOnlyEmitsTheLoudestBin) {
    const std::string text = render(frame(), meta(), std::nullopt, true);
    const auto rows = data_rows(text);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].starts_with("100.000000")) << "row was \"" << rows[0] << '"';
}

TEST(Report, MinDbDropsQuietBins) {
    const std::string text = render(frame(), meta(), -75.0f, false);
    EXPECT_EQ(data_rows(text).size(), 2u) << "only -6 and -70 clear a -75 floor";
}

TEST(Report, OverrunsAreSurfacedInTheHeader) {
    engine::SpectrumFrame dropped = frame();
    dropped.overruns = 3;
    const std::string text = render(dropped, meta(), std::nullopt, true);
    EXPECT_NE(text.find("WARNING: 3 dropped block(s)"), std::string::npos);
}

TEST(Report, ACleanRunCarriesNoWarning) {
    EXPECT_EQ(render(frame(), meta(), std::nullopt, true).find("WARNING"), std::string::npos);
}

// The header is what the golden files compare, and it carries the Rust core's
// Debug spelling of the window and the averaging mode.
TEST(Report, NamesWindowsAndAveragingAsTheRustCoreDid) {
    EXPECT_EQ(describe(dsp::WindowKind::rectangular()), "Rectangular");
    EXPECT_EQ(describe(dsp::WindowKind::blackman_harris()), "BlackmanHarris");
    EXPECT_EQ(describe(dsp::WindowKind::flat_top()), "FlatTop");
    EXPECT_EQ(describe(dsp::WindowKind::tukey(0.25f)), "Tukey { alpha: 0.25 }");
    // A whole number keeps its `.0`, as Rust's `{:?}` does.
    EXPECT_EQ(describe(dsp::WindowKind::tukey(1.0f)), "Tukey { alpha: 1.0 }");
    EXPECT_EQ(describe(dsp::Averaging::none()), "None");
    EXPECT_EQ(describe(dsp::Averaging::infinite()), "Infinite");
    EXPECT_EQ(describe(dsp::Averaging::peak_hold()), "PeakHold");
    EXPECT_EQ(describe(dsp::Averaging::exponential(0.2f)), "Exponential { alpha: 0.2 }");
}

// The last of several equal maxima wins (std::max_element would pick the first). A silent spectrum
// is all equal, so the answer is its top bin rather than DC.
TEST(Report, PeakOfAFlatSpectrumIsTheLastBin) {
    engine::SpectrumFrame silent = frame();
    silent.bins.assign(4, -200.0f);
    const auto rows = data_rows(render(silent, meta(), std::nullopt, true));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].starts_with("300.000000")) << rows[0];
}

}  // namespace
}  // namespace analyzer::cli
