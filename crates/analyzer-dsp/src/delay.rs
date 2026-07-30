//! Finding the propagation delay between reference and measurement.
//!
//! A microphone five metres from a loudspeaker hears everything about fifteen
//! milliseconds late. Left uncompensated that wrecks a transfer function: a
//! constant delay is a phase that rotates ever faster with frequency, and within
//! one analysis frame the two channels stop lining up, so coherence collapses at
//! the top of the band. Finding and removing the delay is a prerequisite, not a
//! refinement.
//!
//! # Why PHAT
//!
//! Plain cross-correlation weights each frequency by how much energy it carries,
//! so a real room — where the bass is loud and reverberant — produces a broad,
//! smeared peak sitting on a forest of reflections. The phase transform (GCC-PHAT)
//! divides the cross-spectrum by its own magnitude, keeping only phase. Every
//! frequency then contributes equally and the direct-sound peak becomes sharp and
//! unambiguous. It is the difference between a delay finder that works in an
//! anechoic chamber and one that works in a room.
//!
//! Plain correlation is still available, because with a very poor
//! signal-to-noise ratio PHAT's equal weighting amplifies bins that are pure
//! noise.

use std::sync::Arc;

use realfft::{ComplexToReal, RealFftPlanner, RealToComplex};

use crate::Complex32;

/// How the cross-spectrum is weighted before transforming back.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Weighting {
    /// Phase transform. Whitens the cross-spectrum so every frequency counts
    /// equally, giving a sharp peak in a reverberant space.
    #[default]
    Phat,
    /// No weighting. Better when noise dominates, worse when reflections do.
    None,
}

/// What the finder concluded.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct DelayEstimate {
    /// Delay in samples. Positive means the measurement arrived *after* the
    /// reference, which is the normal case for a microphone at a distance.
    pub samples: f32,
    /// The same delay in seconds.
    pub seconds: f32,
    /// Height of the correlation peak relative to the mean, a measure of how
    /// sharp and isolated it was. Around 1 means no peak at all; a clean direct
    /// arrival gives tens or hundreds.
    pub confidence: f32,
}

impl DelayEstimate {
    /// Distance the sound travelled, assuming 343 m/s.
    ///
    /// Only meaningful for an acoustic path; an electrical loopback delay is not
    /// a distance.
    pub fn metres(&self) -> f32 {
        self.seconds * 343.0
    }
}

/// Finds the delay between two signals by cross-correlation.
///
/// All buffers are sized at construction.
pub struct DelayFinder {
    sample_rate: f32,
    forward: Arc<dyn RealToComplex<f32>>,
    inverse: Arc<dyn ComplexToReal<f32>>,
    size: usize,

    padded_reference: Vec<f32>,
    padded_measurement: Vec<f32>,
    reference_spectrum: Vec<Complex32>,
    measurement_spectrum: Vec<Complex32>,
    cross: Vec<Complex32>,
    correlation: Vec<f32>,
    forward_scratch: Vec<Complex32>,
    inverse_scratch: Vec<Complex32>,
    weighting: Weighting,
}

impl DelayFinder {
    /// Build a finder over a correlation window of `size` samples.
    ///
    /// The largest delay that can be found is `size / 2` in either direction,
    /// because beyond that the circular correlation wraps and a late arrival is
    /// indistinguishable from an early one. At 48 kHz, 16384 covers ±170 ms,
    /// which is far more than any sane acoustic path.
    ///
    /// # Panics
    ///
    /// Panics if `size` is odd or below four, or `sample_rate` is not positive.
    pub fn new(sample_rate: f32, size: usize, weighting: Weighting) -> Self {
        assert!(
            size >= 4 && size.is_multiple_of(2),
            "correlation size must be even and at least 4, got {size}"
        );
        assert!(
            sample_rate > 0.0,
            "sample rate must be positive, got {sample_rate}"
        );

        let mut planner = RealFftPlanner::<f32>::new();
        let forward = planner.plan_fft_forward(size);
        let inverse = planner.plan_fft_inverse(size);
        let bins = size / 2 + 1;

        Self {
            sample_rate,
            forward_scratch: forward.make_scratch_vec(),
            inverse_scratch: inverse.make_scratch_vec(),
            forward,
            inverse,
            size,
            padded_reference: vec![0.0; size],
            padded_measurement: vec![0.0; size],
            reference_spectrum: vec![Complex32::default(); bins],
            measurement_spectrum: vec![Complex32::default(); bins],
            cross: vec![Complex32::default(); bins],
            correlation: vec![0.0; size],
            weighting,
        }
    }

    /// A finder with sensible defaults for acoustic work.
    pub fn acoustic(sample_rate: f32) -> Self {
        Self::new(sample_rate, 16_384, Weighting::Phat)
    }

    /// Correlation window length.
    pub fn size(&self) -> usize {
        self.size
    }

    /// Largest delay this finder can resolve, in either direction.
    pub fn max_delay_samples(&self) -> usize {
        self.size / 2
    }

    /// Estimate the delay from `reference` to `measurement`.
    ///
    /// Both slices are truncated or zero-padded to the correlation size. Returns
    /// `None` when either signal is silent, since a delay between nothing and
    /// nothing is not a number.
    pub fn find(&mut self, reference: &[f32], measurement: &[f32]) -> Option<DelayEstimate> {
        let take = self.size.min(reference.len()).min(measurement.len());
        if take == 0 {
            return None;
        }

        self.padded_reference.fill(0.0);
        self.padded_measurement.fill(0.0);
        if let (Some(dst), Some(src)) =
            (self.padded_reference.get_mut(..take), reference.get(..take))
        {
            dst.copy_from_slice(src);
        }
        if let (Some(dst), Some(src)) = (
            self.padded_measurement.get_mut(..take),
            measurement.get(..take),
        ) {
            dst.copy_from_slice(src);
        }

        let reference_energy: f32 = self.padded_reference.iter().map(|s| s * s).sum();
        let measurement_energy: f32 = self.padded_measurement.iter().map(|s| s * s).sum();
        if reference_energy <= f32::EPSILON || measurement_energy <= f32::EPSILON {
            return None;
        }

        self.forward
            .process_with_scratch(
                &mut self.padded_reference,
                &mut self.reference_spectrum,
                &mut self.forward_scratch,
            )
            .ok()?;
        self.forward
            .process_with_scratch(
                &mut self.padded_measurement,
                &mut self.measurement_spectrum,
                &mut self.forward_scratch,
            )
            .ok()?;

        for ((slot, y), x) in self
            .cross
            .iter_mut()
            .zip(&self.measurement_spectrum)
            .zip(&self.reference_spectrum)
        {
            // Y · conj(X): a measurement that lags the reference produces a
            // positive peak position.
            let product = y * x.conj();
            *slot = match self.weighting {
                Weighting::None => product,
                Weighting::Phat => {
                    let magnitude = product.norm();
                    if magnitude > 1e-20 {
                        product / magnitude
                    } else {
                        Complex32::default()
                    }
                }
            };
        }

        // A real inverse transform requires DC and Nyquist to be purely real;
        // rounding can leave a tiny imaginary part that the transform rejects.
        if let Some(dc) = self.cross.first_mut() {
            dc.im = 0.0;
        }
        if let Some(nyquist) = self.cross.last_mut() {
            nyquist.im = 0.0;
        }

        self.inverse
            .process_with_scratch(
                &mut self.cross,
                &mut self.correlation,
                &mut self.inverse_scratch,
            )
            .ok()?;

        self.locate_peak()
    }

    fn locate_peak(&self) -> Option<DelayEstimate> {
        let (index, peak) = self
            .correlation
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.total_cmp(b.1))
            .map(|(i, v)| (i, *v))?;

        let mean: f32 = self.correlation.iter().map(|v| v.abs()).sum::<f32>() / self.size as f32;
        let confidence = if mean > 0.0 { peak / mean } else { 0.0 };

        // Sub-sample refinement by fitting a parabola through the peak and its
        // neighbours. Worth doing: at 48 kHz one sample is 7 mm of path length,
        // and alignment work cares at that scale.
        //
        // The neighbours wrap, because the correlation is circular. Reaching for
        // index - 1 directly underflows at index 0 and silently skips refinement
        // for exactly the zero-delay case, which is the one most likely to be
        // sub-sample.
        let before = self
            .correlation
            .get((index + self.size - 1) % self.size)
            .copied()
            .unwrap_or(peak);
        let after = self
            .correlation
            .get((index + 1) % self.size)
            .copied()
            .unwrap_or(peak);
        let curvature = before - 2.0 * peak + after;
        let offset = if curvature.abs() > 1e-20 {
            (0.5 * (before - after) / curvature).clamp(-0.5, 0.5)
        } else {
            0.0
        };

        // The correlation is circular, so the upper half represents negative
        // lags: the measurement arriving *before* the reference.
        let position = index as f32 + offset;
        let samples = if index > self.size / 2 {
            position - self.size as f32
        } else {
            position
        };

        Some(DelayEstimate {
            samples,
            seconds: samples / self.sample_rate,
            confidence,
        })
    }
}

impl std::fmt::Debug for DelayFinder {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("DelayFinder")
            .field("size", &self.size)
            .field("sample_rate", &self.sample_rate)
            .field("weighting", &self.weighting)
            .finish_non_exhaustive()
    }
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::{Generator, Signal};

    const RATE: f32 = 48_000.0;
    const SIZE: usize = 8192;

    fn noise(samples: usize, seed: u64) -> Vec<f32> {
        let mut generator = Generator::new(RATE, Signal::PinkNoise { amplitude: 0.5 }, seed);
        let mut out = vec![0.0; samples];
        generator.fill(&mut out);
        out
    }

    fn delayed(source: &[f32], delay: usize) -> Vec<f32> {
        let mut out = vec![0.0; source.len()];
        if delay < source.len() {
            out[delay..].copy_from_slice(&source[..source.len() - delay]);
        }
        out
    }

    #[test]
    fn an_integer_delay_is_recovered_exactly() {
        let reference = noise(SIZE, 1);
        for delay in [1_usize, 7, 64, 512, 2000] {
            let measurement = delayed(&reference, delay);
            let mut finder = DelayFinder::new(RATE, SIZE, Weighting::Phat);
            let estimate = finder.find(&reference, &measurement).unwrap();
            assert!(
                (estimate.samples - delay as f32).abs() < 0.1,
                "delay {delay} came back as {}",
                estimate.samples
            );
        }
    }

    #[test]
    fn zero_delay_is_found() {
        let reference = noise(SIZE, 2);
        let mut finder = DelayFinder::new(RATE, SIZE, Weighting::Phat);
        let estimate = finder.find(&reference, &reference).unwrap();
        assert!(estimate.samples.abs() < 0.1, "got {}", estimate.samples);
    }

    /// The measurement arriving *before* the reference is a real case - a
    /// mis-patched loopback, or an internal reference taken after the output
    /// buffer. The circular correlation must report it as negative, not as a
    /// huge positive delay.
    #[test]
    fn a_negative_delay_is_reported_as_negative() {
        let source = noise(SIZE, 3);
        let reference = delayed(&source, 300);
        let measurement = source;

        let mut finder = DelayFinder::new(RATE, SIZE, Weighting::Phat);
        let estimate = finder.find(&reference, &measurement).unwrap();
        assert!(
            (estimate.samples + 300.0).abs() < 0.5,
            "expected about -300, got {}",
            estimate.samples
        );
    }

    /// One sample at 48 kHz is 7 mm of path length, so sub-sample resolution is
    /// not a luxury for alignment work.
    #[test]
    fn a_fractional_delay_is_resolved_between_samples() {
        // Half-sample delay via linear interpolation of the source.
        let source = noise(SIZE, 4);
        let mut measurement = vec![0.0; source.len()];
        for i in 1..source.len() {
            measurement[i] = 0.5 * source[i] + 0.5 * source[i - 1];
        }

        let mut finder = DelayFinder::new(RATE, SIZE, Weighting::Phat);
        let estimate = finder.find(&source, &measurement).unwrap();
        assert!(
            (estimate.samples - 0.5).abs() < 0.2,
            "expected about 0.5 samples, got {}",
            estimate.samples
        );
        // And it must not be an integer, which would mean interpolation is off.
        assert!(estimate.samples.fract().abs() > 0.01);
    }

    #[test]
    fn seconds_and_metres_follow_from_samples() {
        let reference = noise(SIZE, 5);
        // 480 samples at 48 kHz is 10 ms, about 3.43 m.
        let measurement = delayed(&reference, 480);
        let mut finder = DelayFinder::new(RATE, SIZE, Weighting::Phat);
        let estimate = finder.find(&reference, &measurement).unwrap();

        assert!(
            (estimate.seconds - 0.01).abs() < 1e-4,
            "{}",
            estimate.seconds
        );
        assert!(
            (estimate.metres() - 3.43).abs() < 0.05,
            "{}",
            estimate.metres()
        );
    }

    /// The property PHAT exists for. With strong reflections, plain correlation
    /// smears and can latch onto the wrong arrival; PHAT should stay on the
    /// direct sound.
    #[test]
    fn phat_survives_reflections_better_than_plain_correlation() {
        let direct = 200_usize;
        let source = noise(SIZE, 6);

        // Direct arrival plus three strong, slightly later reflections - the
        // shape of a small room.
        let mut measurement = vec![0.0; source.len()];
        for (delay, gain) in [(direct, 1.0_f32), (263, 0.8), (341, 0.75), (455, 0.7)] {
            for i in delay..source.len() {
                measurement[i] += source[i - delay] * gain;
            }
        }

        let mut phat = DelayFinder::new(RATE, SIZE, Weighting::Phat);
        let with_phat = phat.find(&source, &measurement).unwrap();
        assert!(
            (with_phat.samples - direct as f32).abs() < 1.0,
            "PHAT should find the direct arrival at {direct}, got {}",
            with_phat.samples
        );

        // PHAT's peak should also be far sharper relative to the background.
        let mut plain = DelayFinder::new(RATE, SIZE, Weighting::None);
        let without = plain.find(&source, &measurement).unwrap();
        assert!(
            with_phat.confidence > without.confidence,
            "PHAT peak should be sharper: {} vs {}",
            with_phat.confidence,
            without.confidence
        );
    }

    #[test]
    fn uncorrelated_signals_give_low_confidence() {
        let reference = noise(SIZE, 7);
        let measurement = noise(SIZE, 8);

        let mut finder = DelayFinder::new(RATE, SIZE, Weighting::Phat);
        let unrelated = finder.find(&reference, &measurement).unwrap();

        let matched = finder.find(&reference, &delayed(&reference, 100)).unwrap();
        assert!(
            matched.confidence > unrelated.confidence * 3.0,
            "a real delay should be far more confident: {} vs {}",
            matched.confidence,
            unrelated.confidence
        );
    }

    #[test]
    fn silence_yields_no_estimate() {
        let mut finder = DelayFinder::new(RATE, SIZE, Weighting::Phat);
        let silence = vec![0.0; SIZE];
        let signal = noise(SIZE, 9);

        assert!(finder.find(&silence, &silence).is_none());
        assert!(finder.find(&signal, &silence).is_none());
        assert!(finder.find(&silence, &signal).is_none());
        assert!(finder.find(&[], &[]).is_none());
    }

    #[test]
    fn short_input_is_zero_padded_rather_than_refused() {
        let reference = noise(1000, 10);
        let measurement = delayed(&reference, 50);
        let mut finder = DelayFinder::new(RATE, SIZE, Weighting::Phat);
        let estimate = finder.find(&reference, &measurement).unwrap();
        assert!(
            (estimate.samples - 50.0).abs() < 1.0,
            "{}",
            estimate.samples
        );
    }

    #[test]
    fn a_finder_can_be_reused_without_carrying_state() {
        let mut finder = DelayFinder::new(RATE, SIZE, Weighting::Phat);
        let a = noise(SIZE, 11);
        let b = noise(SIZE, 12);

        let first = finder.find(&a, &delayed(&a, 111)).unwrap();
        let second = finder.find(&b, &delayed(&b, 222)).unwrap();
        let third = finder.find(&a, &delayed(&a, 111)).unwrap();

        assert!((first.samples - 111.0).abs() < 0.5);
        assert!((second.samples - 222.0).abs() < 0.5);
        assert!(
            (first.samples - third.samples).abs() < 1e-3,
            "repeat gave a different answer"
        );
    }

    #[test]
    fn the_acoustic_default_covers_a_realistic_room() {
        let finder = DelayFinder::acoustic(RATE);
        // ±170 ms is about ±58 m of path, far beyond any real room.
        assert!(finder.max_delay_samples() >= 8000);
    }

    #[test]
    #[should_panic(expected = "correlation size must be even")]
    fn an_odd_size_is_rejected() {
        let _ = DelayFinder::new(RATE, 1023, Weighting::Phat);
    }
}
