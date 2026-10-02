#include "audio/backend.hpp"

#include <algorithm>
#include <utility>

namespace analyzer::audio {

namespace {

// The first device in `devices` whose `flag` member is set.
std::optional<DeviceInfo> find_flagged(std::vector<DeviceInfo> devices, bool DeviceInfo::*flag) {
    const auto found =
        std::ranges::find_if(devices, [flag](const DeviceInfo& device) { return device.*flag; });
    if (found == devices.end()) {
        return std::nullopt;
    }
    return std::move(*found);
}

}  // namespace

std::optional<DeviceInfo> AudioBackend::default_input() const {
    return find_flagged(devices(), &DeviceInfo::is_default_input);
}

std::optional<DeviceInfo> AudioBackend::default_output() const {
    return find_flagged(devices(), &DeviceInfo::is_default_output);
}

}  // namespace analyzer::audio
