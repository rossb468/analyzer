// The calibration chain: raw converter samples to absolute dB SPL.
//
// This is its own module because calibration is pervasive and subtle, and every
// number a user reads depends on all of it:
//
//   dBFS --> + SPL offset --> + microphone response --> + weighting --> dB SPL
//
// Scattered across the DSP and model modules it would be subtly wrong somewhere
// and stay wrong for a long time, because the failure mode is a constant offset
// on every reading rather than anything that looks broken. Here it is one chain
// that raw levels pass through exactly once, with a golden test asserting that a
// 94 dB reference tone reads back as 94 dB.
//
// The offset can be arrived at two ways, and both are supported because both
// are used in practice:
//
// - From a reference tone. Put an acoustic calibrator on the capsule, read
//   the level, and store the difference. This is what people actually do, and
//   it absorbs every unknown in the chain at once - capsule sensitivity, preamp
//   gain, converter scaling - without needing any of them to be known.
// - From the physical chain. Compute it from microphone sensitivity, preamp
//   gain and converter full-scale voltage. Useful when no calibrator is at hand
//   and the numbers are on the datasheets, but every one of them is a chance to
//   be wrong.

#pragma once

#include <optional>
#include <span>

#include "cal/curve.hpp"
#include "cal/weighting.hpp"

namespace analyzer::cal {

// The reference level of a standard acoustic calibrator: 1 Pa.
inline constexpr float kCalibratorSplDb = 94.0f;

// Full calibration for one input channel.
//
// A value type. A default-constructed Calibration is uncalibrated: readings
// stay in dBFS and are not pretending to be SPL.
class Calibration {
public:
    Calibration() = default;

    // Derive the offset from a calibrator reading.
    //
    // `measured_dbfs` is what the analyzer showed with a calibrator producing
    // `reference_spl_db` on the capsule. Use a flat-top window to take that
    // reading: its main lobe is flat, so the level is right regardless of where
    // the calibrator's tone falls between bins.
    static Calibration from_reference_tone(float measured_dbfs, float reference_spl_db) noexcept;

    // Derive the offset from the physical signal chain.
    //
    // - sensitivity_mv_per_pa: capsule output at 1 Pa, from its datasheet.
    // - preamp_gain_db: gain between capsule and converter.
    // - full_scale_volts: the RMS voltage of a sine that reads 0 dBFS. This
    //   matches the analyzer's convention that 0 dBFS is a full-scale *sine*,
    //   not a full-scale square.
    //
    // Returns nullopt for non-positive or non-finite sensitivity or full-scale
    // voltage, which would otherwise produce an infinite offset.
    static std::optional<Calibration> from_signal_chain(float sensitivity_mv_per_pa,
                                                        float preamp_gain_db,
                                                        float full_scale_volts) noexcept;

    // A copy with a microphone response curve attached.
    [[nodiscard]] Calibration with_microphone(ResponseCurve curve) const;

    // A copy with the weighting set.
    [[nodiscard]] Calibration with_weighting(Weighting weighting) const;

    // Whether an SPL offset has been established.
    //
    // When false, output is still dBFS and a UI must label it as such rather
    // than showing a number that looks like SPL.
    //
    // Note this is "the offset is not exactly zero": a rig that genuinely
    // needs no correction reads as uncalibrated here. Callers that must tell
    // "not measured" from "measured as zero" keep an optional offset of their
    // own, as the measurement model does.
    bool is_calibrated() const noexcept { return offset_db_ != 0.0f; }

    // Decibels added to convert dBFS to dB SPL.
    float offset_db() const noexcept { return offset_db_; }

    // The weighting in force.
    Weighting weighting() const noexcept { return weighting_; }

    // The microphone curve in force.
    const ResponseCurve& microphone() const noexcept { return microphone_; }

    // Convert one bin from dBFS to dB SPL.
    float to_spl(float dbfs, float hz) const noexcept;

    // Apply the chain across a spectrum in place.
    //
    // Bin k sits at k * bin_spacing_hz. Bin 0 is DC and is left alone: it has
    // no meaningful weighting and no microphone correction. A bin spacing that
    // is not positive leaves the spectrum untouched.
    void apply(std::span<float> bins, float bin_spacing_hz) const noexcept;

    friend bool operator==(const Calibration&, const Calibration&) = default;

private:
    // Decibels added to a dBFS reading to get dB SPL.
    float offset_db_ = 0.0f;
    // Capsule correction. Flat when unknown.
    ResponseCurve microphone_;
    // Weighting applied on top.
    Weighting weighting_ = Weighting::Z;
};

}  // namespace analyzer::cal
