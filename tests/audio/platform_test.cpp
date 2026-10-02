// Ported from crates/analyzer-audio/src/platform.rs.

#include "audio/platform.hpp"

#include <gtest/gtest.h>

#include "audio/error.hpp"

namespace analyzer::audio {
namespace {

TEST(UnavailableBackend, ListsNothingAndRefusesToOpen) {
    UnavailableBackend backend;
    EXPECT_TRUE(backend.devices().empty());
    EXPECT_FALSE(backend.default_input().has_value());
    StreamConfig config;
    config.sample_rate = 48000.0;
    config.buffer_frames = 512;
    config.input_channels = {0};
    EXPECT_THROW(backend.open(config, make_callback([](AudioBuffers&) {})), NoBackendError);
}

TEST(DefaultBackend, ExistsOnEveryPlatform) {
    const auto backend = default_backend();
    ASSERT_NE(backend, nullptr);
    EXPECT_FALSE(backend->name().empty());
}

}  // namespace
}  // namespace analyzer::audio
