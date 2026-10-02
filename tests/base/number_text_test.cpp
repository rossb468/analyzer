// The number formatting and parsing that makes the text formats match the Rust
// core's. Every expected string here is what Rust's `{}` / `{:.N}` / `parse`
// produces.

#include "base/number_text.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include <gtest/gtest.h>

namespace analyzer::text {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

TEST(Shortest, PrintsTheShortestDecimalThatReadsBack) {
    EXPECT_EQ(shortest(0.1), "0.1");
    EXPECT_EQ(shortest(94.3f), "94.3");
    EXPECT_EQ(shortest(0.707f), "0.707");
    EXPECT_EQ(shortest(-5.5f), "-5.5");
    EXPECT_EQ(shortest(11.71875), "11.71875");
    EXPECT_EQ(shortest(0.0078125), "0.0078125");
    EXPECT_EQ(shortest(1378.125), "1378.125");
}

TEST(Shortest, IntegersHaveNoPointAndZeroIsZero) {
    EXPECT_EQ(shortest(100.0), "100");
    EXPECT_EQ(shortest(48000.0), "48000");
    EXPECT_EQ(shortest(20000.0f), "20000");
    EXPECT_EQ(shortest(0.0), "0");
    EXPECT_EQ(shortest(-0.0), "-0");
}

// %g would print 1e-07 and 1e+21; Rust never uses an exponent.
TEST(Shortest, NeverUsesScientificNotation) {
    EXPECT_EQ(shortest(1e-7), "0.0000001");
    EXPECT_EQ(shortest(1e-7f), "0.0000001");
    EXPECT_EQ(shortest(1e21), "1000000000000000000000");
    EXPECT_EQ(shortest(1.5e-5), "0.000015");
    EXPECT_EQ(shortest(2.5e10), "25000000000");
}

// A positional to_chars would print the exact binary value of 1e23, which is
// 99999999999999991611392. Rust prints the shortest digits and pads with zeros.
TEST(Shortest, LargeValuesPadTheShortestDigitsWithZeros) {
    EXPECT_EQ(shortest(1e23), "100000000000000000000000");
    EXPECT_EQ(shortest(1e30f), "1000000000000000000000000000000");
}

TEST(Shortest, NonFiniteValuesUseRustsSpelling) {
    EXPECT_EQ(shortest(kNaN), "NaN");
    EXPECT_EQ(shortest(kInf), "inf");
    EXPECT_EQ(shortest(-kInf), "-inf");
}

TEST(Shortest, TheSmallestAndLargestDoublesFit) {
    EXPECT_EQ(shortest(std::numeric_limits<double>::denorm_min()).size(), 2u + 323u + 1u);
    EXPECT_EQ(shortest(std::numeric_limits<double>::max()).size(), 309u);
}

TEST(Fixed, RoundsTheExactValueTiesToEven) {
    // Exact binary ties: -6.25 is representable, so it is a true tie.
    EXPECT_EQ(fixed(-6.25f, 1), "-6.2");
    EXPECT_EQ(fixed(6.75f, 1), "6.8");
    EXPECT_EQ(fixed(0.125, 2), "0.12");
    EXPECT_EQ(fixed(2.5, 0), "2");
    // 0.35 is slightly below 0.35 in binary, so it rounds down.
    EXPECT_EQ(fixed(0.35, 1), "0.3");
}

TEST(Fixed, PrintsEveryRequestedPlace) {
    EXPECT_EQ(fixed(0.0, 6), "0.000000");
    EXPECT_EQ(fixed(10.0, 6), "10.000000");
    EXPECT_EQ(fixed(-0.0, 4), "-0.0000");
    EXPECT_EQ(fixed(-0.0f, 15), "-0.000000000000000");
    EXPECT_EQ(fixed(0.5f, 15), "0.500000000000000");
}

TEST(Fixed, NonFiniteValuesIgnoreThePrecision) {
    EXPECT_EQ(fixed(kNaN, 4), "NaN");
    EXPECT_EQ(fixed(kInf, 4), "inf");
    EXPECT_EQ(fixed(-kInf, 4), "-inf");
}

TEST(Fixed, ASingleFloatPrintsItsOwnExactValue) {
    // 0.1f is 0.100000001490116119384765625.
    EXPECT_EQ(fixed(0.1f, 15), "0.100000001490116");
}

TEST(SignedFixed, ShowsAPlusOnNonNegativeValues) {
    EXPECT_EQ(signed_fixed(3.0, 4), "+3.0000");
    EXPECT_EQ(signed_fixed(-3.0, 4), "-3.0000");
    EXPECT_EQ(signed_fixed(0.0, 4), "+0.0000");
    EXPECT_EQ(signed_fixed(kNaN, 4), "NaN");
}

TEST(Pad, WidthIsAMinimum) {
    EXPECT_EQ(pad_left("63.00", 9), "    63.00");
    EXPECT_EQ(pad_left("123456.00", 5), "123456.00");
    EXPECT_EQ(pad_right("PK", 4), "PK  ");
    EXPECT_EQ(pad_right("LSC", 2), "LSC");
}

TEST(ParseF64, AcceptsWhatRustAccepts) {
    EXPECT_EQ(parse_f64("48000"), std::optional(48000.0));
    EXPECT_EQ(parse_f64("+1.5"), std::optional(1.5));
    EXPECT_EQ(parse_f64("-0.25"), std::optional(-0.25));
    EXPECT_EQ(parse_f64("1."), std::optional(1.0));
    EXPECT_EQ(parse_f64(".5"), std::optional(0.5));
    EXPECT_EQ(parse_f64("1e3"), std::optional(1000.0));
    EXPECT_EQ(parse_f64("1E-3"), std::optional(0.001));
    EXPECT_EQ(parse_f64("2.5e+2"), std::optional(250.0));
    EXPECT_EQ(parse_f64("1.e1"), std::optional(10.0));
    EXPECT_EQ(parse_f64("0.0078125"), std::optional(0.0078125));
}

TEST(ParseF64, RejectsWhatRustRejects) {
    for (const char* text : {"", "+", "-", ".", "e5", "1e", "1e+", "1.5.2", "1,5", " 1", "1 ",
                             "0x10", "1_000", "--1", "+-1", "1f", "infin", "nann", "1e5x"}) {
        EXPECT_FALSE(parse_f64(text).has_value()) << "'" << text << "'";
    }
}

TEST(ParseF64, InfinityAndNanInAnyCase) {
    EXPECT_EQ(parse_f64("inf"), std::optional(kInf));
    EXPECT_EQ(parse_f64("-Infinity"), std::optional(-kInf));
    EXPECT_EQ(parse_f64("+INF"), std::optional(kInf));
    const auto nan = parse_f64("NaN");
    ASSERT_TRUE(nan.has_value());
    EXPECT_TRUE(std::isnan(*nan));
    EXPECT_TRUE(std::isnan(*parse_f64("nan")));
}

TEST(ParseF64, OutOfRangeBecomesInfinityOrZeroAsInRust) {
    EXPECT_EQ(parse_f64("1e999"), std::optional(kInf));
    EXPECT_EQ(parse_f64("-1e999"), std::optional(-kInf));
    EXPECT_EQ(parse_f64("1e-999"), std::optional(0.0));
}

TEST(ParseF64, NegativeZeroKeepsItsSign) {
    const auto zero = parse_f64("-0");
    ASSERT_TRUE(zero.has_value());
    EXPECT_TRUE(std::signbit(*zero));
}

// Rounding a decimal to double and then to float can land one float away from
// rounding it once. The text below is a hair under the midpoint between the
// floats 1 + 2^-23 and 1 + 2^-22, so it belongs to the lower one; as a double
// it is exactly the midpoint, which then ties to the even float, the upper one.
TEST(ParseF32, RoundsOnceNotTwice) {
    const std::string text = "1.00000017881393432617187499";
    const float lower = std::nextafter(1.0f, 2.0f);
    const float upper = std::nextafter(lower, 2.0f);

    const auto direct = parse_f32(text);
    ASSERT_TRUE(direct.has_value());
    EXPECT_EQ(*direct, lower);
    EXPECT_EQ(static_cast<float>(*parse_f64(text)), upper) << "the double route is the wrong one";
}

TEST(ParseU32, AcceptsDigitsAndAPlus) {
    EXPECT_EQ(parse_u32("4096"), std::optional<std::uint32_t>(4096));
    EXPECT_EQ(parse_u32("+4096"), std::optional<std::uint32_t>(4096));
    EXPECT_EQ(parse_u32("0"), std::optional<std::uint32_t>(0));
    EXPECT_EQ(parse_u32("4294967295"), std::optional<std::uint32_t>(4294967295u));
}

TEST(ParseU32, RejectsEverythingElse) {
    for (const char* text :
         {"", "+", "-1", "-0", "4294967296", "12ab", " 12", "1.0", "1e3", "++1"}) {
        EXPECT_FALSE(parse_u32(text).has_value()) << "'" << text << "'";
    }
}

TEST(ParseUsize, AcceptsDigitsAndAPlus) {
    EXPECT_EQ(parse_usize("4096"), std::optional<std::size_t>(4096));
    EXPECT_EQ(parse_usize("+8"), std::optional<std::size_t>(8));
    EXPECT_EQ(parse_usize("0"), std::optional<std::size_t>(0));
}

TEST(ParseUsize, RejectsSignsFractionsAndOverflow) {
    for (const char* text :
         {"", "+", "-1", "-0", "1.0", "1e3", " 1", "1 ", "abc", "99999999999999999999999"}) {
        EXPECT_FALSE(parse_usize(text).has_value()) << "'" << text << "'";
    }
}

TEST(ParseBool, ExactlyTrueOrFalse) {
    EXPECT_EQ(parse_bool("true"), std::optional(true));
    EXPECT_EQ(parse_bool("false"), std::optional(false));
    for (const char* text : {"", "True", "FALSE", "maybe", "1", "0", "yes"}) {
        EXPECT_FALSE(parse_bool(text).has_value()) << "'" << text << "'";
    }
}

TEST(SaturatingCast, MatchesRustsAs) {
    EXPECT_EQ(analyzer::saturating_cast<std::uint64_t>(-1.0), 0u);
    EXPECT_EQ(analyzer::saturating_cast<std::uint64_t>(kNaN), 0u);
    EXPECT_EQ(analyzer::saturating_cast<std::uint64_t>(1e300),
              std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(analyzer::saturating_cast<std::uint64_t>(7.9), 7u);
    EXPECT_EQ(analyzer::saturating_cast<std::int64_t>(-1e300),
              std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(analyzer::saturating_cast<std::int64_t>(-7.9), -7);
    EXPECT_EQ(analyzer::saturating_cast<std::int32_t>(40000.5f), 40000);
    EXPECT_EQ(analyzer::saturating_cast<std::uint32_t>(48000.9f), 48000u);
    EXPECT_EQ(analyzer::saturating_cast<std::uint32_t>(1e20f),
              std::numeric_limits<std::uint32_t>::max());
}

TEST(ToDegrees, MultipliesByRustsConstant) {
    EXPECT_EQ(to_degrees(0.0), 0.0);
    EXPECT_LT(std::abs(to_degrees(std::acos(-1.0)) - 180.0), 1e-12);
    EXPECT_LT(std::abs(to_degrees(-std::acos(0.0)) - -90.0), 1e-12);
}

}  // namespace
}  // namespace analyzer::text
