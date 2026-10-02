#include "cli/text.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace analyzer::cli::text {
namespace {

// Rust's `{}` is the shortest decimal that reads back, in plain positional
// notation. These are the values the harness actually prints: sample rates,
// durations and a window parameter.
TEST(Display, IsTheShortestDecimalThatReadsBack) {
    EXPECT_EQ(display(48'000.0), "48000");
    EXPECT_EQ(display(44'100.0f), "44100");
    EXPECT_EQ(display(0.1), "0.1");
    EXPECT_EQ(display(0.1f), "0.1");
    EXPECT_EQ(display(22'050.5), "22050.5");
    EXPECT_EQ(display(0.5), "0.5");
    EXPECT_EQ(display(-3.25), "-3.25");
}

// printf's %g would give 1e-07 and 1e+21; Rust never uses an exponent.
TEST(Display, NeverUsesAnExponent) {
    EXPECT_EQ(display(1e-7), "0.0000001");
    EXPECT_EQ(display(1e21), "1000000000000000000000");
}

TEST(Display, NamesTheValuesWithNoDigits) {
    EXPECT_EQ(display(std::numeric_limits<double>::quiet_NaN()), "NaN");
    EXPECT_EQ(display(std::numeric_limits<double>::infinity()), "inf");
    EXPECT_EQ(display(-std::numeric_limits<double>::infinity()), "-inf");
    EXPECT_EQ(display(-0.0), "-0");
}

// `{:?}` keeps the `.0` that `{}` drops. The harness prints a Tukey window's
// alpha this way in the header.
TEST(Debug, KeepsTheDecimalPointOnWholeNumbers) {
    EXPECT_EQ(debug(0.25f), "0.25");
    EXPECT_EQ(debug(1.0f), "1.0");
    EXPECT_EQ(debug(48'000.0), "48000.0");
}

TEST(Fixed, RoundsTheExactBinaryValueTiesToEven) {
    // -6.25 is exactly representable, so one place is a true tie.
    EXPECT_EQ(fixed(-6.25, 1), "-6.2");
    EXPECT_EQ(fixed(0.125, 2), "0.12");
    EXPECT_EQ(fixed(0.375, 2), "0.38");
    EXPECT_EQ(fixed(2.5, 0), "2");
    EXPECT_EQ(fixed(3.5, 0), "4");
}

TEST(Fixed, PrintsTheRequestedNumberOfPlaces) {
    EXPECT_EQ(fixed(1.0, 6), "1.000000");
    EXPECT_EQ(fixed(113'280.0f, 0), "113280");
    EXPECT_EQ(fixed(-0.001, 2), "-0.00");
    EXPECT_EQ(fixed(17.6f, 4), "17.6000");
}

// A float is printed as the value it holds, not as the decimal it was written
// as: 0.1f is 0.100000001490116...
TEST(Fixed, ExpandsASinglePrecisionValueExactly) {
    EXPECT_EQ(fixed(0.1f, 10), "0.1000000015");
}

TEST(Fixed, NamesTheValuesWithNoDigits) {
    EXPECT_EQ(fixed(std::numeric_limits<float>::quiet_NaN(), 2), "NaN");
    EXPECT_EQ(fixed(std::numeric_limits<float>::infinity(), 2), "inf");
}

TEST(Padding, MatchesRustAlignment) {
    EXPECT_EQ(pad_left("ab", 5), "   ab");
    EXPECT_EQ(pad_right("ab", 5), "ab   ");
    EXPECT_EQ(pad_left("abcdef", 3), "abcdef");
    EXPECT_EQ(pad_right("abcdef", 3), "abcdef");
}

TEST(ParseF64, AcceptsWhatRustAccepts) {
    EXPECT_EQ(parse_f64("1000"), 1000.0);
    EXPECT_EQ(parse_f64("996.09375"), 996.09375);
    EXPECT_EQ(parse_f64("+1.5"), 1.5);
    EXPECT_EQ(parse_f64("-.5"), -0.5);
    EXPECT_EQ(parse_f64("5."), 5.0);
    EXPECT_EQ(parse_f64("1e3"), 1000.0);
    EXPECT_EQ(parse_f64("2.5E-1"), 0.25);
    EXPECT_EQ(parse_f64("inf"), std::numeric_limits<double>::infinity());
    EXPECT_EQ(parse_f64("-Infinity"), -std::numeric_limits<double>::infinity());
    EXPECT_TRUE(std::isnan(*parse_f64("NaN")));
}

// strtod would take several of these. A flag value that Rust refuses has to be
// refused here too, or the same command line means two things.
TEST(ParseF64, RejectsWhatRustRejects) {
    for (const char* bad : {"", " ", " 1", "1 ", "1.5x", ".", "e5", "1e", "1e+", "0x10", "--1",
                            "1,5", "infinit", "nano", "+", "-"}) {
        EXPECT_FALSE(parse_f64(bad).has_value()) << '"' << bad << '"';
    }
}

TEST(ParseF32, RoundsOnceStraightToSinglePrecision) {
    EXPECT_EQ(parse_f32("0.1"), 0.1f);
    EXPECT_EQ(parse_f32("-90"), -90.0f);
    EXPECT_FALSE(parse_f32("abc").has_value());
}

TEST(ParseUsize, TakesDigitsAndAnOptionalPlus) {
    EXPECT_EQ(parse_usize("4096"), 4096u);
    EXPECT_EQ(parse_usize("+8"), 8u);
    EXPECT_EQ(parse_usize("0"), 0u);
}

TEST(ParseUsize, RejectsSignsFractionsAndOverflow) {
    for (const char* bad :
         {"", "+", "-1", "-0", "1.0", "1e3", " 1", "1 ", "abc", "99999999999999999999999"}) {
        EXPECT_FALSE(parse_usize(bad).has_value()) << '"' << bad << '"';
    }
}

}  // namespace
}  // namespace analyzer::cli::text
