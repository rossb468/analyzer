// Ported from crates/analyzer-model/src/filter_export.rs.

#include "model/filter_export.hpp"

#include <cmath>
#include <cstddef>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::model {
namespace {

using dsp::FilterBand;
using dsp::FilterKind;

constexpr float kRate = 48'000.0f;

std::vector<FilterBand> bands() {
    return {
        FilterBand::peaking(63.0f, -5.0f, 4.0f),
        FilterBand{FilterKind::LowShelf, 100.0f, 3.0f, 0.707f, true},
        FilterBand{FilterKind::Notch, 50.0f, 0.0f, 10.0f, true},
    };
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        out.push_back(line);
    }
    return out;
}

// The number after `key=` on the first line starting with `key`, or on the
// `occurrence`th such line.
float value_of(const std::string& text, const std::string& key, std::size_t occurrence = 0) {
    std::size_t seen = 0;
    for (std::string line : lines(text)) {
        if (!line.starts_with(key)) {
            continue;
        }
        if (seen++ != occurrence) {
            continue;
        }
        while (line.ends_with(',')) {
            line.pop_back();
        }
        return std::stof(line.substr(line.find('=') + 1));
    }
    ADD_FAILURE() << "no line starting with " << key;
    return 0.0f;
}

TEST(FilterExport, RewOutputCarriesEveryBand) {
    const std::string text = to_rew(bands(), -6.0f);
    EXPECT_TRUE(contains(text, "Equaliser: Generic")) << text;
    EXPECT_TRUE(contains(text, "Filter  1: ON  PK")) << text;
    EXPECT_TRUE(contains(text, "Filter  2: ON  LSC")) << text;
    EXPECT_TRUE(contains(text, "Filter  3: ON  NO")) << text;
    EXPECT_TRUE(contains(text, "Preamp recommended: -6.0 dB")) << text;
}

// A shelf designed by Q must not import as a shelf designed by slope.
TEST(FilterExport, ShelvesAndPassFiltersUseTheQTokens) {
    EXPECT_EQ(parametric_token(FilterKind::LowShelf), std::optional<std::string_view>("LSC"));
    EXPECT_EQ(parametric_token(FilterKind::HighShelf), std::optional<std::string_view>("HSC"));
    EXPECT_EQ(parametric_token(FilterKind::LowPass), std::optional<std::string_view>("LPQ"));
    EXPECT_EQ(parametric_token(FilterKind::HighPass), std::optional<std::string_view>("HPQ"));
}

// Gain is meaningless for a notch, and a gain field on one would be read.
TEST(FilterExport, ABandThatIgnoresGainDoesNotWriteOne) {
    const std::vector<FilterBand> notch = {
        FilterBand{FilterKind::Notch, 50.0f, 0.0f, 10.0f, true},
    };
    EXPECT_FALSE(contains(to_rew(notch, 0.0f), "Gain"));
    EXPECT_FALSE(contains(to_equalizer_apo(notch, 0.0f), "Gain"));
}

TEST(FilterExport, ApoOutputLeadsWithThePreamp) {
    const std::string text = to_equalizer_apo(bands(), -6.0f);
    EXPECT_TRUE(text.starts_with("Preamp: -6.0 dB\n")) << text;
    EXPECT_TRUE(contains(text, "Filter 1: ON PK Fc 63 Hz Gain -5 dB Q 4")) << text;
}

TEST(FilterExport, ADisabledBandKeepsItsSlot) {
    std::vector<FilterBand> disabled = bands();
    disabled[1].enabled = false;
    EXPECT_TRUE(contains(to_rew(disabled, 0.0f), "Filter  2: OFF None"));
    EXPECT_TRUE(contains(to_equalizer_apo(disabled, 0.0f), "Filter 2: OFF None"));
}

// The trap this module exists to avoid. miniDSP adds the feedback terms where
// the standard form subtracts them, so the exported signs are the negatives of
// the designed ones.
TEST(FilterExport, MinidspNegatesTheFeedbackCoefficients) {
    const FilterBand band = FilterBand::peaking(1000.0f, 6.0f, 2.0f);
    const dsp::Biquad designed = band.design(kRate);
    const std::string text = to_minidsp(std::vector{band}, 0.0f, kRate);

    EXPECT_LT(std::abs(value_of(text, "b0") - designed.b0), 1e-6f);
    EXPECT_LT(std::abs(value_of(text, "a1") + designed.a1), 1e-6f) << "a1 must be negated";
    EXPECT_LT(std::abs(value_of(text, "a2") + designed.a2), 1e-6f) << "a2 must be negated";
}

TEST(FilterExport, MinidspNumbersEverySectionAndEndsWithoutAComma) {
    const std::string text = to_minidsp(bands(), 0.0f, kRate);
    EXPECT_TRUE(contains(text, "biquad1,"));
    EXPECT_TRUE(contains(text, "biquad2,"));
    EXPECT_TRUE(contains(text, "biquad3,"));
    const std::size_t last = text.find_last_not_of(" \n\t\r");
    EXPECT_NE(text[last], ',') << text;
}

// The trim has nowhere else to go, and dropping it would export a cascade that
// clips exactly where the trim existed to stop it.
TEST(FilterExport, MinidspFoldsThePreampIntoTheFirstSectionOnly) {
    const std::vector<FilterBand> all = bands();
    const std::string plain = to_minidsp(all, 0.0f, kRate);
    const std::string trimmed = to_minidsp(all, -6.0f, kRate);

    const float expected = std::pow(10.0f, -6.0f / 20.0f);
    EXPECT_LT(std::abs(value_of(trimmed, "b0=", 0) / value_of(plain, "b0=", 0) - expected), 1e-5f);
    // The second section is untouched: the trim is applied once, not once per
    // band.
    EXPECT_LT(std::abs(value_of(trimmed, "b0=", 1) - value_of(plain, "b0=", 1)), 1e-6f);
}

TEST(FilterExport, AnEmptyEqualiserStillExportsItsTrim) {
    const std::string text = to_minidsp({}, -6.0f, kRate);
    EXPECT_TRUE(contains(text, "biquad1,")) << text;
    EXPECT_TRUE(contains(text, "b0=0.501")) << text;
}

TEST(FilterExport, EveryFormatProducesSomethingForAnEmptyEqualiser) {
    for (const FilterFormat format :
         {FilterFormat::Rew, FilterFormat::EqualizerApo, FilterFormat::MiniDsp}) {
        EXPECT_FALSE(to_text(format, {}, 0.0f, kRate).empty()) << to_string(format);
    }
}

}  // namespace
}  // namespace analyzer::model
