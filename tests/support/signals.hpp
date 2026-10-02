// Test signal helpers shared across modules.
//
// Keep these boring and obviously correct: a bug here would make every test
// that uses it agree with itself.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::test {

inline constexpr float kTau = 2.0f * std::numbers::pi_v<float>;

// `count` samples of amplitude * sin(2*pi*cycles*n / period), starting at
// sample `offset`.
//
// Prefer a continuous stream - advance `offset` between blocks - over
// re-sending one block. A repeated block is periodic, and that periodicity is
// never what a test is measuring (see "Feed tests a continuous stream" in
// CLAUDE.md).
inline std::vector<float> sine(std::size_t count, float cycles_per_period, std::size_t period,
                               float amplitude = 1.0f, std::size_t offset = 0) {
    std::vector<float> out(count);
    for (std::size_t n = 0; n < count; ++n) {
        const auto t = static_cast<float>(n + offset) / static_cast<float>(period);
        out[n] = amplitude * std::sin(kTau * cycles_per_period * t);
    }
    return out;
}

// `count` frequencies spaced evenly on a log axis from `from` to `to`, both
// included: the shape of the plot's own column frequencies, and of what the
// optimiser and target curves are given. `count` must be at least 2.
inline std::vector<float> log_spaced(float from, float to, std::size_t count) {
    std::vector<float> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count - 1);
        out[i] = from * std::pow(to / from, t);
    }
    return out;
}

// Index of the largest value, the first of any equals.
inline std::size_t peak_bin(std::span<const float> values) {
    return static_cast<std::size_t>(std::max_element(values.begin(), values.end()) -
                                    values.begin());
}

// Assert two equal-length spans agree element by element within `tolerance`,
// naming the first index that does not.
inline void expect_all_near(std::span<const float> actual, std::span<const float> expected,
                            float tolerance) {
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        ASSERT_NEAR(actual[i], expected[i], tolerance) << "at index " << i;
    }
}

}  // namespace analyzer::test
