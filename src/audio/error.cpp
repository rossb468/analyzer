#include "audio/error.hpp"

#include <locale>
#include <sstream>
#include <utility>

namespace analyzer::audio {

namespace {

// A rate the way the user would write it: 48000 rather than 48000.000000, and
// 44100.5 rather than 4.41005e+04. The classic locale keeps a German system
// from printing a decimal comma into an error message that may be parsed.
std::string format_hz(double hz) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(15);
    out << hz;
    return out.str();
}

}  // namespace

NoBackendError::NoBackendError() : AudioError("no audio backend is available on this platform") {}

DeviceNotFoundError::DeviceNotFoundError(std::string device)
    : AudioError("audio device not found: " + device), device_(std::move(device)) {}

UnsupportedSampleRateError::UnsupportedSampleRateError(std::string device, double requested)
    : AudioError(device + " does not support " + format_hz(requested) + " Hz"),
      device_(std::move(device)),
      requested_(requested) {}

ChannelOutOfRangeError::ChannelOutOfRangeError(std::string device, std::uint32_t channel,
                                               std::uint32_t available)
    : AudioError("channel " + std::to_string(channel) + " out of range for " + device +
                 ", which has " + std::to_string(available)),
      device_(std::move(device)),
      channel_(channel),
      available_(available) {}

NothingToDoError::NothingToDoError()
    : AudioError("stream configuration selects no input and no output channels") {}

AlreadyRunningError::AlreadyRunningError() : AudioError("stream is already running") {}

BackendError::BackendError(const std::string& detail)
    : AudioError("audio backend error: " + detail) {}

}  // namespace analyzer::audio
