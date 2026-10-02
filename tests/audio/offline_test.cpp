// Ported from crates/analyzer-audio/src/offline.rs.

#include "audio/offline.hpp"

#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

#include "audio/error.hpp"

namespace analyzer::audio {
namespace {

Source ramp(std::size_t frames, std::size_t channels) {
    // Frame n, channel c holds n + c/10, so both axes are identifiable.
    std::vector<float> samples;
    for (std::size_t n = 0; n < frames; ++n) {
        for (std::size_t c = 0; c < channels; ++c) {
            samples.push_back(static_cast<float>(n) + static_cast<float>(c) / 10.0f);
        }
    }
    return Source(std::move(samples), channels, 48000.0);
}

StreamConfig config(std::vector<std::uint32_t> inputs, std::vector<std::uint32_t> outputs) {
    StreamConfig result;
    result.input = DeviceId(kOfflineDeviceId);
    result.output = DeviceId(kOfflineDeviceId);
    result.sample_rate = 48000.0;
    result.buffer_frames = 4;
    result.input_channels = std::move(inputs);
    result.output_channels = std::move(outputs);
    return result;
}

TEST(Source, ReportsFrames) {
    EXPECT_EQ(ramp(10, 2).frames(), 10u);
    EXPECT_EQ(Source::mono(std::vector<float>(7, 0.0f), 48000.0).frames(), 7u);
}

TEST(SourceDeathTest, RaggedSourceIsRejected) {
    EXPECT_DEATH(Source(std::vector<float>(5, 0.0f), 2, 48000.0), "whole number of frames");
}

TEST(OfflineBackend, EnumeratesOneDevice) {
    const OfflineBackend backend(ramp(4, 2), 2);
    const auto devices = backend.devices();
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_TRUE(devices[0].has_input() && devices[0].has_output());
    EXPECT_TRUE(backend.default_input().has_value());
    EXPECT_TRUE(backend.default_output().has_value());
}

TEST(OfflineBackend, RejectsOutOfRangeInputChannel) {
    OfflineBackend backend(ramp(4, 2), 2);
    try {
        backend.open(config({7}, {}), make_callback([](AudioBuffers&) {}));
        FAIL() << "expected a ChannelOutOfRangeError";
    } catch (const ChannelOutOfRangeError& error) {
        EXPECT_EQ(error.channel(), 7u);
    }
}

TEST(OfflineStream, DeliversSelectedChannelsInRequestedOrder) {
    OfflineBackend backend(ramp(3, 2), 3);
    auto stream = backend.open_offline(
        // Reversed on purpose: order must follow the request, not the device.
        config({1, 0}, {}), make_callback([](AudioBuffers& buffers) {
            // ramp() makes channel 1 larger than channel 0 in every
            // frame, so slot 0 holding the larger value proves the
            // requested order was honoured rather than the device order.
            const auto first = buffers.input_channel(0);
            const auto second = buffers.input_channel(1);
            const float slot0 = first.empty() ? 0.0f : first[0];
            const float slot1 = second.empty() ? 0.0f : second[0];
            EXPECT_GT(slot0, slot1)
                << "slot 0 should carry device channel 1: " << slot0 << " vs " << slot1;
        }));

    EXPECT_NO_THROW(stream.start());
    // 3 frames of material, block of 3.
    EXPECT_EQ(stream.run_to_end(), 3u);
}

TEST(OfflineStream, BlocksSplitTheSourceAndStopAtTheEnd) {
    OfflineBackend backend(ramp(10, 1), 4);
    std::size_t blocks = 0;
    auto stream = backend.open_offline(config({0}, {}), make_callback([](AudioBuffers&) {}));

    while (true) {
        const std::size_t frames = stream.pump();
        if (frames == 0) {
            break;
        }
        ++blocks;
    }
    // 4 + 4 + 2
    EXPECT_EQ(blocks, 3u);
    EXPECT_EQ(stream.position(), 10u);
    EXPECT_EQ(stream.pump(), 0u) << "exhausted stream stays exhausted";
}

TEST(OfflineStream, CapturesWhatTheCallbackWrites) {
    OfflineBackend backend(ramp(4, 1), 2);
    auto stream =
        backend.open_offline(config({0}, {0, 1}), make_callback([](AudioBuffers& buffers) {
                                 for (float& sample : buffers.output()) {
                                     sample = 0.25f;
                                 }
                             }));

    stream.run_to_end();
    // 4 frames x 2 output channels.
    ASSERT_EQ(stream.captured_output().size(), 8u);
    for (const float sample : stream.captured_output()) {
        EXPECT_EQ(sample, 0.25f);
    }
}

TEST(OfflineStream, OutputBufferIsClearedBetweenBlocks) {
    OfflineBackend backend(ramp(4, 1), 2);
    auto stream = backend.open_offline(config({0}, {0}),
                                       make_callback([first = true](AudioBuffers& buffers) mutable {
                                           if (first) {
                                               for (float& sample : buffers.output()) {
                                                   sample = 1.0f;
                                               }
                                               first = false;
                                           } else {
                                               // Second block must not inherit the first block's
                                               // data.
                                               for (const float sample : buffers.output()) {
                                                   EXPECT_EQ(sample, 0.0f);
                                               }
                                           }
                                       }));
    EXPECT_EQ(stream.run_to_end(), 4u);
}

TEST(OfflineStream, RewindReplaysFromTheStart) {
    OfflineBackend backend(ramp(4, 1), 4);
    auto stream = backend.open_offline(config({0}, {0}), make_callback([](AudioBuffers& buffers) {
                                           for (float& sample : buffers.output()) {
                                               sample = 1.0f;
                                           }
                                       }));

    stream.run_to_end();
    EXPECT_EQ(stream.captured_output().size(), 4u);

    stream.rewind();
    EXPECT_EQ(stream.position(), 0u);
    EXPECT_TRUE(stream.captured_output().empty());
    EXPECT_EQ(stream.run_to_end(), 4u);
}

TEST(OfflineStream, DoubleStartIsAnErrorAndStopIsIdempotent) {
    OfflineBackend backend(ramp(2, 1), 2);
    auto stream = backend.open_offline(config({0}, {}), make_callback([](AudioBuffers&) {}));

    EXPECT_NO_THROW(stream.start());
    EXPECT_THROW(stream.start(), AlreadyRunningError);
    EXPECT_NO_THROW(stream.stop());
    EXPECT_NO_THROW(stream.stop());
    EXPECT_FALSE(stream.is_running());
}

}  // namespace
}  // namespace analyzer::audio
