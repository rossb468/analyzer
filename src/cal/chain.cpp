#include "cal/chain.hpp"

#include <cmath>
#include <utility>

#include "base/units.hpp"

namespace analyzer::cal {

Calibration Calibration::from_reference_tone(float measured_dbfs, float reference_spl_db) noexcept {
    Calibration cal;
    cal.offset_db_ = reference_spl_db - measured_dbfs;
    return cal;
}

std::optional<Calibration> Calibration::from_signal_chain(float sensitivity_mv_per_pa,
                                                          float preamp_gain_db,
                                                          float full_scale_volts) noexcept {
    // Positive form, and finiteness checked explicitly: a NaN from a
    // mis-parsed datasheet field would otherwise sail through a bare
    // comparison and produce an infinite offset.
    const bool usable = std::isfinite(sensitivity_mv_per_pa) && std::isfinite(full_scale_volts) &&
                        sensitivity_mv_per_pa > 0.0f && full_scale_volts > 0.0f;
    if (!usable) {
        return std::nullopt;
    }
    // Volts at the converter when the capsule sees 1 Pa, i.e. 94 dB SPL.
    const float volts_at_one_pascal =
        (sensitivity_mv_per_pa / 1000.0f) * db_to_amplitude(preamp_gain_db);
    const float dbfs_at_one_pascal = amplitude_to_db(volts_at_one_pascal / full_scale_volts);
    Calibration cal;
    cal.offset_db_ = kCalibratorSplDb - dbfs_at_one_pascal;
    return cal;
}

Calibration Calibration::with_microphone(ResponseCurve curve) const {
    Calibration cal = *this;
    cal.microphone_ = std::move(curve);
    return cal;
}

Calibration Calibration::with_weighting(Weighting weighting) const {
    Calibration cal = *this;
    cal.weighting_ = weighting;
    return cal;
}

float Calibration::to_spl(float dbfs, float hz) const noexcept {
    return dbfs + offset_db_ + microphone_.db_at(hz) + db_at(weighting_, hz);
}

void Calibration::apply(std::span<float> bins, float bin_spacing_hz) const noexcept {
    if (bin_spacing_hz <= 0.0f) {
        return;
    }
    for (std::size_t index = 1; index < bins.size(); ++index) {
        bins[index] = to_spl(bins[index], static_cast<float>(index) * bin_spacing_hz);
    }
}

}  // namespace analyzer::cal
