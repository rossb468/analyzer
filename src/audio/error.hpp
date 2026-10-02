// Errors raised while enumerating devices or opening streams.
//
// These are all setup-time failures, so they are exceptions: opening a device,
// negotiating a rate and starting a stream are rare, can fail in ways a caller
// wants to describe to a user, and have nothing to return in place of the
// thing that failed. Nothing here is thrown from the real-time callback, which
// has no way to handle an error and no time to try. That path is noexcept and
// reports through values instead.
//
// One class per case a caller might handle differently, all deriving from
// AudioError, so a caller that only wants "something went wrong talking to the
// hardware" catches one type and a caller that wants to offer a fix catches the
// specific one.

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace analyzer::audio {

// Something went wrong talking to the audio hardware.
//
// The base of every error in this module. Catch it to handle all of them;
// catch a derived class to handle one.
class AudioError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// No backend is compiled in or available for this platform.
class NoBackendError final : public AudioError {
public:
    NoBackendError();
};

// The requested device is gone - unplugged, or an id from a stale list.
class DeviceNotFoundError final : public AudioError {
public:
    // `device` is whatever identified the device: a UID, or a sentence saying
    // none was selected.
    explicit DeviceNotFoundError(std::string device);

    const std::string& device() const noexcept { return device_; }

private:
    std::string device_;
};

// The device will not run at the requested rate.
class UnsupportedSampleRateError final : public AudioError {
public:
    // `device` is the human-readable device name; `requested` is the rate that
    // was asked for, in hertz.
    UnsupportedSampleRateError(std::string device, double requested);

    const std::string& device() const noexcept { return device_; }
    double requested() const noexcept { return requested_; }

private:
    std::string device_;
    double requested_;
};

// A requested channel index does not exist on the device.
class ChannelOutOfRangeError final : public AudioError {
public:
    // `device` is the human-readable device name, `channel` the index that was
    // asked for and `available` how many the device actually has.
    ChannelOutOfRangeError(std::string device, std::uint32_t channel, std::uint32_t available);

    const std::string& device() const noexcept { return device_; }
    std::uint32_t channel() const noexcept { return channel_; }
    std::uint32_t available() const noexcept { return available_; }

private:
    std::string device_;
    std::uint32_t channel_;
    std::uint32_t available_;
};

// No input and no output channel was selected, so there is nothing to do.
class NothingToDoError final : public AudioError {
public:
    NothingToDoError();
};

// start() was called on a stream that is already running.
class AlreadyRunningError final : public AudioError {
public:
    AlreadyRunningError();
};

// The platform API failed for a reason worth passing through verbatim.
class BackendError final : public AudioError {
public:
    explicit BackendError(const std::string& detail);
};

}  // namespace analyzer::audio
