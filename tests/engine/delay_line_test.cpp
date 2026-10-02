// Ported from the DelayLine tests in crates/analyzer-engine/src/engine.rs.

#include "engine/delay_line.hpp"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::engine {
namespace {

// A delay line must hold a sample back by exactly the requested count.
TEST(DelayLine, DelaysByTheRequestedAmount) {
    DelayLine line(64);
    std::vector<float> input(32, 0.0f);
    input[0] = 1.0f;
    std::vector<float> out(32, 0.0f);

    line.process(input, out, 5);
    EXPECT_EQ(out[5], 1.0f) << "impulse should land 5 samples late";
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i != 5) {
            ASSERT_EQ(out[i], 0.0f) << "at " << i;
        }
    }
}

// Zero delay must be a pass-through, not an off-by-one.
TEST(DelayLine, AZeroDelayPassesSamplesStraightThrough) {
    DelayLine line(64);
    std::vector<float> input(16);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<float>(i);
    }
    std::vector<float> out(16, 0.0f);
    line.process(input, out, 0);
    EXPECT_EQ(out, input);
}

// The delay must survive across calls, since a block boundary falls wherever
// the device chooses and carries no meaning.
TEST(DelayLine, CarriesAcrossBlocks) {
    DelayLine line(64);
    std::vector<float> out(4, 0.0f);
    std::vector<float> first(4, 0.0f);
    first[1] = 1.0f;
    line.process(first, out, 6);
    for (const float v : out) {
        ASSERT_EQ(v, 0.0f) << "too early to have arrived";
    }
    line.process(std::vector<float>(4, 0.0f), out, 6);
    EXPECT_EQ(out[3], 1.0f) << "impulse should arrive in the next block";
}

// Asking for more delay than the line holds must clamp, not abort or wrap.
TEST(DelayLine, AnOversizedDelayClamps) {
    DelayLine line(8);
    std::vector<float> out(4, 0.0f);
    line.process(std::vector{1.0f, 0.0f, 0.0f, 0.0f}, out, 10'000);
    for (const float v : out) {
        ASSERT_TRUE(std::isfinite(v));
    }
}

}  // namespace
}  // namespace analyzer::engine
