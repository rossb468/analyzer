// Ported from crates/analyzer-audio/src/stream.rs.

#include "audio/stream.hpp"

#include <array>
#include <vector>

#include <gtest/gtest.h>

#include "audio/error.hpp"
#include "engine/rt.hpp"

namespace analyzer::audio {
namespace {

TEST(StreamConfig, ValidateRejectsEmptyChannelSets) {
    StreamConfig config;
    config.sample_rate = 48000.0;
    EXPECT_THROW(config.validate(), NothingToDoError);
}

TEST(StreamConfig, ValidateRejectsNonPositiveRate) {
    StreamConfig config;
    config.sample_rate = 0.0;
    config.input_channels = {0};
    EXPECT_THROW(config.validate(), UnsupportedSampleRateError);
}

TEST(StreamConfig, ValidateAcceptsInputOnly) {
    StreamConfig config;
    config.sample_rate = 48000.0;
    config.buffer_frames = 128;
    config.input_channels = {0, 1};
    EXPECT_NO_THROW(config.validate());
}

TEST(StreamLatency, RoundTripLatencySumsAllThreeParts) {
    const StreamLatency latency{
        .input_frames = 100, .output_frames = 200, .safety_offset_frames = 33};
    EXPECT_EQ(latency.round_trip_frames(), 333u);
    const double seconds = latency.round_trip_seconds(48000.0);
    EXPECT_NEAR(seconds, 333.0 / 48000.0, 1e-12);
    // A zero rate must not divide by zero.
    EXPECT_EQ(latency.round_trip_seconds(0.0), 0.0);
}

TEST(AudioBuffers, InputChannelDeinterleaves) {
    // Two channels, three frames: L0 R0 L1 R1 L2 R2
    const std::array<float, 6> input{1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f};
    std::array<float, 3> output{};
    const AudioBuffers buffers(input, output, 2, 1, 3);

    const auto left = buffers.input_channel(0);
    const auto right = buffers.input_channel(1);
    EXPECT_EQ(std::vector<float>(left.begin(), left.end()), (std::vector<float>{1.0f, 2.0f, 3.0f}));
    EXPECT_EQ(std::vector<float>(right.begin(), right.end()),
              (std::vector<float>{-1.0f, -2.0f, -3.0f}));
}

TEST(AudioBuffers, OutOfRangeChannelYieldsNothingRatherThanAborting) {
    const std::array<float, 2> input{1.0f, 2.0f};
    std::array<float, 2> output{};
    const AudioBuffers buffers(input, output, 1, 1, 2);
    const auto missing = buffers.input_channel(5);
    EXPECT_EQ(std::distance(missing.begin(), missing.end()), 0);
    EXPECT_TRUE(missing.empty());
}

TEST(AudioBuffers, SilenceOutputZeroesEverything) {
    std::array<float, 4> output{0.5f, -0.5f, 0.25f, -0.25f};
    AudioBuffers buffers({}, output, 0, 2, 2);
    buffers.silence_output();
    for (const float sample : buffers.output()) {
        EXPECT_EQ(sample, 0.0f);
    }
}

TEST(AudioBuffersDeathTest, MismatchedInputLengthAborts) {
    const std::array<float, 3> input{1.0f, 2.0f, 3.0f};
    std::array<float, 2> output{};
    EXPECT_DEATH(AudioBuffers(input, output, 2, 1, 2), "input buffer must be");
}

TEST(AudioCallback, ClosuresAreCallbacks) {
    const auto takes_callback = [](AudioCallback& callback) {
        std::array<float, 2> output{1.0f, 1.0f};
        AudioBuffers buffers({}, output, 0, 1, 2);
        callback.process(buffers);
        for (const float sample : output) {
            EXPECT_EQ(sample, 0.0f);
        }
    };
    const auto callback = make_callback([](AudioBuffers& buffers) { buffers.silence_output(); });
    takes_callback(*callback);
}

// The wrapper is an object allocated at setup, so the guarantee that
// invoking it is allocation-free has to be proved rather than assumed. The test
// binary links the allocation trap, which aborts inside an rt_section on any
// allocation - so reaching the assertions at all is the result.
TEST(AudioCallbackRealTime, InvokingAWrappedCallbackDoesNotAllocate) {
    ASSERT_TRUE(engine::alloc_trap_installed());

    // Everything the audio thread would use is built before the section, as a
    // backend does at open.
    std::size_t calls = 0;
    auto callback = make_callback([&calls](AudioBuffers& buffers) {
        const ChannelView left = buffers.input_channel(0);
        float sum = 0.0f;
        for (const float sample : left) {
            sum += sample;
        }
        buffers.silence_output();
        buffers.output()[0] = sum;
        ++calls;
    });
    const std::array<float, 6> input{1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f};
    std::array<float, 3> output{9.0f, 9.0f, 9.0f};

    engine::rt_section([&] {
        AudioBuffers buffers(input, output, 2, 1, 3);
        callback->process(buffers);
    });

    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(output[0], 6.0f);
    EXPECT_EQ(output[1], 0.0f);
}

}  // namespace
}  // namespace analyzer::audio
