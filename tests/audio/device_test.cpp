// Not ported from the Rust, which had no tests for device.rs. These pin the
// behaviour its comments promise.

#include "audio/device.hpp"

#include <functional>
#include <set>

#include <gtest/gtest.h>

namespace analyzer::audio {
namespace {

DeviceInfo device_with_rates(std::vector<double> rates) {
    return DeviceInfo{.id = DeviceId("uid"),
                      .name = "Device",
                      .input_channels = 2,
                      .output_channels = 0,
                      .default_sample_rate = 48000.0,
                      .supported_sample_rates = std::move(rates),
                      .is_default_input = false,
                      .is_default_output = false};
}

// An empty list means the backend could not tell, which must not read as "no
// rate works".
TEST(DeviceInfo, AnUnknownRateListSupportsEverything) {
    EXPECT_TRUE(device_with_rates({}).supports_sample_rate(12345.0));
}

TEST(DeviceInfo, AKnownRateListIsMatchedToWithinHalfAHertz) {
    const DeviceInfo device = device_with_rates({44100.0, 48000.0});
    EXPECT_TRUE(device.supports_sample_rate(48000.0));
    EXPECT_TRUE(device.supports_sample_rate(44100.4));
    EXPECT_FALSE(device.supports_sample_rate(96000.0));
}

TEST(DeviceInfo, DirectionsFollowChannelCounts) {
    const DeviceInfo device = device_with_rates({});
    EXPECT_TRUE(device.has_input());
    EXPECT_FALSE(device.has_output());
}

TEST(DeviceId, ComparesOrdersAndHashesByItsString) {
    EXPECT_EQ(DeviceId("a"), DeviceId("a"));
    EXPECT_NE(DeviceId("a"), DeviceId("b"));
    EXPECT_LT(DeviceId("a"), DeviceId("b"));
    EXPECT_EQ(std::hash<DeviceId>{}(DeviceId("a")), std::hash<DeviceId>{}(DeviceId("a")));
    const std::set<DeviceId> ordered{DeviceId("b"), DeviceId("a")};
    EXPECT_EQ(ordered.begin()->str(), "a");
}

}  // namespace
}  // namespace analyzer::audio
