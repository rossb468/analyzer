// The backend abstraction.
//
// This class is the project's own, deliberately not a portability library's.
// Device control, exclusive access and high channel counts are exactly where
// general-purpose audio wrappers are weakest, and a measurement tool lives on
// device control. Each platform gets a direct implementation: CoreAudio and
// RemoteIO now, WASAPI and ALSA/PipeWire when the other clients happen.
//
// Few real implementations exist today. That is the point - an interface with a
// single implementation looks like overhead and is the cheapest insurance
// against the deferred port turning into a rewrite.

#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "audio/device.hpp"
#include "audio/stream.hpp"

namespace analyzer::audio {

// A source of audio devices and streams.
//
// Threads: used by one thread at a time. A backend holds no state the audio
// thread reaches; that belongs to the streams it opens.
class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    // Name of this backend, for display and logs. Valid for the backend's
    // lifetime.
    virtual std::string_view name() const noexcept = 0;

    // Enumerate currently present devices.
    //
    // The result is a snapshot. Devices come and go, so ids from an old call may
    // no longer resolve.
    //
    // Throws BackendError if the platform refuses to enumerate.
    virtual std::vector<DeviceInfo> devices() const = 0;

    // The system default capture device, if there is one.
    //
    // Throws BackendError if the platform refuses to report it.
    virtual std::optional<DeviceInfo> default_input() const;

    // The system default playback device, if there is one.
    //
    // Throws BackendError if the platform refuses to report it.
    virtual std::optional<DeviceInfo> default_output() const;

    // Open a stream. The returned stream is stopped; call AudioStream::start()
    // to begin.
    //
    // Opening separately from starting matters: allocation and device
    // negotiation happen here, so that starting is cheap and nothing on the
    // real-time path has to allocate.
    //
    // The stream takes ownership of `callback`. If open throws, the callback is
    // destroyed with the failed attempt.
    //
    // Throws DeviceNotFoundError, UnsupportedSampleRateError,
    // ChannelOutOfRangeError or NothingToDoError for a configuration the
    // hardware cannot satisfy, and BackendError for a platform failure.
    virtual std::unique_ptr<AudioStream> open(const StreamConfig& config,
                                              std::unique_ptr<AudioCallback> callback) = 0;

protected:
    AudioBackend() = default;
    AudioBackend(const AudioBackend&) = default;
    AudioBackend(AudioBackend&&) = default;
    AudioBackend& operator=(const AudioBackend&) = default;
    AudioBackend& operator=(AudioBackend&&) = default;
};

}  // namespace analyzer::audio
