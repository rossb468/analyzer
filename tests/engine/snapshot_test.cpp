// Ported from crates/analyzer-engine/src/snapshot.rs.

#include "engine/snapshot.hpp"

#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::engine {
namespace {

struct Frame {
    std::uint64_t sequence = 0;
    std::vector<float> bins;
};

TEST(Snapshot, ReaderSeesTheInitialValueBeforeAnythingIsPublished) {
    auto [publisher, reader] = snapshot_channel(Frame{7, {1.0f, 2.0f}});
    EXPECT_EQ(reader.read().sequence, 7u);
}

TEST(Snapshot, PublishedValuesBecomeVisible) {
    auto [publisher, reader] = snapshot_channel(Frame{});

    publisher.publish_with([](Frame& frame) {
        frame.sequence = 1;
        frame.bins.assign({0.5f, 0.25f});
    });

    EXPECT_TRUE(reader.has_update());
    const Frame& frame = reader.read();
    EXPECT_EQ(frame.sequence, 1u);
    EXPECT_EQ(frame.bins, (std::vector{0.5f, 0.25f}));
}

TEST(Snapshot, HasUpdateIsFalseUntilSomethingIsPublished) {
    auto [publisher, reader] = snapshot_channel(Frame{});
    EXPECT_FALSE(reader.has_update());

    publisher.publish_with([](Frame& frame) { frame.sequence = 1; });
    EXPECT_TRUE(reader.has_update());

    reader.read();
    EXPECT_FALSE(reader.has_update()) << "consumed update should clear";
}

// The rate-mismatch behaviour: a fast producer must not queue up work for a
// slow consumer, it must overwrite.
TEST(Snapshot, ASlowReaderSeesOnlyTheNewestValue) {
    auto [publisher, reader] = snapshot_channel(Frame{});
    for (std::uint64_t sequence = 1; sequence <= 100; ++sequence) {
        publisher.publish_with([sequence](Frame& frame) { frame.sequence = sequence; });
    }
    EXPECT_EQ(reader.read().sequence, 100u);
}

TEST(Snapshot, RepeatedReadsWithoutAPublishAreStable) {
    auto [publisher, reader] = snapshot_channel(Frame{});
    publisher.publish_with([](Frame& frame) { frame.sequence = 42; });

    EXPECT_EQ(reader.read().sequence, 42u);
    EXPECT_EQ(reader.read().sequence, 42u);
    EXPECT_EQ(reader.read().sequence, 42u);
}

// Documents the buffer-recycling surprise: the pending buffer holds an older
// value, not the last published one. Code that assumes otherwise reads stale
// fields, so publish_with must overwrite everything it cares about.
TEST(Snapshot, PendingBufferIsRecycledNotFreshlyCopied) {
    auto [publisher, reader] = snapshot_channel(Frame{});

    publisher.publish_with([](Frame& frame) { frame.sequence = 1; });
    publisher.publish_with([](Frame& frame) { frame.sequence = 2; });
    publisher.publish_with([](Frame& frame) {
        // Whatever is here is a recycled buffer, and specifically not 2.
        EXPECT_NE(frame.sequence, 2u) << "must not be the value just published";
        frame.sequence = 3;
    });

    EXPECT_EQ(reader.read().sequence, 3u);
}

TEST(Snapshot, CrossesThreads) {
    auto [publisher, reader] = snapshot_channel(Frame{0, std::vector<float>(1, 0.0f)});

    std::thread writer([&publisher] {
        for (std::uint64_t sequence = 1; sequence <= 1000; ++sequence) {
            publisher.publish_with([sequence](Frame& frame) {
                frame.sequence = sequence;
                frame.bins[0] = static_cast<float>(sequence);
            });
        }
    });

    std::uint64_t last = 0;
    std::uint64_t observations = 0;
    while (last < 1000) {
        const Frame& frame = reader.read();
        // Sequence must never go backwards, and the payload must always match
        // the sequence - that is what proves a torn read never happens.
        ASSERT_GE(frame.sequence, last) << "sequence went backwards";
        ASSERT_EQ(frame.bins[0], static_cast<float>(frame.sequence)) << "torn snapshot observed";
        last = frame.sequence;
        ASSERT_LT(++observations, 10'000'000u) << "reader never saw the final value";
    }

    writer.join();
}

}  // namespace
}  // namespace analyzer::engine
