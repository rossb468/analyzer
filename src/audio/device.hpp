// Device identity and capabilities.

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace analyzer::audio {

// Opaque, backend-scoped device identifier.
//
// Deliberately a string rather than an integer: CoreAudio uses numeric ids that
// are only stable within a boot, WASAPI uses endpoint id strings, and ALSA uses
// `hw:X,Y` names. A string is the only representation all three can round-trip,
// and it is what gets persisted in a saved session.
class DeviceId {
public:
    // Wrap a backend-specific identifier.
    explicit DeviceId(std::string_view id) : id_(id) {}

    // The underlying identifier.
    const std::string& str() const noexcept { return id_; }

    friend bool operator==(const DeviceId&, const DeviceId&) = default;
    friend auto operator<=>(const DeviceId&, const DeviceId&) = default;

private:
    std::string id_;
};

// What a device is and what it can do.
struct DeviceInfo {
    // Backend-scoped identifier.
    DeviceId id;
    // Name to show a user.
    std::string name;
    // Capture channels available.
    std::uint32_t input_channels = 0;
    // Playback channels available.
    std::uint32_t output_channels = 0;
    // The rate the device is currently set to.
    double default_sample_rate = 0.0;
    // Rates the device reports it can run at.
    //
    // Empty means the backend could not determine the list, not that no rate
    // works. Treat it as unknown rather than unsupported.
    std::vector<double> supported_sample_rates;
    // Whether the system considers this the default capture device.
    bool is_default_input = false;
    // Whether the system considers this the default playback device.
    bool is_default_output = false;

    // Whether this device can capture.
    bool has_input() const noexcept { return input_channels > 0; }

    // Whether this device can play back.
    bool has_output() const noexcept { return output_channels > 0; }

    // Whether `rate` is known to work.
    //
    // Returns true when the supported list is empty, since an unknown list
    // must not be read as a refusal.
    bool supports_sample_rate(double rate) const noexcept;

    friend bool operator==(const DeviceInfo&, const DeviceInfo&) = default;
};

}  // namespace analyzer::audio

// Hashable, so a client can key the map of its open streams by device.
template <>
struct std::hash<analyzer::audio::DeviceId> {
    std::size_t operator()(const analyzer::audio::DeviceId& id) const noexcept {
        return std::hash<std::string>{}(id.str());
    }
};
