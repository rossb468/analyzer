// Not ported from Rust: the Rust got this from the language (`as` saturates).
// These pin that behaviour for the C++ replacement, on exactly the inputs a
// plain static_cast turns into undefined behaviour.

#include "base/numeric.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

namespace analyzer {
namespace {

constexpr float kFloatNan = std::numeric_limits<float>::quiet_NaN();
constexpr float kFloatInf = std::numeric_limits<float>::infinity();
constexpr double kDoubleNan = std::numeric_limits<double>::quiet_NaN();
constexpr double kDoubleInf = std::numeric_limits<double>::infinity();

TEST(SaturatingCast, InRangeValuesTruncateTowardZero) {
    EXPECT_EQ(saturating_cast<int>(3.9f), 3);
    EXPECT_EQ(saturating_cast<int>(-3.9f), -3);
    EXPECT_EQ(saturating_cast<std::size_t>(7.99f), 7u);
    EXPECT_EQ(saturating_cast<std::uint32_t>(1234.5), 1234u);
    EXPECT_EQ(saturating_cast<int>(0.0f), 0);
    EXPECT_EQ(saturating_cast<int>(-0.0f), 0);
}

TEST(SaturatingCast, NanBecomesZero) {
    EXPECT_EQ(saturating_cast<int>(kFloatNan), 0);
    EXPECT_EQ(saturating_cast<std::size_t>(kFloatNan), 0u);
    EXPECT_EQ(saturating_cast<std::uint64_t>(kDoubleNan), 0u);
    EXPECT_EQ(saturating_cast<std::int8_t>(-kFloatNan), 0);
}

TEST(SaturatingCast, UnsignedTargetsClampNegativesToZero) {
    EXPECT_EQ(saturating_cast<std::size_t>(-1.0f), 0u);
    EXPECT_EQ(saturating_cast<std::size_t>(-0.5f), 0u);
    EXPECT_EQ(saturating_cast<std::uint32_t>(-1e30f), 0u);
    EXPECT_EQ(saturating_cast<std::size_t>(-kFloatInf), 0u);
}

TEST(SaturatingCast, InfinitiesSaturateAtTheLimits) {
    EXPECT_EQ(saturating_cast<int>(kFloatInf), std::numeric_limits<int>::max());
    EXPECT_EQ(saturating_cast<int>(-kFloatInf), std::numeric_limits<int>::min());
    EXPECT_EQ(saturating_cast<std::size_t>(kFloatInf), std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(saturating_cast<std::int64_t>(-kDoubleInf), std::numeric_limits<std::int64_t>::min());
}

// The bug a naive `value >= float(max)` guard hides. INT32_MAX is not a float:
// it rounds up to 2^31, which is one past the largest int, so that exact value
// must saturate rather than be cast.
TEST(SaturatingCast, ThePowerOfTwoJustAboveMaxSaturates) {
    EXPECT_EQ(saturating_cast<std::int32_t>(2147483648.0f),
              std::numeric_limits<std::int32_t>::max());
    EXPECT_EQ(saturating_cast<std::uint32_t>(4294967296.0f),
              std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(saturating_cast<std::int64_t>(9223372036854775808.0),
              std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(saturating_cast<std::uint64_t>(18446744073709551616.0f),
              std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(saturating_cast<std::uint64_t>(18446744073709551616.0),
              std::numeric_limits<std::uint64_t>::max());
}

// The largest float below that power of two must still convert exactly, not be
// saturated early.
TEST(SaturatingCast, TheLargestRepresentableValueBelowItConvertsExactly) {
    // 2^31 - 128 is the float just under 2^31.
    EXPECT_EQ(saturating_cast<std::int32_t>(2147483520.0f), 2147483520);
    // 2^63 - 1024 is the double just under 2^63.
    EXPECT_EQ(saturating_cast<std::int64_t>(9223372036854774784.0), 9223372036854774784);
    // 2^64 - 2048 is the double just under 2^64.
    EXPECT_EQ(saturating_cast<std::uint64_t>(18446744073709549568.0), 18446744073709549568ull);
}

TEST(SaturatingCast, SignedLowerLimitIsExact) {
    EXPECT_EQ(saturating_cast<std::int32_t>(-2147483648.0f),
              std::numeric_limits<std::int32_t>::min());
    EXPECT_EQ(saturating_cast<std::int32_t>(-3e9f), std::numeric_limits<std::int32_t>::min());
    EXPECT_EQ(saturating_cast<std::int64_t>(-9223372036854775808.0),
              std::numeric_limits<std::int64_t>::min());
    // Just above the limit is an ordinary conversion.
    EXPECT_EQ(saturating_cast<std::int32_t>(-2147483520.0f), -2147483520);
}

TEST(SaturatingCast, NarrowTargetsSaturateEarly) {
    EXPECT_EQ(saturating_cast<std::uint8_t>(255.9f), 255);
    EXPECT_EQ(saturating_cast<std::uint8_t>(256.0f), 255);
    EXPECT_EQ(saturating_cast<std::int8_t>(-128.5f), -128);
    EXPECT_EQ(saturating_cast<std::int8_t>(127.5f), 127);
    EXPECT_EQ(saturating_cast<std::int16_t>(1e9), 32767);
}

TEST(SaturatingCast, IsUsableInAConstantExpression) {
    static_assert(saturating_cast<int>(2.5f) == 2);
    static_assert(saturating_cast<std::uint8_t>(1e9f) == 255);
    static_assert(saturating_cast<int>(kFloatNan) == 0);
    static_assert(saturating_cast<std::size_t>(-1.0) == 0);
}

}  // namespace
}  // namespace analyzer
