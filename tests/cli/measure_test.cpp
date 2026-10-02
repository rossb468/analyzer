#include "cli/measure.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "cli/error.hpp"
#include "cli/text.hpp"

namespace analyzer::cli {
namespace {

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        out.push_back(line);
    }
    return out;
}

std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream stream(line);
    for (std::string word; stream >> word;) {
        out.push_back(word);
    }
    return out;
}

// The first number on the first line containing `label`.
std::optional<float> field(const std::string& report, const std::string& label) {
    for (const std::string& line : lines(report)) {
        if (line.find(label) == std::string::npos) {
            continue;
        }
        for (const std::string& word : words(line)) {
            if (const auto number = text::parse_f32(word)) {
                return number;
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// What follows the "# measured:" line.
std::vector<std::string> measured_lines(const std::string& report) {
    std::vector<std::string> out;
    bool seen = false;
    for (std::string& line : lines(report)) {
        seen = seen || line.find("# measured:") != std::string::npos;
        if (seen) {
            out.push_back(std::move(line));
        }
    }
    return out;
}

// The whole Milestone 2 chain over a room whose answers are known in
// advance: sweep, convolve, deconvolve, measure.
TEST(Measure, TheDemoRecoversTheRoomItBuilt) {
    const std::string report = demo(MeasureOptions{});

    // The direct arrival was built at 10 ms.
    std::optional<float> arrival;
    for (const std::string& line : measured_lines(report)) {
        if (line.find("direct arrival") != std::string::npos) {
            const auto columns = words(line);
            ASSERT_GE(columns.size(), 4u);
            arrival = text::parse_f32(columns[3]);
            break;
        }
    }
    ASSERT_TRUE(arrival.has_value()) << "an arrival should be reported";
    EXPECT_LT(std::abs(*arrival - 10.0f), 0.2f)
        << "arrival measured at " << *arrival << " ms, built at 10";
}

TEST(Measure, TheDemoRecoversTheReverberationItBuilt) {
    const std::string report = demo(MeasureOptions{});
    const auto t20 = field(report, "T20");
    ASSERT_TRUE(t20.has_value()) << "T20 should be reported";
    // The tail was built with a 0.45 s reverberation time.
    EXPECT_LT(std::abs(*t20 - 0.45f), 0.1f) << "T20 measured " << *t20 << " s, built 0.45";
}

TEST(Measure, TheDemoFindsTheReflectionsItBuilt) {
    const std::string report = demo(MeasureOptions{});
    std::string measured;
    for (const std::string& line : measured_lines(report)) {
        measured += line + "\n";
    }
    std::size_t reflections = 0;
    for (std::size_t at = measured.find("reflection"); at != std::string::npos;
         at = measured.find("reflection", at + 1)) {
        ++reflections;
    }
    EXPECT_GE(reflections, 2u) << "expected to find several reflections, found " << reflections;
}

TEST(Measure, TheReportMarksWhichFrequenciesTheGateCanSupport) {
    const std::string report = demo(MeasureOptions{});
    EXPECT_NE(report.find("valid above"), std::string::npos);
    // A 5 ms gate cannot support 20 Hz, but can support 10 kHz.
    std::vector<std::string> rows;
    for (const std::string& line : lines(report)) {
        if (!line.starts_with('#') && line.find('\t') != std::string::npos) {
            rows.push_back(line);
        }
    }
    ASSERT_FALSE(rows.empty());
    EXPECT_TRUE(rows.front().ends_with("no")) << "20 Hz is not trustworthy";
    EXPECT_TRUE(rows.back().ends_with("yes")) << "20 kHz is";
}

TEST(Measure, ALongerGateLowersTheTrustworthyLimit) {
    MeasureOptions tight_options;
    tight_options.gate_ms = 5.0f;
    MeasureOptions wide_options;
    wide_options.gate_ms = 40.0f;
    const std::string tight = demo(tight_options);
    const std::string wide = demo(wide_options);

    const auto limit = [](const std::string& report) {
        for (const std::string& line : lines(report)) {
            if (line.find("valid above") != std::string::npos) {
                const auto columns = words(line);
                if (columns.size() >= 2) {
                    return text::parse_f32(columns[columns.size() - 2]).value_or(std::nanf(""));
                }
            }
        }
        return std::nanf("");
    };
    EXPECT_LT(limit(wide), limit(tight))
        << "a wider gate should reach lower: " << limit(wide) << " vs " << limit(tight);
}

TEST(Measure, SilenceIsReportedRatherThanProducingAReport) {
    const std::vector<float> silence(4096, 0.0f);
    EXPECT_THROW(analyse(silence, silence, 48'000.0f, MeasureOptions{}), CliError);
}

}  // namespace
}  // namespace analyzer::cli
