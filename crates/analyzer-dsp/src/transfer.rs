//! Dual-FFT transfer function measurement.
//!
//! A spectrum shows what came out. It cannot show what the system *did*, because
//! it does not know what went in. Measuring two channels at once — a reference
//! of what was sent and a measurement of what was heard — and dividing one by
//! the other cancels the source content entirely. That is what makes it possible
//! to measure with pink noise, or with the actual programme material during a
//! show, and get the same answer either way.
//!
//! # The estimator
//!
//! With `X` the reference spectrum and `Y` the measurement:
//!
//! ```text
//! Gxx = mean(|X|²)          reference auto-power
//! Gyy = mean(|Y|²)          measurement auto-power
//! Gxy = mean(Y · conj(X))   cross-spectrum
//!
//! H₁  = Gxy / Gxx           complex response
//! γ²  = |Gxy|² / (Gxx·Gyy)  coherence
//! ```
//!
//! **The averaging happens on the spectra, never on `H` itself.** Averaging the
//! ratio is the classic mistake here: it converges to something else, it does not
//! suppress noise the same way, and it leaves coherence meaningless because
//! coherence is defined in terms of those averaged products.
//!
//! # Coherence needs more than one frame
//!
//! With a single frame `|Gxy|² = Gxx·Gyy` identically, so coherence is exactly 1
//! no matter how noisy the measurement. It only becomes informative once several
//! frames have been averaged and uncorrelated content has had a chance to cancel
//! in the cross-spectrum while still summing in the auto-spectra.
//! [`TransferFunction::frames`] is exposed so a display can refuse to show
//! coherence before it means anything.

use crate::Complex32;
use crate::fft::{Fft, RealFft};
use crate::spectrum::Overlap;
use crate::window::{Window, WindowKind};

/// Floor for magnitude output where the reference carries no energy.
pub const MAGNITUDE_FLOOR_DB: f32 = -200.0;

/// How successive frames combine.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub enum TransferAveraging {
    /// Average everything since the last reset. Variance keeps falling, so this
    /// is the mode for a static measurement.
    #[default]
    Infinite,
    /// Exponential moving average, for a live display that must track changes.
    Exponential {
        /// Smoothing coefficient in `0..=1`. Smaller is smoother and slower.
        alpha: f32,
    },
}

/// Setup for a [`TransferFunction`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TransferConfig {
    /// Sample rate in hertz.
    pub sample_rate: f32,
    /// FFT size. Must be even and at least two.
    pub size: usize,
    /// Analysis window.
    pub window: WindowKind,
    /// Frame overlap.
    pub overlap: Overlap,
    /// Averaging mode.
    pub averaging: TransferAveraging,
}

impl Default for TransferConfig {
    fn default() -> Self {
        Self {
            sample_rate: 48_000.0,
            size: 4096,
            window: WindowKind::Hann,
            overlap: Overlap::ThreeQuarters,
            averaging: TransferAveraging::Infinite,
        }
    }
}

/// Two-channel transfer function estimator.
///
/// Feed matched reference and measurement samples with
/// [`TransferFunction::push`]. All buffers are sized at construction, so pushing
/// does not allocate.
#[derive(Debug)]
pub struct TransferFunction {
    sample_rate: f32,
    fft: RealFft,
    window: Window,
    hop: usize,
    averaging: TransferAveraging,

    reference_frame: Vec<f32>,
    measurement_frame: Vec<f32>,
    filled: usize,

    windowed: Vec<f32>,
    reference_spectrum: Vec<Complex32>,
    measurement_spectrum: Vec<Complex32>,

    gxx: Vec<f32>,
    gyy: Vec<f32>,
    gxy: Vec<Complex32>,
    frames: u32,
}

impl TransferFunction {
    /// Build an estimator.
    ///
    /// # Panics
    ///
    /// Panics if `size` is odd or below two, or `sample_rate` is not positive.
    pub fn new(config: TransferConfig) -> Self {
        assert!(
            config.sample_rate > 0.0,
            "sample rate must be positive, got {}",
            config.sample_rate
        );
        let fft = RealFft::new(config.size);
        let bins = fft.bins();
        Self {
            sample_rate: config.sample_rate,
            window: Window::new(config.window, config.size),
            hop: config.overlap.hop(config.size),
            averaging: config.averaging,
            reference_frame: vec![0.0; config.size],
            measurement_frame: vec![0.0; config.size],
            filled: 0,
            windowed: vec![0.0; config.size],
            reference_spectrum: vec![Complex32::default(); bins],
            measurement_spectrum: vec![Complex32::default(); bins],
            gxx: vec![0.0; bins],
            gyy: vec![0.0; bins],
            gxy: vec![Complex32::default(); bins],
            frames: 0,
            fft,
        }
    }

    /// Feed matched samples, returning how many frames completed.
    ///
    /// The two slices must be **sample aligned**: element `n` of each is the same
    /// instant. That alignment is exactly why the capture ring carries
    /// interleaved frames rather than one queue per channel — channels drifting
    /// apart by a single sample would corrupt every phase reading here.
    ///
    /// # Panics
    ///
    /// Panics if the slices differ in length. That is a programmer error, and
    /// silently truncating to the shorter one would produce a plausible-looking
    /// but wrong measurement.
    pub fn push(&mut self, reference: &[f32], measurement: &[f32]) -> usize {
        assert_eq!(
            reference.len(),
            measurement.len(),
            "reference and measurement must be sample aligned"
        );

        let size = self.reference_frame.len();
        let mut produced = 0;
        let mut offset = 0;

        while offset < reference.len() {
            let wanted = size - self.filled;
            let taken = wanted.min(reference.len() - offset);

            if let (Some(dst), Some(src)) = (
                self.reference_frame
                    .get_mut(self.filled..self.filled + taken),
                reference.get(offset..offset + taken),
            ) {
                dst.copy_from_slice(src);
            }
            if let (Some(dst), Some(src)) = (
                self.measurement_frame
                    .get_mut(self.filled..self.filled + taken),
                measurement.get(offset..offset + taken),
            ) {
                dst.copy_from_slice(src);
            }

            self.filled += taken;
            offset += taken;

            if self.filled == size {
                self.process_frame();
                produced += 1;
                self.reference_frame.copy_within(self.hop.., 0);
                self.measurement_frame.copy_within(self.hop.., 0);
                self.filled = size - self.hop;
            }
        }
        produced
    }

    fn process_frame(&mut self) {
        self.window
            .apply_to(&self.reference_frame, &mut self.windowed);
        self.fft
            .forward(&self.windowed, &mut self.reference_spectrum);

        self.window
            .apply_to(&self.measurement_frame, &mut self.windowed);
        self.fft
            .forward(&self.windowed, &mut self.measurement_spectrum);

        // Weight for this frame's contribution. Infinite averaging uses an
        // incremental mean, which stays stable however long a measurement runs.
        let weight = match self.averaging {
            TransferAveraging::Infinite => 1.0 / (self.frames as f32 + 1.0),
            TransferAveraging::Exponential { alpha } => {
                if self.frames == 0 {
                    1.0
                } else {
                    alpha.clamp(0.0, 1.0)
                }
            }
        };

        for index in 0..self.gxx.len() {
            let (Some(x), Some(y)) = (
                self.reference_spectrum.get(index),
                self.measurement_spectrum.get(index),
            ) else {
                continue;
            };

            let auto_x = x.norm_sqr();
            let auto_y = y.norm_sqr();
            // Y · conj(X). The conjugate goes on the reference so that a
            // measurement delayed relative to it produces negative phase, which
            // is the sign convention every analyser displays.
            let cross = y * x.conj();

            if let Some(slot) = self.gxx.get_mut(index) {
                *slot += (auto_x - *slot) * weight;
            }
            if let Some(slot) = self.gyy.get_mut(index) {
                *slot += (auto_y - *slot) * weight;
            }
            if let Some(slot) = self.gxy.get_mut(index) {
                *slot += (cross - *slot) * weight;
            }
        }

        self.frames = self.frames.saturating_add(1);
    }

    /// Magnitude of `H₁` in decibels.
    ///
    /// # Panics
    ///
    /// Panics if `out` is not one element per bin.
    pub fn write_magnitude_db(&self, out: &mut [f32]) {
        assert_eq!(out.len(), self.gxx.len(), "output must be one per bin");
        for ((slot, cross), auto) in out.iter_mut().zip(&self.gxy).zip(&self.gxx) {
            *slot = if *auto > 0.0 {
                let magnitude = cross.norm() / auto;
                if magnitude > 0.0 {
                    20.0 * magnitude.log10()
                } else {
                    MAGNITUDE_FLOOR_DB
                }
            } else {
                // No reference energy in this bin means nothing can be said about
                // the system there. A zero would read as "flat", which is a lie.
                MAGNITUDE_FLOOR_DB
            };
        }
    }

    /// The complex response `H₁` itself.
    ///
    /// Magnitude and phase separately are what a display wants, but anything
    /// that interpolates between frequencies needs the complex value: averaging
    /// two phases across a ±180° wrap gives an answer pointing the wrong way.
    ///
    /// # Panics
    ///
    /// Panics if `out` is not one element per bin.
    pub fn write_response(&self, out: &mut [Complex32]) {
        assert_eq!(out.len(), self.gxx.len(), "output must be one per bin");
        for ((slot, cross), auto) in out.iter_mut().zip(&self.gxy).zip(&self.gxx) {
            *slot = if *auto > 0.0 {
                cross / *auto
            } else {
                Complex32::default()
            };
        }
    }

    /// Phase of `H₁` in degrees, wrapped to `-180..=180`.
    ///
    /// # Panics
    ///
    /// Panics if `out` is not one element per bin.
    pub fn write_phase_degrees(&self, out: &mut [f32]) {
        assert_eq!(out.len(), self.gxy.len(), "output must be one per bin");
        for (slot, cross) in out.iter_mut().zip(&self.gxy) {
            *slot = cross.arg().to_degrees();
        }
    }

    /// Coherence, `0..=1`.
    ///
    /// The fraction of the measurement linearly explained by the reference. Low
    /// values mean noise, nonlinearity, or a timing mismatch, and mark the parts
    /// of the response that should not be believed.
    ///
    /// Meaningless until several frames have been averaged — see the module docs.
    ///
    /// # Panics
    ///
    /// Panics if `out` is not one element per bin.
    pub fn write_coherence(&self, out: &mut [f32]) {
        assert_eq!(out.len(), self.gxx.len(), "output must be one per bin");
        for (((slot, cross), auto_x), auto_y) in
            out.iter_mut().zip(&self.gxy).zip(&self.gxx).zip(&self.gyy)
        {
            let denominator = auto_x * auto_y;
            *slot = if denominator > 0.0 {
                // Clamped because floating-point error can push a perfectly
                // coherent bin a hair above one, and a coherence of 1.0000001
                // looks like a bug to anyone reading it.
                (cross.norm_sqr() / denominator).clamp(0.0, 1.0)
            } else {
                0.0
            };
        }
    }

    /// Frames folded into the current average.
    pub fn frames(&self) -> u32 {
        self.frames
    }

    /// Number of bins.
    pub fn bins(&self) -> usize {
        self.gxx.len()
    }

    /// FFT size.
    pub fn size(&self) -> usize {
        self.reference_frame.len()
    }

    /// Samples between frames.
    pub fn hop(&self) -> usize {
        self.hop
    }

    /// Width of one bin in hertz.
    pub fn bin_spacing_hz(&self) -> f32 {
        self.sample_rate / self.size() as f32
    }

    /// Centre frequency of bin `index`.
    pub fn bin_frequency(&self, index: usize) -> f32 {
        index as f32 * self.bin_spacing_hz()
    }

    /// Discard the average and any partial frame.
    pub fn reset(&mut self) {
        self.gxx.fill(0.0);
        self.gyy.fill(0.0);
        self.gxy.fill(Complex32::default());
        self.reference_frame.fill(0.0);
        self.measurement_frame.fill(0.0);
        self.filled = 0;
        self.frames = 0;
    }
}

/// Unwrap a wrapped phase curve in place, removing the ±360° jumps.
///
/// Wrapped phase is what a display wants; unwrapped is what group delay and any
/// slope measurement need, because a wrap looks like an infinite derivative.
pub fn unwrap_phase_degrees(phase: &mut [f32]) {
    let mut offset = 0.0_f32;
    let mut previous = phase.first().copied().unwrap_or(0.0);
    for value in phase.iter_mut() {
        let raw = *value;
        let step = raw - previous;
        if step > 180.0 {
            offset -= 360.0;
        } else if step < -180.0 {
            offset += 360.0;
        }
        previous = raw;
        *value = raw + offset;
    }
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::{Generator, Signal};

    const RATE: f32 = 48_000.0;
    const SIZE: usize = 4096;

    fn estimator(averaging: TransferAveraging) -> TransferFunction {
        TransferFunction::new(TransferConfig {
            sample_rate: RATE,
            size: SIZE,
            window: WindowKind::Hann,
            overlap: Overlap::Half,
            averaging,
        })
    }

    fn pink(samples: usize, seed: u64) -> Vec<f32> {
        let mut generator = Generator::new(RATE, Signal::PinkNoise { amplitude: 0.5 }, seed);
        let mut out = vec![0.0; samples];
        generator.fill(&mut out);
        out
    }

    /// Bins with enough reference energy to say anything about, avoiding the
    /// extreme ends where pink noise runs out of level.
    fn usable(tf: &TransferFunction) -> std::ops::Range<usize> {
        let low = (200.0 / tf.bin_spacing_hz()) as usize;
        let high = (8000.0 / tf.bin_spacing_hz()) as usize;
        low..high.min(tf.bins())
    }

    /// Measuring a wire: the measurement is the reference, so the system is
    /// unity gain, zero phase, perfect coherence.
    #[test]
    fn an_identical_signal_measures_as_a_perfect_wire() {
        let signal = pink(SIZE * 32, 1);
        let mut tf = estimator(TransferAveraging::Infinite);
        tf.push(&signal, &signal);
        assert!(tf.frames() > 8);

        let mut magnitude = vec![0.0; tf.bins()];
        let mut phase = vec![0.0; tf.bins()];
        let mut coherence = vec![0.0; tf.bins()];
        tf.write_magnitude_db(&mut magnitude);
        tf.write_phase_degrees(&mut phase);
        tf.write_coherence(&mut coherence);

        for bin in usable(&tf) {
            assert!(
                magnitude[bin].abs() < 0.01,
                "bin {bin}: {} dB",
                magnitude[bin]
            );
            assert!(phase[bin].abs() < 0.1, "bin {bin}: {}°", phase[bin]);
            assert!(coherence[bin] > 0.999, "bin {bin}: γ² {}", coherence[bin]);
        }
    }

    #[test]
    fn a_gain_shows_up_as_a_flat_offset() {
        let reference = pink(SIZE * 32, 2);
        let measurement: Vec<f32> = reference.iter().map(|s| s * 2.0).collect();

        let mut tf = estimator(TransferAveraging::Infinite);
        tf.push(&reference, &measurement);

        let mut magnitude = vec![0.0; tf.bins()];
        tf.write_magnitude_db(&mut magnitude);
        for bin in usable(&tf) {
            assert!(
                (magnitude[bin] - 6.0206).abs() < 0.01,
                "bin {bin}: {} dB, expected +6.02",
                magnitude[bin]
            );
        }
    }

    #[test]
    fn an_attenuation_shows_up_as_a_negative_offset() {
        let reference = pink(SIZE * 32, 3);
        let measurement: Vec<f32> = reference.iter().map(|s| s * 0.5).collect();

        let mut tf = estimator(TransferAveraging::Infinite);
        tf.push(&reference, &measurement);

        let mut magnitude = vec![0.0; tf.bins()];
        tf.write_magnitude_db(&mut magnitude);
        for bin in usable(&tf) {
            assert!((magnitude[bin] + 6.0206).abs() < 0.01, "bin {bin}");
        }
    }

    /// A pure delay is flat in magnitude and linear in unwrapped phase, with a
    /// slope set by the delay. This is the property the delay finder will later
    /// exploit, and the sign convention matters: a later measurement means
    /// negative phase.
    #[test]
    fn a_pure_delay_gives_flat_magnitude_and_linear_phase() {
        let delay = 32_usize;
        let reference = pink(SIZE * 32, 4);
        let mut measurement = vec![0.0; reference.len()];
        measurement[delay..].copy_from_slice(&reference[..reference.len() - delay]);

        let mut tf = estimator(TransferAveraging::Infinite);
        tf.push(&reference, &measurement);

        let mut magnitude = vec![0.0; tf.bins()];
        let mut phase = vec![0.0; tf.bins()];
        tf.write_magnitude_db(&mut magnitude);
        tf.write_phase_degrees(&mut phase);
        unwrap_phase_degrees(&mut phase);

        let band = usable(&tf);
        for bin in band.clone() {
            assert!(
                magnitude[bin].abs() < 0.2,
                "delay must not change magnitude: bin {bin} = {} dB",
                magnitude[bin]
            );
        }

        // Phase slope in degrees per hertz should be -360 * delay / rate.
        let low = band.start;
        let high = band.end - 1;
        let measured_slope =
            (phase[high] - phase[low]) / (tf.bin_frequency(high) - tf.bin_frequency(low));
        let expected_slope = -360.0 * delay as f32 / RATE;
        assert!(
            (measured_slope - expected_slope).abs() < 0.001,
            "slope {measured_slope} deg/Hz, expected {expected_slope}"
        );
    }

    /// Uncorrelated signals must show low coherence. This is the metric's whole
    /// purpose: telling the user which parts of a curve to believe.
    #[test]
    fn uncorrelated_signals_have_low_coherence() {
        let reference = pink(SIZE * 64, 5);
        let measurement = pink(SIZE * 64, 6);

        let mut tf = estimator(TransferAveraging::Infinite);
        tf.push(&reference, &measurement);

        let mut coherence = vec![0.0; tf.bins()];
        tf.write_coherence(&mut coherence);

        let band = usable(&tf);
        let mean: f32 = coherence[band.clone()].iter().sum::<f32>() / band.len() as f32;
        assert!(mean < 0.2, "uncorrelated mean coherence was {mean}");
    }

    /// Signal plus independent noise sits between the two extremes, and more
    /// noise must lower it.
    #[test]
    fn coherence_falls_as_noise_is_added() {
        let reference = pink(SIZE * 64, 7);
        let noise = pink(SIZE * 64, 8);

        let mean_coherence = |noise_gain: f32| -> f32 {
            let measurement: Vec<f32> = reference
                .iter()
                .zip(&noise)
                .map(|(s, n)| s + n * noise_gain)
                .collect();
            let mut tf = estimator(TransferAveraging::Infinite);
            tf.push(&reference, &measurement);
            let mut coherence = vec![0.0; tf.bins()];
            tf.write_coherence(&mut coherence);
            let band = usable(&tf);
            coherence[band.clone()].iter().sum::<f32>() / band.len() as f32
        };

        let clean = mean_coherence(0.05);
        let noisy = mean_coherence(1.0);
        assert!(clean > 0.9, "nearly clean should be coherent, got {clean}");
        assert!(
            noisy < clean - 0.2,
            "noise must reduce coherence: {clean} -> {noisy}"
        );
    }

    /// Documents the trap rather than hiding it: one frame is always perfectly
    /// coherent by construction, whatever the signals are.
    #[test]
    fn a_single_frame_reports_coherence_of_one_even_for_noise() {
        let reference = pink(SIZE, 9);
        let measurement = pink(SIZE, 10);

        let mut tf = estimator(TransferAveraging::Infinite);
        tf.push(&reference, &measurement);
        assert_eq!(tf.frames(), 1);

        let mut coherence = vec![0.0; tf.bins()];
        tf.write_coherence(&mut coherence);
        for bin in usable(&tf) {
            assert!(
                coherence[bin] > 0.99,
                "single-frame coherence should be 1, got {}",
                coherence[bin]
            );
        }
    }

    #[test]
    fn coherence_always_lies_between_zero_and_one() {
        let reference = pink(SIZE * 16, 11);
        let measurement = pink(SIZE * 16, 12);
        let mut tf = estimator(TransferAveraging::Infinite);
        tf.push(&reference, &measurement);

        let mut coherence = vec![0.0; tf.bins()];
        tf.write_coherence(&mut coherence);
        for (bin, value) in coherence.iter().enumerate() {
            assert!(
                (0.0..=1.0).contains(value),
                "bin {bin} coherence out of range: {value}"
            );
        }
    }

    /// Where the reference has no energy, magnitude must floor rather than
    /// reading 0 dB, which would look like a flat response.
    #[test]
    fn silent_reference_bins_floor_rather_than_reading_flat() {
        let mut tf = estimator(TransferAveraging::Infinite);
        let silence = vec![0.0; SIZE * 4];
        tf.push(&silence, &silence);

        let mut magnitude = vec![0.0; tf.bins()];
        tf.write_magnitude_db(&mut magnitude);
        assert!(magnitude.iter().all(|m| *m <= MAGNITUDE_FLOOR_DB + 1e-3));
        assert!(magnitude.iter().all(|m| m.is_finite()));
    }

    #[test]
    fn chunking_does_not_change_the_result() {
        let reference = pink(SIZE * 8, 13);
        let measurement: Vec<f32> = reference.iter().map(|s| s * 0.7).collect();

        let mut whole = estimator(TransferAveraging::Infinite);
        whole.push(&reference, &measurement);

        let mut chunked = estimator(TransferAveraging::Infinite);
        for (r, m) in reference.chunks(97).zip(measurement.chunks(97)) {
            chunked.push(r, m);
        }

        assert_eq!(whole.frames(), chunked.frames());
        let mut a = vec![0.0; whole.bins()];
        let mut b = vec![0.0; chunked.bins()];
        whole.write_magnitude_db(&mut a);
        chunked.write_magnitude_db(&mut b);
        for (bin, (x, y)) in a.iter().zip(&b).enumerate() {
            assert!((x - y).abs() < 1e-3, "bin {bin}: {x} vs {y}");
        }
    }

    #[test]
    fn reset_clears_everything() {
        let signal = pink(SIZE * 4, 14);
        let mut tf = estimator(TransferAveraging::Infinite);
        tf.push(&signal, &signal);
        assert!(tf.frames() > 0);

        tf.reset();
        assert_eq!(tf.frames(), 0);
        let mut coherence = vec![1.0; tf.bins()];
        tf.write_coherence(&mut coherence);
        assert!(coherence.iter().all(|c| *c == 0.0));
    }

    #[test]
    #[should_panic(expected = "sample aligned")]
    fn mismatched_lengths_are_rejected() {
        let mut tf = estimator(TransferAveraging::Infinite);
        tf.push(&[0.0; 100], &[0.0; 99]);
    }

    #[test]
    fn phase_unwrapping_removes_the_jumps() {
        let mut phase = vec![170.0, 179.0, -179.0, -170.0, -179.0, 179.0, 170.0];
        unwrap_phase_degrees(&mut phase);
        // Should rise monotonically past 180 then come back, with no 360 steps.
        for pair in phase.windows(2) {
            assert!(
                (pair[1] - pair[0]).abs() < 180.0,
                "unwrap left a jump: {pair:?}"
            );
        }
        assert!((phase[2] - 181.0).abs() < 1e-3, "got {}", phase[2]);
    }

    #[test]
    fn unwrapping_an_empty_or_single_value_is_harmless() {
        let mut empty: Vec<f32> = Vec::new();
        unwrap_phase_degrees(&mut empty);
        let mut single = vec![42.0];
        unwrap_phase_degrees(&mut single);
        assert_eq!(single, vec![42.0]);
    }

    /// Exponential averaging must track a change rather than holding the old
    /// answer forever.
    #[test]
    fn exponential_averaging_follows_a_change() {
        let reference = pink(SIZE * 32, 15);
        let quiet: Vec<f32> = reference.iter().map(|s| s * 0.1).collect();
        let loud: Vec<f32> = reference.iter().map(|s| s * 2.0).collect();

        let mut tf = estimator(TransferAveraging::Exponential { alpha: 0.3 });
        tf.push(&reference, &quiet);
        let mut magnitude = vec![0.0; tf.bins()];
        tf.write_magnitude_db(&mut magnitude);
        let before = magnitude[usable(&tf).start];

        tf.push(&reference, &loud);
        tf.write_magnitude_db(&mut magnitude);
        let after = magnitude[usable(&tf).start];

        assert!(before < -15.0, "expected about -20 dB, got {before}");
        assert!(
            after > before + 10.0,
            "should have tracked upward: {before} -> {after}"
        );
    }
}
