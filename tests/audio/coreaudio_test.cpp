// Ported from crates/analyzer-audio/src/coreaudio.rs.
//
// These talk to the real HAL, so they only exist on macOS. Everywhere else the
// file is empty after the preprocessor, which keeps the test glob in CMake
// platform-agnostic.

#include "audio/target.hpp"

#if ANALYZER_AUDIO_COREAUDIO

#include "audio/coreaudio.hpp"

#include <string>

#include <gtest/gtest.h>

#include "audio/error.hpp"

namespace analyzer::audio {
namespace {

bool mentions_aggregate(const std::string& message) {
    return message.find("aggregate") != std::string::npos;
}

// Enumeration must not crash and must find something. Every Mac has at
// least a built-in output, so an empty list means the property calls are
// broken rather than that the machine is unusual.
TEST(CoreAudioBackend, EnumeratesDevices) {
    const CoreAudioBackend backend;
    const auto devices = backend.devices();
    ASSERT_FALSE(devices.empty()) << "no CoreAudio devices found at all";

    for (const auto& device : devices) {
        EXPECT_FALSE(device.id.str().empty()) << "device UID must not be empty";
        EXPECT_FALSE(device.name.empty()) << "device name must not be empty";
    }
}

TEST(CoreAudioBackend, ADefaultInputOrOutputExists) {
    const CoreAudioBackend backend;
    const bool has_default =
        backend.default_input().has_value() || backend.default_output().has_value();
    EXPECT_TRUE(has_default) << "expected at least one default device";
}

TEST(CoreAudioBackend, DeviceUidsResolveBackToNumericIds) {
    const CoreAudioBackend backend;
    for (const auto& device : backend.devices()) {
        EXPECT_TRUE(detail::resolve_uid(device.id.str()).has_value())
            << "UID " << device.id.str() << " did not resolve back";
    }
}

TEST(CoreAudioBackend, UnknownUidIsReportedNotAborted) {
    CoreAudioBackend backend;
    StreamConfig config;
    config.input = DeviceId("no-such-device-uid");
    config.sample_rate = 48000.0;
    config.buffer_frames = 512;
    config.input_channels = {0};
    EXPECT_THROW(backend.open_input(config, make_callback([](AudioBuffers&) {})),
                 DeviceNotFoundError);
}

// Two devices means two IOProcs on two clocks, and a transfer function
// measured across unsynchronised clocks drifts in phase until it is
// meaningless. The error has to point at aggregate devices, which is the
// actual fix.
TEST(CoreAudioBackend, SplitInputAndOutputDevicesAreRefusedWithAWayForward) {
    CoreAudioBackend backend;
    StreamConfig config;
    config.input = DeviceId("device-a");
    config.output = DeviceId("device-b");
    config.sample_rate = 48000.0;
    config.buffer_frames = 512;
    config.input_channels = {0};
    config.output_channels = {0};
    try {
        backend.open_input(config, make_callback([](AudioBuffers&) {}));
        FAIL() << "expected a backend error";
    } catch (const BackendError& error) {
        EXPECT_TRUE(mentions_aggregate(error.what())) << "unhelpful error: " << error.what();
    }
}

TEST(CoreAudioBackend, TheSameDeviceForBothDirectionsIsAccepted) {
    CoreAudioBackend backend;
    const auto device = backend.default_output();
    if (!device) {
        return;
    }
    StreamConfig config;
    config.input = device->id;
    config.output = device->id;
    config.sample_rate = device->default_sample_rate;
    config.buffer_frames = 512;
    config.output_channels = {0};
    // Opening can still fail on hardware or permissions, but never with the
    // split-device complaint.
    try {
        backend.open_input(config, make_callback([](AudioBuffers&) {}));
    } catch (const BackendError& error) {
        EXPECT_FALSE(mentions_aggregate(error.what()))
            << "one device treated as split: " << error.what();
    } catch (const AudioError&) {
        // Any other refusal is the hardware's business, not this test's.
    }
}

}  // namespace
}  // namespace analyzer::audio

#endif  // ANALYZER_AUDIO_COREAUDIO
