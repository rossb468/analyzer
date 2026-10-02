#include "cli/live_display.hpp"

#include <algorithm>
#include <cmath>

#include "base/numeric.hpp"
#include "base/peak.hpp"
#include "base/units.hpp"

namespace analyzer::cli {

float broadband_db(std::span<const float> bins) {
    // write_db_fs stores 10*log10(2*power), so power is 10^(db/10)/2.
    float total = 0.0f;
    for (const float db : bins) {
        total += db_to_power(db) / 2.0f;
    }
    return total > 0.0f ? power_to_db(2.0f * total) : kSilenceDb;
}

std::pair<std::size_t, float> peak_bin(std::span<const float> bins) {
    if (bins.empty()) {
        return {0, kSilenceDb};
    }
    const std::size_t loudest = last_max_index(bins);
    return {loudest, bins[loudest]};
}

std::string bar(float db) {
    constexpr std::size_t kWidth = 40;
    const auto filled = saturating_cast<std::size_t>(
        std::round(std::clamp((db + 90.0f) / 90.0f, 0.0f, 1.0f) * static_cast<float>(kWidth)));
    std::string out;
    out.reserve(kWidth + 2);
    out += '[';
    for (std::size_t i = 0; i < kWidth; ++i) {
        out += i < filled ? '#' : '.';
    }
    out += ']';
    return out;
}

}  // namespace analyzer::cli
