//! What a stored measurement is.
//!
//! # Storage rules
//!
//! Three decisions here are load-bearing, and all three are cheap now and
//! impossible to retrofit once someone has a folder of saved measurements.
//!
//! **Unsmoothed, at the native sample rate, complex.** Smoothing and
//! fractional-octave banding are *view* transforms. Storing a smoothed magnitude
//! curve throws away the phase and the resolution, which permanently forecloses
//! group delay, RT60, minimum-phase decomposition and any meaningful interop.
//!
//! **`f64`, not `f32`.** Analysis runs in `f32` because that is what converters
//! deliver and what SIMD likes. Storage is different: a file is read back,
//! processed, and written again, possibly many times, and rounding at every
//! round trip accumulates. Doubling the file size is a trivial price.
//!
//! **Absolute references travel with the data.** Time zero, full-scale voltages,
//! reference resistance, SPL offset. Without them a measurement cannot be
//! compared with another, converted to real units, or aligned in time — it
//! becomes a picture of a measurement rather than a measurement.

use std::fmt;

/// A complex value in storage precision.
pub type Complex64 = rustfft::num_complex::Complex<f64>;

/// Identifier for a measurement within a session.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct MeasurementId(pub u64);

impl fmt::Display for MeasurementId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "#{}", self.0)
    }
}

/// Absolute references that make a measurement comparable and convertible.
///
/// Every field is optional because an uncalibrated measurement is a legitimate
/// thing to have. `None` means "not known", which is different from zero and
/// must not be silently substituted for it.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct References {
    /// Decibels added to dBFS to obtain dB SPL.
    pub spl_offset_db: Option<f64>,
    /// RMS volts that read full scale on the input converter.
    pub full_scale_input_volts: Option<f64>,
    /// RMS volts the output converter produces at full scale.
    pub full_scale_output_volts: Option<f64>,
    /// Series resistance used for impedance measurement, in ohms.
    pub reference_resistance_ohms: Option<f64>,
    /// Acoustic propagation delay already removed from the data, in seconds.
    ///
    /// Recording this matters as much as removing it: two measurements aligned
    /// by different amounts cannot be compared, and there is no way to tell
    /// after the fact unless the amount was written down.
    pub propagation_delay_seconds: Option<f64>,
}

impl References {
    /// Whether anything at all is known.
    pub fn is_empty(&self) -> bool {
        *self == Self::default()
    }
}

/// The payload of a measurement.
#[derive(Debug, Clone, PartialEq)]
pub enum MeasurementData {
    /// A power spectrum, stored as complex bins so phase survives.
    Spectrum {
        /// One complex value per bin, bin `k` at `k * bin_spacing_hz`.
        bins: Vec<Complex64>,
        /// Hertz between bins.
        bin_spacing_hz: f64,
    },
    /// An impulse response in the time domain.
    ImpulseResponse {
        /// Samples at the measurement's sample rate.
        samples: Vec<f64>,
        /// Index of `t = 0`, which is fractional because the direct arrival
        /// rarely lands on a sample. Rounding it away costs sub-sample alignment
        /// and, at 48 kHz, 7 mm of path length per sample.
        time_zero_samples: f64,
    },
    /// A magnitude-only spectrum, as an RTA produces.
    ///
    /// Deliberately a separate variant rather than a [`MeasurementData::Spectrum`]
    /// with zero imaginary parts. A power spectrum discards phase when it squares
    /// the magnitude - there is no phase to store, and writing zeros would be
    /// indistinguishable from having measured zero phase. Later code would
    /// believe it.
    PowerSpectrum {
        /// Level per bin in decibels.
        magnitude_db: Vec<f64>,
        /// Hertz between bins.
        bin_spacing_hz: f64,
    },
    /// A two-channel transfer function with its coherence.
    TransferFunction {
        /// Complex response per bin.
        bins: Vec<Complex64>,
        /// Coherence per bin, `0..=1`. Kept alongside because a response without
        /// it cannot be judged - there is no way to tell which parts to believe.
        coherence: Vec<f64>,
        /// Hertz between bins.
        bin_spacing_hz: f64,
    },
}

impl MeasurementData {
    /// A short label for the kind of data.
    pub fn kind(&self) -> &'static str {
        match self {
            MeasurementData::Spectrum { .. } => "spectrum",
            MeasurementData::PowerSpectrum { .. } => "power_spectrum",
            MeasurementData::ImpulseResponse { .. } => "impulse_response",
            MeasurementData::TransferFunction { .. } => "transfer_function",
        }
    }

    /// Number of stored points.
    pub fn len(&self) -> usize {
        match self {
            MeasurementData::Spectrum { bins, .. } => bins.len(),
            MeasurementData::PowerSpectrum { magnitude_db, .. } => magnitude_db.len(),
            MeasurementData::ImpulseResponse { samples, .. } => samples.len(),
            MeasurementData::TransferFunction { bins, .. } => bins.len(),
        }
    }

    /// Whether there is no data.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Bin spacing, for frequency-domain data.
    pub fn bin_spacing_hz(&self) -> Option<f64> {
        match self {
            MeasurementData::Spectrum { bin_spacing_hz, .. }
            | MeasurementData::PowerSpectrum { bin_spacing_hz, .. }
            | MeasurementData::TransferFunction { bin_spacing_hz, .. } => Some(*bin_spacing_hz),
            MeasurementData::ImpulseResponse { .. } => None,
        }
    }

    /// Magnitude in decibels per bin, for frequency-domain data.
    ///
    /// A view, computed on demand. Deliberately not stored — see the module docs.
    pub fn magnitude_db(&self) -> Option<Vec<f64>> {
        let bins = match self {
            MeasurementData::Spectrum { bins, .. }
            | MeasurementData::TransferFunction { bins, .. } => bins,
            // Already in decibels; nothing to derive.
            MeasurementData::PowerSpectrum { magnitude_db, .. } => {
                return Some(magnitude_db.clone());
            }
            MeasurementData::ImpulseResponse { .. } => return None,
        };
        Some(
            bins.iter()
                .map(|bin| {
                    let magnitude = bin.norm();
                    if magnitude > 0.0 {
                        20.0 * magnitude.log10()
                    } else {
                        -200.0
                    }
                })
                .collect(),
        )
    }

    /// Phase in degrees per bin, for frequency-domain data.
    pub fn phase_degrees(&self) -> Option<Vec<f64>> {
        let bins = match self {
            MeasurementData::Spectrum { bins, .. }
            | MeasurementData::TransferFunction { bins, .. } => bins,
            // No phase was ever measured, so none is reported.
            MeasurementData::PowerSpectrum { .. } | MeasurementData::ImpulseResponse { .. } => {
                return None;
            }
        };
        Some(bins.iter().map(|bin| bin.arg().to_degrees()).collect())
    }
}

/// One saved measurement.
#[derive(Debug, Clone, PartialEq)]
pub struct Measurement {
    /// Identifier within its session.
    pub id: MeasurementId,
    /// Name shown to the user.
    pub name: String,
    /// Free-text notes.
    pub notes: String,
    /// Seconds since the Unix epoch when this was captured.
    ///
    /// A plain number rather than a `SystemTime`, because it has to survive a
    /// round trip through a file and back without depending on a clock type.
    pub captured_at: i64,
    /// Rate the measurement was taken at. Never assumed.
    pub sample_rate: f64,
    /// How many input channels contributed.
    pub channels: usize,
    /// Absolute references, so far as they are known.
    pub references: References,
    /// The data itself.
    pub data: MeasurementData,
}

impl Measurement {
    /// Build a measurement with empty metadata.
    pub fn new(
        id: MeasurementId,
        name: impl Into<String>,
        sample_rate: f64,
        data: MeasurementData,
    ) -> Self {
        Self {
            id,
            name: name.into(),
            notes: String::new(),
            captured_at: 0,
            sample_rate,
            channels: 1,
            references: References::default(),
            data,
        }
    }

    /// Frequency of bin `index`, for frequency-domain data.
    pub fn bin_frequency(&self, index: usize) -> Option<f64> {
        self.data
            .bin_spacing_hz()
            .map(|spacing| index as f64 * spacing)
    }

    /// Duration in seconds, for time-domain data.
    pub fn duration_seconds(&self) -> Option<f64> {
        match &self.data {
            MeasurementData::ImpulseResponse { samples, .. } if self.sample_rate > 0.0 => {
                Some(samples.len() as f64 / self.sample_rate)
            }
            _ => None,
        }
    }

    /// Whether an SPL offset is recorded, meaning levels can be shown as SPL
    /// rather than dBFS.
    pub fn is_spl_calibrated(&self) -> bool {
        self.references.spl_offset_db.is_some()
    }
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;

    fn spectrum() -> MeasurementData {
        MeasurementData::Spectrum {
            bins: vec![
                Complex64::new(1.0, 0.0),
                Complex64::new(0.5, 0.5),
                Complex64::new(0.0, -1.0),
            ],
            bin_spacing_hz: 10.0,
        }
    }

    #[test]
    fn magnitude_and_phase_are_derived_not_stored() {
        let data = spectrum();
        let magnitude = data.magnitude_db().unwrap();
        let phase = data.phase_degrees().unwrap();

        assert!((magnitude[0] - 0.0).abs() < 1e-9, "unit magnitude is 0 dB");
        assert!((phase[0] - 0.0).abs() < 1e-9);
        // 0 - j means -90 degrees.
        assert!((phase[2] - -90.0).abs() < 1e-9, "got {}", phase[2]);
    }

    #[test]
    fn a_zero_bin_floors_rather_than_producing_infinity() {
        let data = MeasurementData::Spectrum {
            bins: vec![Complex64::new(0.0, 0.0)],
            bin_spacing_hz: 1.0,
        };
        let magnitude = data.magnitude_db().unwrap();
        assert!(magnitude[0].is_finite());
        assert!(magnitude[0] <= -200.0);
    }

    #[test]
    fn time_domain_data_has_no_spectrum_views() {
        let data = MeasurementData::ImpulseResponse {
            samples: vec![0.0; 16],
            time_zero_samples: 4.5,
        };
        assert!(data.magnitude_db().is_none());
        assert!(data.phase_degrees().is_none());
        assert!(data.bin_spacing_hz().is_none());
    }

    #[test]
    fn bin_frequency_follows_the_spacing() {
        let m = Measurement::new(MeasurementId(1), "test", 48_000.0, spectrum());
        assert_eq!(m.bin_frequency(0), Some(0.0));
        assert_eq!(m.bin_frequency(5), Some(50.0));
    }

    #[test]
    fn duration_only_applies_to_time_domain_data() {
        let ir = Measurement::new(
            MeasurementId(1),
            "ir",
            48_000.0,
            MeasurementData::ImpulseResponse {
                samples: vec![0.0; 48_000],
                time_zero_samples: 0.0,
            },
        );
        assert!((ir.duration_seconds().unwrap() - 1.0).abs() < 1e-9);

        let sp = Measurement::new(MeasurementId(2), "sp", 48_000.0, spectrum());
        assert!(sp.duration_seconds().is_none());
    }

    /// None and zero are different. An unknown SPL offset must not read as a
    /// calibrated measurement that happens to need no correction.
    #[test]
    fn unknown_references_are_distinct_from_zero() {
        let mut m = Measurement::new(MeasurementId(1), "test", 48_000.0, spectrum());
        assert!(!m.is_spl_calibrated());
        assert!(m.references.is_empty());

        m.references.spl_offset_db = Some(0.0);
        assert!(
            m.is_spl_calibrated(),
            "an offset of zero is still an offset"
        );
        assert!(!m.references.is_empty());
    }

    #[test]
    fn time_zero_is_fractional() {
        // At 48 kHz one sample is 7 mm of path, so rounding this away loses
        // sub-sample alignment that the delay finder worked to establish.
        let data = MeasurementData::ImpulseResponse {
            samples: vec![0.0; 8],
            time_zero_samples: 3.25,
        };
        let MeasurementData::ImpulseResponse {
            time_zero_samples, ..
        } = data
        else {
            panic!("wrong variant");
        };
        assert!((time_zero_samples - 3.25).abs() < 1e-12);
    }

    #[test]
    fn kinds_are_stable_strings() {
        assert_eq!(spectrum().kind(), "spectrum");
        assert_eq!(
            MeasurementData::ImpulseResponse {
                samples: Vec::new(),
                time_zero_samples: 0.0
            }
            .kind(),
            "impulse_response"
        );
        assert_eq!(
            MeasurementData::TransferFunction {
                bins: Vec::new(),
                coherence: Vec::new(),
                bin_spacing_hz: 1.0
            }
            .kind(),
            "transfer_function"
        );
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod power_spectrum_tests {
    use super::*;

    fn power_spectrum() -> MeasurementData {
        MeasurementData::PowerSpectrum {
            magnitude_db: vec![-40.0, -35.0, -60.0],
            bin_spacing_hz: 10.0,
        }
    }

    #[test]
    fn magnitude_passes_straight_through() {
        assert_eq!(
            power_spectrum().magnitude_db().unwrap(),
            vec![-40.0, -35.0, -60.0]
        );
    }

    /// The reason the variant exists: an RTA never measured phase, so it must
    /// not report any. Zeros here would be indistinguishable from a genuine
    /// zero-phase measurement.
    #[test]
    fn no_phase_is_reported() {
        assert!(power_spectrum().phase_degrees().is_none());
    }

    #[test]
    fn it_is_still_a_frequency_domain_measurement() {
        assert_eq!(power_spectrum().bin_spacing_hz(), Some(10.0));
        assert_eq!(power_spectrum().len(), 3);
        assert_eq!(power_spectrum().kind(), "power_spectrum");
    }
}
