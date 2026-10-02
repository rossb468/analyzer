#include "cli/live_display.hpp"

#include <algorithm>
#include <cmath>
#include <compare>

#include "base/numeric.hpp"

namespace analyzer::cli {

float broadband_db(std::span<const float> bins) {
    // write_db_fs stores 10*log10(2*power), so power is 10^(db/10)/2.
    float total = 0.0f;
    for (const float db : bins) {
        total += std::pow(10.0f, db / 10.0f) / 2.0f;
    }
    return total > 0.0f ? 10.0f * std::log10(2.0f * total) : kSilenceDb;
}

std::pair<std::size_t, float> peak_bin(std::span<const float> bins) {
    if (bins.empty()) {
        return {0, kSilenceDb};
    }
    std::size_t loudest = 0;
    for (std::size_t bin = 1; bin < bins.size(); ++bin) {
        if (std::strong_order(bins[bin], bins[loudest]) >= 0) {
            loudest = bin;
        }
    }
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
