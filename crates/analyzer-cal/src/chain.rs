//! The calibration chain: converter samples to absolute sound pressure level.
//!
//! Everything a user reads depends on this, and it is pervasive enough that
//! scattering it across the DSP and model crates would guarantee it is subtly
//! wrong somewhere. So it lives in one place and raw levels pass through it
//! exactly once.
//!
//! ```text
//! dBFS ──▶ + SPL offset ──▶ + microphone response ──▶ + weighting ──▶ dB SPL
//! ```
//!
//! The offset can be arrived at two ways, and both are supported because both
//! are used in practice:
//!
//! - **From a reference tone.** Put an acoustic calibrator on the capsule, read
//!   the level, and store the difference. This is what people actually do, and
//!   it absorbs every unknown in the chain at once — capsule sensitivity, preamp
//!   gain, converter scaling — without needing any of them to be known.
//! - **From the physical chain.** Compute it from microphone sensitivity, preamp
//!   gain and converter full-scale voltage. Useful when no calibrator is at hand
//!   and the numbers are on the datasheets, but every one of them is a chance to
//!   be wrong.

use crate::curve::ResponseCurve;
use crate::weighting::Weighting;

/// The reference level of a standard acoustic calibrator: 1 Pa.
pub const CALIBRATOR_SPL_DB: f32 = 94.0;

/// Full calibration for one input channel.
#[derive(Debug, Clone, PartialEq)]
pub struct Calibration {
    /// Decibels added to a dBFS reading to get dB SPL.
    offset_db: f32,
    /// Capsule correction. Flat when unknown.
    microphone: ResponseCurve,
    /// Weighting applied on top.
    weighting: Weighting,
}

impl Default for Calibration {
    /// Uncalibrated: readings stay in dBFS and are not pretending to be SPL.
    fn default() -> Self {
        Self {
            offset_db: 0.0,
            microphone: ResponseCurve::flat(),
            weighting: Weighting::Z,
        }
    }
}

impl Calibration {
    /// Derive the offset from a calibrator reading.
    ///
    /// `measured_dbfs` is what the analyzer showed with a calibrator producing
    /// `reference_spl_db` on the capsule. Use a flat-top window to take that
    /// reading: its main lobe is flat, so the level is right regardless of where
    /// the calibrator's tone falls between bins.
    pub fn from_reference_tone(measured_dbfs: f32, reference_spl_db: f32) -> Self {
        Self {
            offset_db: reference_spl_db - measured_dbfs,
            ..Self::default()
        }
    }

    /// Derive the offset from the physical signal chain.
    ///
    /// - `sensitivity_mv_per_pa`: capsule output at 1 Pa, from its datasheet.
    /// - `preamp_gain_db`: gain between capsule and converter.
    /// - `full_scale_volts`: the RMS voltage of a sine that reads 0 dBFS. This
    ///   matches the analyzer's convention that 0 dBFS is a full-scale *sine*,
    ///   not a full-scale square.
    ///
    /// Returns `None` for non-positive sensitivity or full-scale voltage, which
    /// would otherwise produce an infinite offset.
    pub fn from_signal_chain(
        sensitivity_mv_per_pa: f32,
        preamp_gain_db: f32,
        full_scale_volts: f32,
    ) -> Option<Self> {
        if !(sensitivity_mv_per_pa > 0.0) || !(full_scale_volts > 0.0) {
            return None;
        }
        // Volts at the converter when the capsule sees 1 Pa, i.e. 94 dB SPL.
        let volts_at_one_pascal =
            (sensitivity_mv_per_pa / 1000.0) * 10.0_f32.powf(preamp_gain_db / 20.0);
        let dbfs_at_one_pascal = 20.0 * (volts_at_one_pascal / full_scale_volts).log10();
        Some(Self {
            offset_db: CALIBRATOR_SPL_DB - dbfs_at_one_pascal,
            ..Self::default()
        })
    }

    /// Attach a microphone response curve.
    #[must_use]
    pub fn with_microphone(mut self, curve: ResponseCurve) -> Self {
        self.microphone = curve;
        self
    }

    /// Set the weighting.
    #[must_use]
    pub fn with_weighting(mut self, weighting: Weighting) -> Self {
        self.weighting = weighting;
        self
    }

    /// Whether an SPL offset has been established.
    ///
    /// When false, output is still dBFS and a UI must label it as such rather
    /// than showing a number that looks like SPL.
    pub fn is_calibrated(&self) -> bool {
        self.offset_db != 0.0
    }

    /// Decibels added to convert dBFS to dB SPL.
    pub fn offset_db(&self) -> f32 {
        self.offset_db
    }

    /// The weighting in force.
    pub fn weighting(&self) -> Weighting {
        self.weighting
    }

    /// The microphone curve in force.
    pub fn microphone(&self) -> &ResponseCurve {
        &self.microphone
    }

    /// Convert one bin from dBFS to dB SPL.
    pub fn to_spl(&self, dbfs: f32, hz: f32) -> f32 {
        dbfs + self.offset_db + self.microphone.db_at(hz) + self.weighting.db_at(hz)
    }

    /// Apply the chain across a spectrum in place.
    ///
    /// Bin `k` sits at `k * bin_spacing_hz`. Bin 0 is DC and is left alone: it
    /// has no meaningful weighting and no microphone correction.
    pub fn apply(&self, bins: &mut [f32], bin_spacing_hz: f32) {
        if bin_spacing_hz <= 0.0 {
            return;
        }
        for (index, level) in bins.iter_mut().enumerate().skip(1) {
            *level = self.to_spl(*level, index as f32 * bin_spacing_hz);
        }
    }
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;

    /// The golden test the plan calls for: inject a known reference tone and
    /// check the reported level. If this drifts, every displayed number is wrong
    /// by a constant and nothing else will notice.
    #[test]
    fn a_ninety_four_decibel_reference_tone_reads_back_as_ninety_four() {
        // A calibrator read -40 dBFS through this rig.
        let cal = Calibration::from_reference_tone(-40.0, CALIBRATOR_SPL_DB);
        assert!((cal.offset_db() - 134.0).abs() < 1e-4);
        assert!((cal.to_spl(-40.0, 1000.0) - 94.0).abs() < 1e-4);
    }

    /// The same rig computed from datasheet numbers instead of a calibrator must
    /// land in the same place.
    #[test]
    fn the_physical_chain_agrees_with_a_reference_tone() {
        // 12.6 mV/Pa capsule, 20 dB of preamp gain, 1.0 V RMS full scale.
        let cal = Calibration::from_signal_chain(12.6, 20.0, 1.0).unwrap();

        // 1 Pa gives 0.0126 V * 10 = 0.126 V, which is -18.0 dBFS against 1 V.
        let expected_dbfs = 20.0 * (0.126_f32).log10();
        let equivalent = Calibration::from_reference_tone(expected_dbfs, CALIBRATOR_SPL_DB);
        assert!(
            (cal.offset_db() - equivalent.offset_db()).abs() < 1e-3,
            "chain {} vs tone {}",
            cal.offset_db(),
            equivalent.offset_db()
        );
        // And a 94 dB source still reads 94.
        assert!((cal.to_spl(expected_dbfs, 1000.0) - 94.0).abs() < 1e-3);
    }

    #[test]
    fn more_preamp_gain_means_a_smaller_offset() {
        let low = Calibration::from_signal_chain(12.6, 0.0, 1.0).unwrap();
        let high = Calibration::from_signal_chain(12.6, 20.0, 1.0).unwrap();
        // 20 dB more gain means the same SPL reads 20 dB hotter, so the offset
        // needed to reach SPL drops by exactly 20.
        assert!((low.offset_db() - high.offset_db() - 20.0).abs() < 1e-3);
    }

    #[test]
    fn a_more_sensitive_capsule_means_a_smaller_offset() {
        let quiet = Calibration::from_signal_chain(1.0, 0.0, 1.0).unwrap();
        let loud = Calibration::from_signal_chain(10.0, 0.0, 1.0).unwrap();
        assert!((quiet.offset_db() - loud.offset_db() - 20.0).abs() < 1e-3);
    }

    #[test]
    fn degenerate_chain_values_are_rejected() {
        assert!(Calibration::from_signal_chain(0.0, 0.0, 1.0).is_none());
        assert!(Calibration::from_signal_chain(-1.0, 0.0, 1.0).is_none());
        assert!(Calibration::from_signal_chain(12.6, 0.0, 0.0).is_none());
        assert!(Calibration::from_signal_chain(f32::NAN, 0.0, 1.0).is_none());
    }

    #[test]
    fn the_default_is_uncalibrated_and_says_so() {
        let cal = Calibration::default();
        assert!(!cal.is_calibrated());
        // Uncalibrated must pass dBFS straight through, not invent an SPL.
        assert!((cal.to_spl(-40.0, 1000.0) - -40.0).abs() < 1e-6);
    }

    #[test]
    fn the_microphone_curve_is_added_on_top() {
        let curve = ResponseCurve::new(vec![(100.0, -3.0), (1000.0, 0.0), (10_000.0, 2.0)]);
        let cal = Calibration::from_reference_tone(-40.0, 94.0).with_microphone(curve);

        assert!((cal.to_spl(-40.0, 1000.0) - 94.0).abs() < 1e-3);
        assert!((cal.to_spl(-40.0, 100.0) - 91.0).abs() < 1e-3);
        assert!((cal.to_spl(-40.0, 10_000.0) - 96.0).abs() < 1e-3);
    }

    #[test]
    fn weighting_is_added_on_top_of_everything_else() {
        let cal = Calibration::from_reference_tone(-40.0, 94.0).with_weighting(Weighting::A);
        // A is 0 dB at 1 kHz by definition.
        assert!((cal.to_spl(-40.0, 1000.0) - 94.0).abs() < 0.05);
        // And about -19.1 dB at 100 Hz.
        assert!((cal.to_spl(-40.0, 100.0) - (94.0 - 19.1)).abs() < 0.2);
    }

    /// All three stages compose, and the order does not silently drop one.
    #[test]
    fn offset_curve_and_weighting_all_apply_together() {
        let curve = ResponseCurve::new(vec![(100.0, -3.0)]);
        let cal = Calibration::from_reference_tone(-40.0, 94.0)
            .with_microphone(curve)
            .with_weighting(Weighting::A);

        // 134 offset, -3 capsule, -19.1 A-weighting.
        let expected = -40.0 + 134.0 - 3.0 - 19.1;
        assert!(
            (cal.to_spl(-40.0, 100.0) - expected).abs() < 0.2,
            "got {}, expected about {expected}",
            cal.to_spl(-40.0, 100.0)
        );
    }

    #[test]
    fn apply_transforms_a_whole_spectrum_and_leaves_dc_alone() {
        let cal = Calibration::from_reference_tone(-40.0, 94.0);
        let mut bins = vec![-40.0_f32; 5];
        cal.apply(&mut bins, 1000.0);

        assert!((bins[0] - -40.0).abs() < 1e-6, "DC must not be shifted");
        for level in &bins[1..] {
            assert!((level - 94.0).abs() < 1e-3, "got {level}");
        }
    }

    #[test]
    fn apply_with_a_bad_spacing_does_nothing() {
        let cal = Calibration::from_reference_tone(-40.0, 94.0);
        let mut bins = vec![-40.0_f32; 4];
        cal.apply(&mut bins, 0.0);
        assert!(bins.iter().all(|l| (*l - -40.0).abs() < 1e-6));
    }
}
