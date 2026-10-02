#include "dsp/distortion.hpp"

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdio>
#include <utility>

#include "base/numeric.hpp"
#include "base/peak.hpp"
#include "base/units.hpp"

namespace analyzer::dsp {

namespace {

std::size_t abs_diff(std::size_t a, std::size_t b) noexcept {
    return a > b ? a - b : b - a;
}

// Loudest bin within `window` of `centre`, skipping DC.
std::optional<std::size_t> peak_near(std::span<const float> power_bins, std::size_t centre,
                                     std::size_t window) noexcept {
    const std::size_t low = std::max<std::size_t>(centre > window ? centre - window : 0, 1);
    // `centre` can be the largest size_t when the caller's frequency divided by
    // its bin spacing saturated, so `centre + window` is compared by
    // subtraction rather than risking an unsigned wrap to a small number.
    const std::size_t last = power_bins.size() - 1;
    const std::size_t high = (centre >= last || window >= last - centre) ? last : centre + window;
    if (low > high) {
        return std::nullopt;
    }
    // The last of several equal maxima, so a flat-topped peak reports its top
    // edge; see base/peak.hpp.
    return low + last_max_index(power_bins.subspan(low, high - low + 1));
}

// Highest bin a lobe around `centre` reaches, clamped to the spectrum.
std::size_t lobe_high(std::size_t size, std::size_t centre, std::size_t lobe) noexcept {
    return std::min(centre + lobe, size - 1);
}

std::size_t lobe_low(std::size_t centre, std::size_t lobe) noexcept {
    return centre > lobe ? centre - lobe : 0;
}

// Total power across a peak's main lobe.
float lobe_power(std::span<const float> power_bins, std::size_t centre, std::size_t lobe) noexcept {
    float sum = 0.0f;
    for (std::size_t bin = lobe_low(centre, lobe);
         bin <= lobe_high(power_bins.size(), centre, lobe); ++bin) {
        sum += power_bins[bin];
    }
    return sum;
}

// Power-weighted centre of a lobe, in hertz.
float centroid_hz(std::span<const float> power_bins, std::size_t centre, std::size_t lobe,
                  float spacing) noexcept {
    float weight = 0.0f;
    float moment = 0.0f;
    for (std::size_t bin = lobe_low(centre, lobe);
         bin <= lobe_high(power_bins.size(), centre, lobe); ++bin) {
        const float power = power_bins[bin];
        weight += power;
        moment += power * static_cast<float>(bin);
    }
    if (weight > 0.0f) {
        return moment / weight * spacing;
    }
    return static_cast<float>(centre) * spacing;
}

void mark(std::vector<bool>& claimed, std::size_t centre, std::size_t lobe) {
    for (std::size_t bin = lobe_low(centre, lobe); bin <= lobe_high(claimed.size(), centre, lobe);
         ++bin) {
        claimed[bin] = true;
    }
}

float power_db(float power) noexcept {
    return power_to_db(power, kDistortionFloorDb);
}

float ratio_db(float amplitude_ratio) noexcept {
    return amplitude_to_db(amplitude_ratio, kDistortionFloorDb);
}

}  // namespace

std::uint32_t Distortion::highest_order() const noexcept {
    return harmonics.empty() ? 1 : harmonics.back().order;
}

std::optional<Harmonic> Distortion::harmonic(std::uint32_t order) const {
    const auto found = std::find_if(harmonics.begin(), harmonics.end(),
                                    [order](const Harmonic& h) { return h.order == order; });
    if (found == harmonics.end()) {
        return std::nullopt;
    }
    return *found;
}

std::string Distortion::thd_label() const {
    char label[64];
    if (orders_above_nyquist > 0) {
        std::snprintf(label, sizeof label, "THD %.4f%% (to H%u)", static_cast<double>(thd_percent),
                      highest_order());
    } else {
        std::snprintf(label, sizeof label, "THD %.4f%%", static_cast<double>(thd_percent));
    }
    return label;
}

std::optional<Distortion> analyse_distortion(std::span<const float> power_bins,
                                             float bin_spacing_hz,
                                             std::optional<float> fundamental_hz,
                                             const DistortionConfig& config) {
    if (power_bins.size() < 4 || bin_spacing_hz <= 0.0f) {
        return std::nullopt;
    }

    // Locate the fundamental. Bin 0 is DC and is never a tone.
    std::size_t fundamental_bin = 0;
    if (fundamental_hz) {
        const std::size_t nominal =
            saturating_cast<std::size_t>(std::round(*fundamental_hz / bin_spacing_hz));
        const auto peak = peak_near(power_bins, nominal, config.search_bins);
        if (!peak) {
            return std::nullopt;
        }
        fundamental_bin = *peak;
    } else {
        fundamental_bin = 1 + last_max_index(power_bins.subspan(1));
    }
    if (fundamental_bin == 0) {
        return std::nullopt;
    }

    const float fundamental_power = lobe_power(power_bins, fundamental_bin, config.lobe_bins);
    if (fundamental_power <= 0.0f) {
        return std::nullopt;
    }

    // Refine the frequency by the power centroid of the lobe, which recovers
    // most of the sub-bin position a bare peak index throws away.
    const float refined_hz =
        centroid_hz(power_bins, fundamental_bin, config.lobe_bins, bin_spacing_hz);

    const float nyquist = static_cast<float>(power_bins.size()) * bin_spacing_hz;
    std::vector<Harmonic> harmonics;
    float harmonic_power = 0.0f;
    std::vector<bool> claimed(power_bins.size(), false);
    mark(claimed, fundamental_bin, config.lobe_bins);
    std::uint32_t above_nyquist = 0;

    for (std::uint32_t order = 2; order <= config.max_order; ++order) {
        const float expected = refined_hz * static_cast<float>(order);
        if (expected >= nyquist) {
            // Aliases back into the band if measured, so it is not measured.
            ++above_nyquist;
            continue;
        }
        const std::size_t nominal =
            saturating_cast<std::size_t>(std::round(expected / bin_spacing_hz));
        const auto peak = peak_near(power_bins, nominal, config.search_bins);
        if (!peak) {
            continue;
        }
        const std::size_t bin = *peak;
        const float power = lobe_power(power_bins, bin, config.lobe_bins);
        mark(claimed, bin, config.lobe_bins);
        harmonic_power += power;

        const float ratio = std::sqrt(power / fundamental_power);
        harmonics.push_back(Harmonic{
            .order = order,
            .hz = centroid_hz(power_bins, bin, config.lobe_bins, bin_spacing_hz),
            .level_db = power_db(power),
            .relative_db = ratio_db(ratio),
            .percent = ratio * 100.0f,
        });
    }

    // Everything except DC and the fundamental's own lobe. Harmonics stay in,
    // because THD+N is by definition distortion *and* noise - excluding them
    // would make it smaller than THD, which is impossible.
    float residual_power = 0.0f;
    for (std::size_t bin = 1; bin < power_bins.size(); ++bin) {
        if (abs_diff(bin, fundamental_bin) > config.lobe_bins) {
            residual_power += power_bins[bin];
        }
    }

    // Noise floor as a median over the unclaimed bins, so a stray spur does not
    // drag it up the way a mean would.
    std::vector<float> unclaimed;
    unclaimed.reserve(power_bins.size());
    for (std::size_t bin = 1; bin < power_bins.size(); ++bin) {
        if (!claimed[bin]) {
            unclaimed.push_back(power_bins[bin]);
        }
    }
    float noise_floor = 0.0f;
    if (!unclaimed.empty()) {
        const auto median = unclaimed.begin() + static_cast<std::ptrdiff_t>(unclaimed.size() / 2);
        std::nth_element(unclaimed.begin(), median, unclaimed.end(),
                         [](float a, float b) { return std::is_lt(std::strong_order(a, b)); });
        noise_floor = *median;
    }

    const float thd_ratio = std::sqrt(harmonic_power / fundamental_power);
    const float thd_n_ratio = std::sqrt(residual_power / fundamental_power);

    Distortion result;
    result.fundamental_hz = refined_hz;
    result.fundamental_db = power_db(fundamental_power);
    result.harmonics = std::move(harmonics);
    result.thd_percent = thd_ratio * 100.0f;
    result.thd_db = ratio_db(thd_ratio);
    result.thd_n_percent = thd_n_ratio * 100.0f;
    result.thd_n_db = ratio_db(thd_n_ratio);
    result.noise_floor_db = power_db(noise_floor);
    result.orders_above_nyquist = above_nyquist;
    return result;
}

}  // namespace analyzer::dsp
