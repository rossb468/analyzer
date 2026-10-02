// CoreAudio backend for macOS.
//
// Talks to the HAL directly - AudioObjectGetPropertyData for enumeration and
// AudioDeviceCreateIOProcID for capture - rather than going through an
// AudioUnit graph or a portability wrapper. For a measurement tool that is the
// right level: it addresses exact devices, reports the hardware's own latency
// figures, and adds no resampling or mixing between the converter and us.
//
// Device identity
// ---------------
// DeviceId carries the device's **UID**, not its AudioDeviceID. The numeric id
// is only stable within a boot, so persisting it in a session would silently
// reopen the wrong device tomorrow. The UID survives reboots and reconnection,
// and is resolved to a numeric id at open time.
//
// Microphone permission
// ---------------------
// macOS gates capture behind TCC. A bundled app needs NSMicrophoneUsageDescription
// in its Info.plist; a bare command-line binary inherits the permission of the
// terminal that launched it, and the first attempt prompts the user. If
// permission is refused the stream starts but delivers silence, which is why
// CoreAudioStream cannot detect it and the caller must.
//
// This header is plain C++ and names no Apple type, so the code that selects a
// backend (platform.cpp) compiles without the CoreAudio SDK. Every Apple type
// is confined to coreaudio.cpp, which CMake builds only on macOS.

#pragma once

#include "audio/target.hpp"

#if ANALYZER_AUDIO_COREAUDIO

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "audio/backend.hpp"
#include "audio/device.hpp"
#include "audio/stream.hpp"

namespace analyzer::audio {

class CoreAudioStream;

// The macOS CoreAudio backend.
class CoreAudioBackend final : public AudioBackend {
public:
    // Create the backend. Enumeration is done lazily, per call.
    CoreAudioBackend() = default;

    // Open a concrete CoreAudioStream.
    //
    // Throws DeviceNotFoundError if the UID no longer resolves,
    // ChannelOutOfRangeError for a channel the device lacks,
    // UnsupportedSampleRateError if the device will not run at the requested
    // rate, or BackendError wrapping an OSStatus.
    std::unique_ptr<CoreAudioStream> open_input(const StreamConfig& config,
                                                std::unique_ptr<AudioCallback> callback);

    std::string_view name() const noexcept override { return "coreaudio"; }
    std::vector<DeviceInfo> devices() const override;
    std::unique_ptr<AudioStream> open(const StreamConfig& config,
                                      std::unique_ptr<AudioCallback> callback) override;
};

// A CoreAudio stream: one device, one IOProc.
//
// Threads: the control thread opens, starts, stops and destroys it. CoreAudio
// calls the IOProc on its own real-time thread, and that thread reaches only
// the IoProcState owned by this object (the callback and its scratch
// buffers), never the control members. Destroying the stream stops the device
// and destroys the IOProc first, so the audio thread is gone before any state
// it used is freed.
//
// Neither copyable nor movable: CoreAudio holds a pointer to the stream's
// state for as long as the IOProc exists.
class CoreAudioStream final : public AudioStream {
public:
    ~CoreAudioStream() override;

    CoreAudioStream(const CoreAudioStream&) = delete;
    CoreAudioStream& operator=(const CoreAudioStream&) = delete;
    CoreAudioStream(CoreAudioStream&&) = delete;
    CoreAudioStream& operator=(CoreAudioStream&&) = delete;

    void start() override;
    void stop() override;
    bool is_running() const noexcept override { return running_; }
    const StreamConfig& config() const noexcept override { return config_; }
    StreamLatency latency() const noexcept override { return latency_; }

private:
    friend class CoreAudioBackend;
    // The AudioDeviceID is a std::uint32_t here so that this header need not
    // name the Apple typedef.
    CoreAudioStream(std::uint32_t device, StreamConfig granted, StreamLatency latency,
                    std::unique_ptr<AudioCallback> callback);

    // Register the IOProc. Separate from the constructor so that a failure
    // leaves a fully constructed stream whose destructor runs - a constructor
    // that threw halfway would leak a registered IOProc.
    void install_io_proc();

    // Everything the IOProc touches, and the IOProc's own id. Defined in
    // coreaudio.cpp, where the Apple types are visible.
    struct Impl;

    std::uint32_t device_;
    StreamConfig config_;
    StreamLatency latency_;
    bool running_ = false;
    std::unique_ptr<Impl> impl_;
};

namespace detail {

// Resolve a device UID to its numeric AudioDeviceID, or nullopt if no present
// device has that UID. Exposed for the tests, which check that every UID
// enumeration reports can be resolved back.
std::optional<std::uint32_t> resolve_uid(std::string_view uid);

}  // namespace detail

}  // namespace analyzer::audio

#endif  // ANALYZER_AUDIO_COREAUDIO
