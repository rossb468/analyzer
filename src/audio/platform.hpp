// The backend for whatever platform this build targets.
//
// Callers that want "the audio hardware on this machine" ask here rather than
// naming a backend type. That keeps platform selection in exactly one place:
// the C ABI, and through it every client, compiles unchanged on every target,
// and a new backend is one more branch in platform.cpp rather than a hunt
// through its callers.

#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "audio/backend.hpp"

namespace analyzer::audio {

// The native backend for this build's target platform.
//
// On a platform with no backend yet this is UnavailableBackend, which
// enumerates nothing and refuses to open, so everything above it still builds,
// runs and can be tested.
std::unique_ptr<AudioBackend> default_backend();

// Stands in where no platform backend exists yet.
//
// Enumeration succeeds and finds nothing, which is the truth: a client can
// show an empty device list rather than an error. Opening throws
// NoBackendError, because pretending to capture would be worse.
class UnavailableBackend final : public AudioBackend {
public:
    std::string_view name() const noexcept override { return "none"; }
    std::vector<DeviceInfo> devices() const override { return {}; }
    std::unique_ptr<AudioStream> open(const StreamConfig& config,
                                      std::unique_ptr<AudioCallback> callback) override;
};

}  // namespace analyzer::audio
