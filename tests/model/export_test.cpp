// Ported from crates/analyzer-model/src/export.rs.

#include "model/export.hpp"

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::model {
namespace {

Measurement spectrum() {
    return Measurement(MeasurementId{1}, "Left", 48'000.0,
                       // Unit magnitude at 0 degrees, then 0.5 at +90.
                       SpectrumData{{Complex64(1.0, 0.0), Complex64(0.0, 0.5)}, 10.0});
}

std::vector<std::string> data_rows(const std::string& text) {
    std::vector<std::string> rows;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        if (!line.starts_with('*')) {
            rows.push_back(line);
        }
    }
    return rows;
}

std::vector<std::string> fields(const std::string& row) {
    std::vector<std::string> out;
    std::istringstream stream(row);
    for (std::string field; stream >> field;) {
        out.push_back(field);
    }
    return out;
}

TEST(RewExport, TheColumnLayoutMatchesWhatRewImports) {
    const std::string text = to_rew_text(spectrum());
    EXPECT_NE(text.find("* Freq(Hz) SPL(dB) Phase(degrees)"), std::string::npos);

    const auto rows = data_rows(text);
    ASSERT_EQ(rows.size(), 2u);
    const auto columns = fields(rows[0]);
    ASSERT_EQ(columns.size(), 3u) << "three columns: " << rows[0];
    EXPECT_EQ(columns[0], "0.000000");
    EXPECT_EQ(columns[1], "0.0000") << "unit magnitude is 0 dB";
}

TEST(RewExport, FrequenciesFollowTheBinSpacing) {
    const auto rows = data_rows(to_rew_text(spectrum()));
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_TRUE(rows[1].starts_with("10.000000"));
}

TEST(RewExport, PhaseIsEmittedFromTheComplexValue) {
    const auto rows = data_rows(to_rew_text(spectrum()));
    ASSERT_EQ(rows.size(), 2u);
    const double phase = std::stod(fields(rows[1])[2]);
    EXPECT_LT(std::abs(phase - 90.0), 1e-3) << "got " << phase;
}

// An uncalibrated measurement must say so rather than labelling dBFS as SPL.
TEST(RewExport, UncalibratedOutputIsLabelledHonestly) {
    const std::string text = to_rew_text(spectrum());
    EXPECT_NE(text.find("not SPL calibrated"), std::string::npos);

    const auto rows = data_rows(text);
    const double level = std::stod(fields(rows[0])[1]);
    EXPECT_LT(std::abs(level - 0.0), 1e-6) << "no offset should be applied";
}

TEST(RewExport, ACalibratedMeasurementHasItsOffsetApplied) {
    Measurement m = spectrum();
    m.references.spl_offset_db = 94.0;
    const std::string text = to_rew_text(m);

    EXPECT_NE(text.find("SPL offset applied: 94"), std::string::npos);
    const auto rows = data_rows(text);
    const double level = std::stod(fields(rows[0])[1]);
    EXPECT_LT(std::abs(level - 94.0), 1e-6) << "got " << level;
}

TEST(RewExport, TransferFunctionExportCarriesCoherenceAsAComment) {
    const Measurement m(MeasurementId{2}, "TF", 48'000.0,
                        TransferFunctionData{{Complex64(1.0, 0.0)}, {0.87}, 10.0});
    const auto rows = data_rows(to_rew_text(m));
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_NE(rows[0].find("* 0.8700"), std::string::npos) << "row was " << rows[0];
    // The three real columns still come first, so a plain reader works.
    EXPECT_EQ(fields(rows[0]).size(), 5u);
}

// t = 0 sits at the recorded arrival, not at the first sample, so samples before
// it must carry negative times.
TEST(RewExport, ImpulseExportPlacesTimeZeroAtTheArrival) {
    const Measurement m(MeasurementId{3}, "IR", 48'000.0,
                        ImpulseResponseData{{0.0, 0.0, 1.0, 0.5}, 2.0});
    const auto rows = data_rows(to_rew_text(m));
    ASSERT_EQ(rows.size(), 4u);

    const double first = std::stod(fields(rows[0])[0]);
    const double third = std::stod(fields(rows[2])[0]);
    EXPECT_LT(first, 0.0) << "samples before the arrival are negative: " << first;
    EXPECT_LT(std::abs(third), 1e-12) << "the arrival itself is t = 0";
}

TEST(RewExport, ASilentBinFloorsRatherThanWritingInfinity) {
    const Measurement m(MeasurementId{4}, "silent", 48'000.0,
                        SpectrumData{{Complex64(0.0, 0.0)}, 1.0});
    const auto rows = data_rows(to_rew_text(m));
    ASSERT_EQ(rows.size(), 1u);
    const double level = std::stod(fields(rows[0])[1]);
    EXPECT_TRUE(std::isfinite(level) && level <= -200.0);
}

TEST(RewExport, NewlinesInANameDoNotBreakTheHeader) {
    Measurement m = spectrum();
    m.name = "two\nlines";
    const std::string text = to_rew_text(m);
    std::size_t comment_lines = 0;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        comment_lines += line.starts_with('*') ? 1 : 0;
    }
    EXPECT_NE(text.find("* Name: two lines"), std::string::npos)
        << "name should be flattened onto one line";
    EXPECT_GE(comment_lines, 4u);
}

}  // namespace
}  // namespace analyzer::model
