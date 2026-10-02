#include "dsp/octave.hpp"

#include <algorithm>
#include <cmath>

#include "base/contract.hpp"

namespace analyzer::dsp {

namespace {

// Rust's `as usize` saturates and maps NaN and negatives to zero; a plain cast
// is undefined behaviour for all three. A bin index here is a band edge divided
// by a caller-supplied bin spacing, so it can be anything.
std::size_t to_index(float value) noexcept {
    if (!(value > 0.0f)) {
        return 0;
    }
    constexpr float kLimit = 9.0e18f;
    return value >= kLimit ? static_cast<std::size_t>(kLimit) : static_cast<std::size_t>(value);
}

}  // namespace

float Band::nominal_hz() const noexcept {
    // The preferred series within one decade.
    constexpr float kPreferred[] = {1.0f, 1.25f, 1.6f, 2.0f, 2.5f, 3.15f, 4.0f, 5.0f, 6.3f, 8.0f};
    if (centre_hz <= 0.0f) {
        return centre_hz;
    }
    // Reduce to the 1..10 decade, snap, then scale back.
    const float decade = std::floor(std::log10(centre_hz));
    const float scale = std::pow(10.0f, decade);
    const float mantissa = centre_hz / scale;

    const auto best = std::min_element(
        std::begin(kPreferred), std::end(kPreferred),
        [mantissa](float a, float b) { return std::abs(a - mantissa) < std::abs(b - mantissa); });
    return *best * scale;
}

OctaveBands::OctaveBands(std::uint32_t fraction, float min_hz, float max_hz) : fraction_(fraction) {
    ANALYZER_EXPECTS(fraction > 0, "fraction must be at least 1");
    ANALYZER_EXPECTS(min_hz > 0.0f && max_hz > min_hz, "need 0 < min_hz < max_hz");

    const auto n = static_cast<float>(fraction);
    const float half_width = std::pow(kOctaveRatio, 1.0f / (2.0f * n));

    // IEC 61260 indexes bands from 1 kHz. Odd fractions centre a band on the
    // reference; even fractions straddle it, which is why the exponent differs
    // between the two.
    const bool even = fraction % 2 == 0;
    for (std::int32_t index = -600; index <= 600; ++index) {
        const float exponent = even ? (2.0f * static_cast<float>(index) + 1.0f) / (2.0f * n)
                                    : static_cast<float>(index) / n;
        const float centre = 1000.0f * std::pow(kOctaveRatio, exponent);
        // One percent of slack, because nominal and exact centres differ. The
        // band everyone calls "20 Hz" actually sits at 19.95, and a strict bound
        // would drop it from a 20 Hz..20 kHz request - while the slack stays far
        // too small to admit the next band down.
        if (centre < min_hz * 0.99f || centre > max_hz * 1.01f) {
            continue;
        }
        bands_.push_back(Band{centre, centre / half_width, centre * half_width});
    }

    std::stable_sort(bands_.begin(), bands_.end(),
                     [](const Band& a, const Band& b) { return a.centre_hz < b.centre_hz; });
}

bool OctaveBands::is_resolvable(std::size_t index, float bin_spacing_hz) const noexcept {
    return index < bands_.size() && bands_[index].width_hz() >= bin_spacing_hz;
}

std::optional<std::size_t> OctaveBands::first_resolvable(float bin_spacing_hz) const noexcept {
    for (std::size_t index = 0; index < bands_.size(); ++index) {
        if (is_resolvable(index, bin_spacing_hz)) {
            return index;
        }
    }
    return std::nullopt;
}

void OctaveBands::apply(std::span<const float> bins_db, float bin_spacing_hz,
                        std::span<float> out) const noexcept {
    ANALYZER_EXPECTS(out.size() == bands_.size(), "output must be one per band");
    // Fewer than two bins is DC alone, and DC belongs to no band. (The Rust
    // clamped the nearest-bin index to 1..=len-1 below, which panicked for a
    // lone bin; here that case has nothing to report and floors.)
    if (bins_db.size() < 2 || bin_spacing_hz <= 0.0f) {
        std::fill(out.begin(), out.end(), kBandFloorDb);
        return;
    }

    const std::size_t last_bin = bins_db.size() - 1;
    for (std::size_t i = 0; i < bands_.size(); ++i) {
        const Band& band = bands_[i];
        // Skip bin 0: DC belongs to no band. fmax rather than std::max: it
        // ignores a NaN argument, as Rust's f32::max did.
        const std::size_t first =
            to_index(std::fmax(std::ceil(band.lower_hz / bin_spacing_hz), 1.0f));
        const std::size_t last =
            std::min(to_index(std::floor(band.upper_hz / bin_spacing_hz)), last_bin);

        if (first <= last) {
            float power = 0.0f;
            for (std::size_t bin = first; bin <= last; ++bin) {
                power += std::pow(10.0f, bins_db[bin] / 10.0f);
            }
            out[i] = power > 0.0f ? 10.0f * std::log10(power) : kBandFloorDb;
        } else {
            // Narrower than the resolution: report the nearest bin rather than
            // nothing, and let is_resolvable flag it as approximate.
            const std::size_t nearest = std::clamp(
                to_index(std::round(band.centre_hz / bin_spacing_hz)), std::size_t{1}, last_bin);
            out[i] = bins_db[nearest];
        }
    }
}

}  // namespace analyzer::dsp
