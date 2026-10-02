#include "audio/device.hpp"

#include <algorithm>
#include <cmath>

namespace analyzer::audio {

bool DeviceInfo::supports_sample_rate(double rate) const noexcept {
    if (supported_sample_rates.empty()) {
        return true;
    }
    return std::ranges::any_of(supported_sample_rates,
                               [rate](double r) { return std::abs(r - rate) < 0.5; });
}

}  // namespace analyzer::audio
