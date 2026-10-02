// iOS backend: AVAudioSession for routing, the RemoteIO unit for audio.
//
// iOS has no HAL to enumerate. The app gets one audio session, the system
// routes it, and the app can only state preferences: which input port, what
// sample rate, what buffer duration. So a DeviceInfo here is an *input
// port* - the built-in microphone, a USB measurement microphone, a wired
// headset - and its id is the port's UID. Output always goes wherever the
// session routes it alongside that input.
//
// RemoteIO is the lowest level iOS offers, and full duplex on one clock: input
// and output share a unit, so the aggregate-device problem macOS has does not
// exist here. The render callback on the output element pulls the input with
// AudioUnitRender, hands both sides to the AudioCallback, and returns.
//
// Measurement mode
//
// The session is put in AVAudioSessionModeMeasurement. Without it iOS applies
// automatic gain control and voice processing to the microphone, and every
// measurement taken through it is a measurement of Apple's signal chain.
// Bluetooth is deliberately not allowed: hands-free input runs at 16 kHz, and
// A2DP output adds a large latency that varies from one connection to the next.
//
// Microphone permission
//
// iOS gates capture behind a user prompt that only the app can raise, and the
// app needs NSMicrophoneUsageDescription in its Info.plist. Asking is the
// client's job, before it starts a session. If permission is refused the unit
// still runs, AudioUnitRender fails, and the callback receives silence - the
// same symptom as macOS, so the caller must check for it in the same way.
//
// Interruptions
//
// A phone call or another app taking the session stops the unit without
// telling it. The client observes AVAudioSession interruption and route-change
// notifications, which are application lifecycle rather than analysis, and
// restarts the session afterwards.
//
// This header is plain C++ and names no Apple type, so the code that selects a
// backend (platform.cpp) compiles without the iOS SDK. The implementation is
// Objective-C++ (ios.mm), which CMake builds only for iOS, because
// AVAudioSession is an Objective-C API.

#pragma once

#include "audio/target.hpp"

#if ANALYZER_AUDIO_IOS

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "audio/backend.hpp"
#include "audio/device.hpp"
#include "audio/stream.hpp"

namespace analyzer::audio {

class IosStream;

// The iOS backend.
class IosBackend final : public AudioBackend {
public:
    // Create the backend. The session is configured lazily, per call.
    IosBackend() = default;

    // Open a concrete IosStream.
    //
    // Throws DeviceNotFoundError if the input port is gone,
    // ChannelOutOfRangeError for a channel the route lacks,
    // UnsupportedSampleRateError if the session will not run at the requested
    // rate, or BackendError wrapping an OSStatus or an NSError.
    std::unique_ptr<IosStream> open_stream(const StreamConfig& config,
                                           std::unique_ptr<AudioCallback> callback);

    std::string_view name() const noexcept override { return "ios"; }

    // The input ports the session can route, each with the output it would
    // play through.
    //
    // Configures and activates the session as a side effect: iOS lists inputs
    // only for a session that records, and reports a meaningful sample rate
    // only for one that is active.
    std::vector<DeviceInfo> devices() const override;

    std::unique_ptr<AudioStream> open(const StreamConfig& config,
                                      std::unique_ptr<AudioCallback> callback) override;
};

// A RemoteIO stream.
//
// Threads: the control thread opens, starts, stops and destroys it. CoreAudio
// calls the render callback on its own real-time thread, and that thread
// reaches only the RenderState owned by this object (the callback and its
// scratch buffers), never the control members. Destroying the stream stops and
// disposes of the unit first, so the audio thread is gone before any state it
// used is freed.
//
// Neither copyable nor movable: the unit holds a pointer to the stream's state
// for as long as it exists.
class IosStream final : public AudioStream {
public:
    ~IosStream() override;

    IosStream(const IosStream&) = delete;
    IosStream& operator=(const IosStream&) = delete;
    IosStream(IosStream&&) = delete;
    IosStream& operator=(IosStream&&) = delete;

    void start() override;
    void stop() override;
    bool is_running() const noexcept override { return running_; }
    const StreamConfig& config() const noexcept override { return config_; }
    StreamLatency latency() const noexcept override { return latency_; }

private:
    friend class IosBackend;
    // `inputs_available` is the number of hardware input channels the route
    // has, which sizes the planes the render callback pulls into.
    IosStream(StreamConfig config, StreamLatency latency, std::unique_ptr<AudioCallback> callback,
              std::uint32_t inputs_available);

    // Create the RemoteIO unit, configure it for `rate` and the route's channel
    // counts, install the render callback and initialise it. Separate from the
    // constructor so that a failure part way leaves a constructed stream whose
    // destructor disposes of whatever was created. Records the rate and buffer
    // size the session actually granted in config().
    void build_unit(double rate, std::uint32_t inputs_available, std::uint32_t outputs_available,
                    double io_buffer_duration_seconds);

    // The unit, the render state and the initialised flag. Defined in ios.mm,
    // where the Apple types are visible.
    struct Impl;

    StreamConfig config_;
    StreamLatency latency_;
    bool running_ = false;
    std::unique_ptr<Impl> impl_;
};

namespace detail {

// Checks exposed for the tests, which cannot see ios.mm's internals.

// Whether the render callback's input buffer list sits in memory exactly as the
// C AudioBufferList does. It is handed to CoreAudio as one, which is only sound
// while the header and the first buffer are where the C struct puts them.
bool input_list_matches_audio_buffer_list() noexcept;

// The four-character code of the RemoteIO audio unit subtype, as the SDK
// defines it.
std::uint32_t remote_io_subtype() noexcept;

}  // namespace detail

}  // namespace analyzer::audio

#endif  // ANALYZER_AUDIO_IOS
