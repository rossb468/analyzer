// Ported from crates/analyzer-engine/src/ring.rs.

#include "engine/ring.hpp"

#include <array>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::engine {
namespace {

TEST(CaptureRing, RoundTripsInterleavedFrames) {
    auto [sink, source] = capture_ring(2, 16);
    const std::array<float, 4> block{1.0f, -1.0f, 2.0f, -2.0f};

    EXPECT_TRUE(sink.write_interleaved(block));
    EXPECT_EQ(source.frames_available(), 2u);

    std::array<float, 4> out{};
    EXPECT_EQ(source.read_interleaved(out), 2u);
    EXPECT_EQ(out, block);
    EXPECT_EQ(source.frames_available(), 0u);
}

TEST(CaptureRing, FullRingDropsWholeBlocksAndCountsThem) {
    auto [sink, source] = capture_ring(1, 4);

    EXPECT_TRUE(sink.write_interleaved(std::array{1.0f, 2.0f, 3.0f, 4.0f}));
    EXPECT_EQ(sink.overruns(), 0u);

    // No room: the block must be refused entirely.
    EXPECT_FALSE(sink.write_interleaved(std::array{5.0f, 6.0f}));
    EXPECT_EQ(sink.overruns(), 1u);
    EXPECT_EQ(source.overruns(), 1u) << "count is visible from both ends";
}

// The property that matters: a refused write must not leave a partial block
// behind, because everything after it would be shifted by a channel.
TEST(CaptureRing, RefusedWriteLeavesNoPartialData) {
    auto [sink, source] = capture_ring(2, 2);
    EXPECT_TRUE(sink.write_interleaved(std::array{1.0f, 2.0f}));

    // Two frames wanted, one frame of room.
    EXPECT_FALSE(sink.write_interleaved(std::array{3.0f, 4.0f, 5.0f, 6.0f}));

    std::array<float, 4> out{};
    EXPECT_EQ(source.read_interleaved(out), 1u);
    EXPECT_EQ(out[0], 1.0f);
    EXPECT_EQ(out[1], 2.0f) << "only the accepted frame is present";
}

TEST(CaptureRing, PartialFrameWriteIsRefused) {
    auto [sink, source] = capture_ring(2, 16);
    // Three samples is one and a half frames.
    EXPECT_FALSE(sink.write_interleaved(std::array{1.0f, 2.0f, 3.0f}));
    EXPECT_EQ(sink.overruns(), 1u);
}

TEST(CaptureRing, EmptyWriteSucceedsWithoutCountingAnOverrun) {
    auto [sink, source] = capture_ring(2, 4);
    EXPECT_TRUE(sink.write_interleaved({}));
    EXPECT_EQ(sink.overruns(), 0u);
}

TEST(CaptureRing, ReadReturnsOnlyWholeFrames) {
    auto [sink, source] = capture_ring(3, 8);
    sink.write_interleaved(std::array{1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f});

    // Room for one frame and a bit: only one frame comes out.
    std::array<float, 5> out{};
    EXPECT_EQ(source.read_interleaved(out), 1u);
    EXPECT_EQ(out[0], 1.0f);
    EXPECT_EQ(out[1], 2.0f);
    EXPECT_EQ(out[2], 3.0f);
    EXPECT_EQ(source.frames_available(), 1u);
}

TEST(CaptureRing, ReadFromEmptyRingYieldsNothing) {
    auto [sink, source] = capture_ring(2, 4);
    std::array<float, 4> out{};
    EXPECT_EQ(source.read_interleaved(out), 0u);
}

// Exercises the wrap-around path, where a block straddles the end of the
// buffer and is copied in two pieces.
TEST(CaptureRing, SurvivesWrappingTheBufferManyTimes) {
    auto [sink, source] = capture_ring(2, 3);
    float expected = 0.0f;
    std::array<float, 4> out{};

    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(sink.write_interleaved(std::array{expected, expected + 0.5f}))
            << "should always have room";
        ASSERT_EQ(source.read_interleaved(out), 1u);
        ASSERT_EQ(out[0], expected);
        ASSERT_EQ(out[1], expected + 0.5f);
        expected += 1.0f;
    }
    EXPECT_EQ(sink.overruns(), 0u);
}

// A block that wraps must come out in order. With a hand-written ring this
// deserves its own test.
TEST(CaptureRing, BlockStraddlingTheEndComesOutInOrder) {
    auto [sink, source] = capture_ring(1, 5);
    std::array<float, 8> out{};
    ASSERT_TRUE(sink.write_interleaved(std::array{0.0f, 1.0f, 2.0f}));
    ASSERT_EQ(source.read_interleaved(out), 3u);

    // Starts at slot 3 of 5, so it wraps after two samples.
    ASSERT_TRUE(sink.write_interleaved(std::array{10.0f, 11.0f, 12.0f, 13.0f}));
    ASSERT_EQ(source.read_interleaved(out), 4u);
    EXPECT_EQ(out[0], 10.0f);
    EXPECT_EQ(out[1], 11.0f);
    EXPECT_EQ(out[2], 12.0f);
    EXPECT_EQ(out[3], 13.0f);
}

TEST(CaptureRing, FreeAndAvailableFramesTrackEachOther) {
    auto [sink, source] = capture_ring(2, 8);
    EXPECT_EQ(sink.frames_free(), 8u);
    EXPECT_EQ(source.frames_available(), 0u);

    sink.write_interleaved(std::vector<float>(6, 0.0f));
    EXPECT_EQ(sink.frames_free(), 5u);
    EXPECT_EQ(source.frames_available(), 3u);

    std::array<float, 6> out{};
    source.read_interleaved(out);
    EXPECT_EQ(source.frames_available(), 0u);
    EXPECT_EQ(sink.frames_free(), 8u);
}

TEST(Deinterleave, SplitsChannels) {
    const std::array source{1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f};
    std::array<float, 3> left{};
    std::array<float, 3> right{};
    const std::array<std::span<float>, 2> destinations{left, right};
    deinterleave(source, destinations);
    EXPECT_EQ(left, (std::array{1.0f, 2.0f, 3.0f}));
    EXPECT_EQ(right, (std::array{-1.0f, -2.0f, -3.0f}));
}

TEST(Deinterleave, StopsAtTheShorterDestination) {
    const std::array source{1.0f, -1.0f, 2.0f, -2.0f};
    std::array<float, 1> left{};
    std::array<float, 1> right{};
    const std::array<std::span<float>, 2> destinations{left, right};
    deinterleave(source, destinations);
    EXPECT_EQ(left[0], 1.0f);
    EXPECT_EQ(right[0], -1.0f);
}

TEST(DeinterleaveDeathTest, RejectsRaggedInput) {
    const std::array source{1.0f, 2.0f, 3.0f};
    std::array<float, 2> a{};
    std::array<float, 2> b{};
    const std::array<std::span<float>, 2> destinations{a, b};
    EXPECT_DEATH(deinterleave(source, destinations), "whole number of frames");
}

TEST(CaptureRingDeathTest, ZeroChannelsIsRejected) {
    EXPECT_DEATH(capture_ring(0, 16), "at least one channel");
}

TEST(CaptureRing, CrossingThreadsPreservesOrder) {
    auto [sink, source] = capture_ring(1, 1024);

    std::uint64_t producer_overruns = 0;
    std::thread producer([&sink, &producer_overruns] {
        for (int n = 0; n < 500; ++n) {
            const std::array sample{static_cast<float>(n)};
            while (!sink.write_interleaved(sample)) {
                std::this_thread::yield();
            }
        }
        producer_overruns = sink.overruns();
    });

    std::vector<float> received;
    std::array<float, 64> out{};
    while (received.size() < 500) {
        const std::size_t frames = source.read_interleaved(out);
        received.insert(received.end(), out.begin(), out.begin() + static_cast<long>(frames));
        if (frames == 0) {
            std::this_thread::yield();
        }
    }
    producer.join();

    EXPECT_EQ(producer_overruns, 0u);
    std::vector<float> expected(500);
    for (int n = 0; n < 500; ++n) {
        expected[static_cast<std::size_t>(n)] = static_cast<float>(n);
    }
    EXPECT_EQ(received, expected);
}

}  // namespace
}  // namespace analyzer::engine
