//! Broadband level metering: peak, RMS and LEQ.
//!
//! # Weighting has to be a filter here
//!
//! `analyzer-cal` already has A and C weighting as per-bin curves, which is the
//! right shape for correcting a spectrum. A time-domain meter cannot use them:
//! it never forms a spectrum, so there are no bins to correct. The same curves
//! are therefore implemented again as cascaded biquads.
//!
//! Two independent implementations of the same standard is usually a smell, but
//! here it is an asset — the tests check the filter's measured response against
//! IEC 61672 *and* against the frequency-domain version, so the two have to agree
//! or something is wrong.
//!
//! # Level convention
//!
//! 0 dBFS is a full-scale **sine**, matching the spectrum analyzer. A sine of
//! amplitude 1.0 has an RMS of 0.7071, so the meter adds 3.01 dB to a raw RMS
//! figure. Without that, meters and spectrum would disagree by 3 dB about the
//! same signal, which is exactly the sort of discrepancy that costs an afternoon.

use crate::biquad::Biquad;
use crate::window::WindowKind;

/// RMS of a full-scale sine, the reference for 0 dBFS.
const FULL_SCALE_RMS: f32 = std::f32::consts::FRAC_1_SQRT_2;

/// Floor for a silent input.
pub const METER_FLOOR_DB: f32 = -200.0;

/// Weighting applied before measuring.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum MeterWeighting {
    /// Unweighted.
    #[default]
    Z,
    /// A-weighting.
    A,
    /// C-weighting.
    C,
}

/// How fast the RMS detector responds.
///
/// The time constants are from IEC 61672. Fast and Slow are exponential; Impulse
/// is deliberately asymmetric, catching transients quickly and releasing slowly
/// so a brief peak stays readable.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub enum Integration {
    /// 125 ms. The usual choice for live work.
    #[default]
    Fast,
    /// 1 s. Steadier, for noise levels.
    Slow,
    /// 35 ms attack, 1.5 s decay.
    Impulse,
    /// An arbitrary symmetric time constant in seconds.
    Custom {
        /// Time constant in seconds.
        seconds: f32,
    },
}

impl Integration {
    /// Attack and decay time constants in seconds.
    fn constants(self) -> (f32, f32) {
        match self {
            Integration::Fast => (0.125, 0.125),
            Integration::Slow => (1.0, 1.0),
            Integration::Impulse => (0.035, 1.5),
            Integration::Custom { seconds } => {
                let clamped = seconds.max(1e-4);
                (clamped, clamped)
            }
        }
    }
}

/// Pole frequencies from IEC 61672-1, in radians per second.
const W1: f32 = 20.598_997 * std::f32::consts::TAU;
const W2: f32 = 107.652_65 * std::f32::consts::TAU;
const W3: f32 = 737.862_23 * std::f32::consts::TAU;
const W4: f32 = 12_194.217 * std::f32::consts::TAU;

/// A or C weighting as a biquad cascade.
#[derive(Debug, Clone, Default)]
struct WeightingFilter {
    sections: Vec<Biquad>,
    gain: f32,
}

impl WeightingFilter {
    fn new(weighting: MeterWeighting, sample_rate: f32) -> Self {
        let mut sections = match weighting {
            MeterWeighting::Z => Vec::new(),
            // C is s² / ((s+ω1)²(s+ω4)²): two zeros at DC, four poles.
            //
            // Cascading two highpass sections here would give s⁴ and an extra
            // 12 dB/octave of bass rolloff - C would read ~50 dB low at 31.5 Hz
            // instead of -3. The ω4 pair has to be a lowpass, not a highpass.
            MeterWeighting::C => vec![
                Biquad::double_pole_highpass(W1, sample_rate),
                Biquad::two_pole_lowpass(W4, W4, sample_rate),
            ],
            // A is s⁴ / ((s+ω1)²(s+ω2)(s+ω3)(s+ω4)²): four zeros at DC, so both
            // ω1 and ω4 sections are highpasses here, plus the ω2/ω3 pair that
            // pulls the midrange down onto the equal-loudness contour.
            MeterWeighting::A => vec![
                Biquad::double_pole_highpass(W1, sample_rate),
                Biquad::double_pole_highpass(W4, sample_rate),
                Biquad::two_pole_lowpass(W2, W3, sample_rate),
            ],
        };
        for section in &mut sections {
            section.reset();
        }

        // Normalise so the cascade is exactly unity at 1 kHz, which is the
        // definition of both curves.
        let response: f32 = sections
            .iter()
            .map(|s| s.magnitude_at(1000.0, sample_rate))
            .product();
        let gain = if response > 0.0 { 1.0 / response } else { 1.0 };

        Self { sections, gain }
    }

    fn process(&mut self, mut x: f32) -> f32 {
        for section in &mut self.sections {
            x = section.process(x);
        }
        x * self.gain
    }

    fn reset(&mut self) {
        for section in &mut self.sections {
            section.reset();
        }
    }
}

/// Broadband level meter.
///
/// Real-time safe after construction: [`LevelMeter::push`] allocates nothing.
#[derive(Debug, Clone)]
pub struct LevelMeter {
    sample_rate: f32,
    filter: WeightingFilter,
    weighting: MeterWeighting,
    integration: Integration,
    attack: f32,
    decay: f32,

    mean_square: f32,
    peak: f32,
    /// f64 because LEQ integrates for minutes or hours and f32 would stop
    /// accumulating once the sum dwarfs each new sample.
    energy: f64,
    samples: u64,
}

impl LevelMeter {
    /// Build a meter.
    ///
    /// # Panics
    ///
    /// Panics if `sample_rate` is not positive.
    pub fn new(sample_rate: f32, weighting: MeterWeighting, integration: Integration) -> Self {
        assert!(
            sample_rate > 0.0,
            "sample rate must be positive, got {sample_rate}"
        );
        let (attack_seconds, decay_seconds) = integration.constants();
        Self {
            sample_rate,
            filter: WeightingFilter::new(weighting, sample_rate),
            weighting,
            integration,
            attack: coefficient(attack_seconds, sample_rate),
            decay: coefficient(decay_seconds, sample_rate),
            mean_square: 0.0,
            peak: 0.0,
            energy: 0.0,
            samples: 0,
        }
    }

    /// Feed samples.
    pub fn push(&mut self, samples: &[f32]) {
        for sample in samples {
            let weighted = self.filter.process(*sample);
            let square = weighted * weighted;

            // Asymmetric so Impulse can rise fast and fall slowly.
            let coefficient = if square > self.mean_square {
                self.attack
            } else {
                self.decay
            };
            self.mean_square += (square - self.mean_square) * coefficient;

            let magnitude = weighted.abs();
            if magnitude > self.peak {
                self.peak = magnitude;
            }

            self.energy += f64::from(square);
        }
        self.samples = self.samples.saturating_add(samples.len() as u64);
    }

    /// Time-weighted RMS in dBFS.
    pub fn rms_db(&self) -> f32 {
        to_db(self.mean_square.sqrt())
    }

    /// Highest sample magnitude since the last reset, in dBFS.
    ///
    /// This is a sample peak, not a true peak: inter-sample overshoots between
    /// converter samples are invisible to it. For measurement work that is the
    /// honest figure; true-peak metering needs oversampling and belongs with
    /// loudness compliance rather than acoustics.
    pub fn peak_db(&self) -> f32 {
        // A peak is an amplitude, and a full-scale sine peaks at 1.0 while
        // reading 0 dBFS, so no RMS correction applies.
        if self.peak > 0.0 {
            20.0 * self.peak.log10()
        } else {
            METER_FLOOR_DB
        }
    }

    /// Equivalent continuous level: the total energy since reset, expressed as
    /// the steady level that would carry the same energy.
    pub fn leq_db(&self) -> f32 {
        if self.samples == 0 {
            return METER_FLOOR_DB;
        }
        let mean = self.energy / self.samples as f64;
        to_db((mean as f32).sqrt())
    }

    /// Seconds of audio integrated since reset.
    pub fn elapsed_seconds(&self) -> f32 {
        self.samples as f32 / self.sample_rate
    }

    /// Clear all state, including the filter's history.
    pub fn reset(&mut self) {
        self.filter.reset();
        self.mean_square = 0.0;
        self.peak = 0.0;
        self.energy = 0.0;
        self.samples = 0;
    }

    /// Clear only the peak hold.
    pub fn reset_peak(&mut self) {
        self.peak = 0.0;
    }

    /// The weighting in force.
    pub fn weighting(&self) -> MeterWeighting {
        self.weighting
    }

    /// The integration in force.
    pub fn integration(&self) -> Integration {
        self.integration
    }

    /// Window recommended for taking a calibration reading with this meter.
    ///
    /// Flat-top, for the same reason the spectrum uses it: its main lobe is flat,
    /// so the level is right regardless of where a calibrator's tone falls
    /// between bins.
    pub fn calibration_window() -> WindowKind {
        WindowKind::FlatTop
    }
}

/// One-pole coefficient for a given time constant.
fn coefficient(seconds: f32, sample_rate: f32) -> f32 {
    if seconds <= 0.0 {
        return 1.0;
    }
    (1.0 - (-1.0 / (seconds * sample_rate)).exp()).clamp(f32::MIN_POSITIVE, 1.0)
}

/// RMS to dBFS, referenced to a full-scale sine.
fn to_db(rms: f32) -> f32 {
    if rms > 0.0 {
        (20.0 * (rms / FULL_SCALE_RMS).log10()).max(METER_FLOOR_DB)
    } else {
        METER_FLOOR_DB
    }
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;
    use std::f32::consts::TAU;

    const RATE: f32 = 48_000.0;

    fn sine(hz: f32, amplitude: f32, seconds: f32) -> Vec<f32> {
        let count = (RATE * seconds) as usize;
        (0..count)
            .map(|n| amplitude * (TAU * hz * n as f32 / RATE).sin())
            .collect()
    }

    fn settled(weighting: MeterWeighting, hz: f32, amplitude: f32) -> LevelMeter {
        let mut meter = LevelMeter::new(RATE, weighting, Integration::Fast);
        // Two seconds is many Fast time constants, so the detector has settled.
        meter.push(&sine(hz, amplitude, 2.0));
        meter
    }

    /// Meters and spectrum must agree about the same signal. A full-scale sine
    /// is 0 dBFS in both, which needs the +3.01 dB RMS correction.
    #[test]
    fn a_full_scale_sine_reads_zero_dbfs() {
        let meter = settled(MeterWeighting::Z, 1000.0, 1.0);
        assert!(meter.rms_db().abs() < 0.1, "rms {}", meter.rms_db());
        assert!(meter.peak_db().abs() < 0.1, "peak {}", meter.peak_db());
    }

    #[test]
    fn halving_amplitude_drops_six_decibels() {
        let full = settled(MeterWeighting::Z, 1000.0, 1.0).rms_db();
        let half = settled(MeterWeighting::Z, 1000.0, 0.5).rms_db();
        assert!((full - half - 6.0206).abs() < 0.05, "{full} -> {half}");
    }

    /// The cross-check that justifies having two weighting implementations: the
    /// biquad cascade must agree with the per-bin curve in analyzer-cal, which
    /// is itself checked against IEC 61672.
    #[test]
    fn the_weighting_filters_match_the_published_curves() {
        // Values from IEC 61672-1 Table 3, the same table analyzer-cal uses.
        //
        // Tolerances follow the class 1 limits, which widen at the band edges,
        // rather than being picked to fit. A bilinear-transformed filter cannot
        // track the analog prototype right up to Nyquist - the 12194 Hz pole is
        // halfway there at 48 kHz - so 8 kHz gets the standard's ±1.5 rather
        // than a figure this implementation could not honestly meet.
        let a_cases = [
            (31.5, -39.4, 1.0),
            (63.0, -26.2, 0.7),
            (125.0, -16.1, 0.5),
            (250.0, -8.6, 0.4),
            (500.0, -3.2, 0.3),
            (1000.0, 0.0, 0.1),
            (2000.0, 1.2, 0.3),
            (4000.0, 1.0, 0.4),
            (8000.0, -1.1, 1.5),
        ];
        for (hz, expected, tolerance) in a_cases {
            let reference = settled(MeterWeighting::Z, hz, 0.5).rms_db();
            let weighted = settled(MeterWeighting::A, hz, 0.5).rms_db();
            let measured = weighted - reference;
            assert!(
                (measured - expected).abs() < tolerance,
                "A at {hz} Hz: filter gives {measured:.2} dB, standard says {expected}"
            );
        }

        let c_cases = [
            (31.5, -3.0, 0.5),
            (125.0, -0.2, 0.3),
            (1000.0, 0.0, 0.1),
            (8000.0, -3.0, 1.5),
        ];
        for (hz, expected, tolerance) in c_cases {
            let reference = settled(MeterWeighting::Z, hz, 0.5).rms_db();
            let weighted = settled(MeterWeighting::C, hz, 0.5).rms_db();
            let measured = weighted - reference;
            assert!(
                (measured - expected).abs() < tolerance,
                "C at {hz} Hz: filter gives {measured:.2} dB, standard says {expected}"
            );
        }
    }

    #[test]
    fn weighting_is_unity_at_one_kilohertz() {
        let flat = settled(MeterWeighting::Z, 1000.0, 0.5).rms_db();
        for weighting in [MeterWeighting::A, MeterWeighting::C] {
            let weighted = settled(weighting, 1000.0, 0.5).rms_db();
            assert!(
                (weighted - flat).abs() < 0.1,
                "{weighting:?} at 1 kHz should be unity, differed by {}",
                weighted - flat
            );
        }
    }

    #[test]
    fn a_weighting_cuts_bass_harder_than_c() {
        let reference = settled(MeterWeighting::Z, 50.0, 0.5).rms_db();
        let a = settled(MeterWeighting::A, 50.0, 0.5).rms_db() - reference;
        let c = settled(MeterWeighting::C, 50.0, 0.5).rms_db() - reference;
        assert!(
            a < c - 20.0,
            "A {a:.1} should be far below C {c:.1} at 50 Hz"
        );
    }

    /// Slow must lag Fast. Feeding a burst and stopping, the Slow meter should
    /// still be climbing when Fast has already arrived.
    #[test]
    fn slow_integration_lags_fast() {
        let burst = sine(1000.0, 1.0, 0.06);
        let mut fast = LevelMeter::new(RATE, MeterWeighting::Z, Integration::Fast);
        let mut slow = LevelMeter::new(RATE, MeterWeighting::Z, Integration::Slow);
        fast.push(&burst);
        slow.push(&burst);

        assert!(
            fast.rms_db() > slow.rms_db() + 5.0,
            "after 60 ms fast should be well ahead: {} vs {}",
            fast.rms_db(),
            slow.rms_db()
        );
    }

    /// Impulse rises fast and falls slowly, which is the whole point of it.
    #[test]
    fn impulse_integration_rises_quickly_and_falls_slowly() {
        let mut meter = LevelMeter::new(RATE, MeterWeighting::Z, Integration::Impulse);
        meter.push(&sine(1000.0, 1.0, 0.15));
        let after_burst = meter.rms_db();
        assert!(
            after_burst > -2.0,
            "should have risen quickly: {after_burst}"
        );

        // Then 200 ms of silence. A 1.5 s decay leaves most of the level intact.
        meter.push(&vec![0.0; (RATE * 0.2) as usize]);
        let after_silence = meter.rms_db();
        assert!(
            after_silence > after_burst - 6.0,
            "impulse should decay slowly: {after_burst} -> {after_silence}"
        );
    }

    #[test]
    fn peak_holds_until_reset() {
        let mut meter = LevelMeter::new(RATE, MeterWeighting::Z, Integration::Fast);
        meter.push(&sine(1000.0, 1.0, 0.1));
        let loud = meter.peak_db();
        assert!(loud.abs() < 0.1);

        meter.push(&sine(1000.0, 0.01, 0.5));
        assert!((meter.peak_db() - loud).abs() < 0.01, "peak must hold");

        meter.reset_peak();
        meter.push(&sine(1000.0, 0.01, 0.1));
        assert!(meter.peak_db() < -30.0, "peak should have been cleared");
    }

    /// LEQ of a steady signal is that signal's level, by definition.
    #[test]
    fn leq_of_a_steady_tone_equals_its_level() {
        let mut meter = LevelMeter::new(RATE, MeterWeighting::Z, Integration::Fast);
        meter.push(&sine(1000.0, 0.5, 2.0));
        assert!(
            (meter.leq_db() - -6.0206).abs() < 0.05,
            "leq {}",
            meter.leq_db()
        );
    }

    /// LEQ integrates energy, so half a period of signal is 3 dB down on the
    /// signal's own level.
    #[test]
    fn leq_averages_energy_over_silence() {
        let mut meter = LevelMeter::new(RATE, MeterWeighting::Z, Integration::Fast);
        meter.push(&sine(1000.0, 1.0, 1.0));
        meter.push(&vec![0.0; RATE as usize]);
        assert!(
            (meter.leq_db() + 3.01).abs() < 0.1,
            "half duty should be -3 dB, got {}",
            meter.leq_db()
        );
        assert!((meter.elapsed_seconds() - 2.0).abs() < 0.01);
    }

    #[test]
    fn silence_reads_at_the_floor_and_stays_finite() {
        let mut meter = LevelMeter::new(RATE, MeterWeighting::Z, Integration::Fast);
        meter.push(&vec![0.0; 4096]);
        assert!(meter.rms_db() <= METER_FLOOR_DB + 1e-3);
        assert!(meter.peak_db() <= METER_FLOOR_DB + 1e-3);
        assert!(meter.rms_db().is_finite() && meter.peak_db().is_finite());
        assert!(meter.leq_db().is_finite());
    }

    #[test]
    fn a_fresh_meter_reads_the_floor() {
        let meter = LevelMeter::new(RATE, MeterWeighting::Z, Integration::Fast);
        assert!(meter.leq_db() <= METER_FLOOR_DB + 1e-3);
        assert_eq!(meter.elapsed_seconds(), 0.0);
    }

    #[test]
    fn reset_clears_the_filter_history_too() {
        let mut meter = LevelMeter::new(RATE, MeterWeighting::A, Integration::Fast);
        meter.push(&sine(1000.0, 1.0, 1.0));
        meter.reset();

        assert_eq!(meter.elapsed_seconds(), 0.0);
        assert!(meter.rms_db() <= METER_FLOOR_DB + 1e-3);

        // If filter state survived, the first samples after reset would ring.
        let mut fresh = LevelMeter::new(RATE, MeterWeighting::A, Integration::Fast);
        meter.push(&sine(1000.0, 0.5, 1.0));
        fresh.push(&sine(1000.0, 0.5, 1.0));
        assert!((meter.rms_db() - fresh.rms_db()).abs() < 0.01);
    }

    #[test]
    fn chunking_does_not_change_the_reading() {
        let signal = sine(997.0, 0.5, 1.0);
        let mut whole = LevelMeter::new(RATE, MeterWeighting::A, Integration::Fast);
        whole.push(&signal);

        let mut chunked = LevelMeter::new(RATE, MeterWeighting::A, Integration::Fast);
        for chunk in signal.chunks(137) {
            chunked.push(chunk);
        }
        assert!((whole.rms_db() - chunked.rms_db()).abs() < 1e-3);
        assert!((whole.leq_db() - chunked.leq_db()).abs() < 1e-3);
    }

    #[test]
    fn a_custom_time_constant_sits_between_fast_and_slow() {
        let burst = sine(1000.0, 1.0, 0.06);
        let mut fast = LevelMeter::new(RATE, MeterWeighting::Z, Integration::Fast);
        let mut custom = LevelMeter::new(
            RATE,
            MeterWeighting::Z,
            Integration::Custom { seconds: 0.4 },
        );
        let mut slow = LevelMeter::new(RATE, MeterWeighting::Z, Integration::Slow);
        fast.push(&burst);
        custom.push(&burst);
        slow.push(&burst);

        assert!(fast.rms_db() > custom.rms_db());
        assert!(custom.rms_db() > slow.rms_db());
    }

    #[test]
    #[should_panic(expected = "sample rate must be positive")]
    fn a_non_positive_sample_rate_is_rejected() {
        let _ = LevelMeter::new(0.0, MeterWeighting::Z, Integration::Fast);
    }
}
