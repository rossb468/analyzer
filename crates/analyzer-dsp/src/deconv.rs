//! Recovering an impulse response from a swept measurement.
//!
//! Play a known stimulus, record what comes back, and divide one by the other in
//! the frequency domain. What falls out is the impulse response — everything the
//! system did to the signal, in one time-domain trace, from which the frequency
//! response, group delay, reverberation time and waterfall all follow.
//!
//! # Why division needs regularising
//!
//! `H = Y / X` is exact and unusable. Wherever the stimulus has no energy — below
//! its start frequency, above its end, in any notch — `X` approaches zero and the
//! quotient explodes, turning measurement noise into enormous spurious content
//! that swamps the real impulse.
//!
//! So the division is regularised:
//!
//! ```text
//! H = Y · conj(X) / (|X|² + ε · max|X|²)
//! ```
//!
//! Where the stimulus is strong the epsilon term is negligible and this is plain
//! division. Where it is weak, the denominator floors out and the result rolls
//! gently to zero instead of blowing up. `ε` is the noise floor being assumed: at
//! the 1e-6 default, anything more than 60 dB below the stimulus peak is treated
//! as unmeasurable rather than amplified.
//!
//! # Why an exponential sweep
//!
//! Any stimulus works here — that is the point of deconvolving rather than using
//! a matched filter. But an exponential sweep has a property nothing else does:
//! its harmonic distortion products appear at *negative* time, bunched before the
//! linear impulse, so they can be windowed away or measured separately. A linear
//! sweep smears them across the response with no way to separate them.

use std::sync::Arc;

use realfft::{ComplexToReal, RealFftPlanner, RealToComplex};

use crate::Complex32;

/// Default regularisation: treat anything 60 dB below the stimulus peak as noise.
pub const DEFAULT_REGULARISATION: f32 = 1e-6;

/// A recovered impulse response.
#[derive(Debug, Clone, PartialEq)]
pub struct ImpulseResponse {
    /// The response, at the measurement sample rate.
    pub samples: Vec<f32>,
    /// Index of the direct arrival, fractional from parabolic interpolation.
    pub peak_samples: f32,
    /// Sample rate the measurement ran at.
    pub sample_rate: f32,
}

impl ImpulseResponse {
    /// Time in seconds of sample `index`, relative to the direct arrival.
    pub fn time_at(&self, index: usize) -> f32 {
        if self.sample_rate <= 0.0 {
            return 0.0;
        }
        (index as f32 - self.peak_samples) / self.sample_rate
    }

    /// Peak absolute amplitude.
    pub fn peak_amplitude(&self) -> f32 {
        self.samples.iter().fold(0.0_f32, |m, s| m.max(s.abs()))
    }

    /// Length in seconds.
    pub fn duration_seconds(&self) -> f32 {
        if self.sample_rate <= 0.0 {
            return 0.0;
        }
        self.samples.len() as f32 / self.sample_rate
    }
}

/// Deconvolves a response against a stimulus.
///
/// Sized at construction; `deconvolve` allocates only the output.
pub struct Deconvolver {
    size: usize,
    sample_rate: f32,
    forward: Arc<dyn RealToComplex<f32>>,
    inverse: Arc<dyn ComplexToReal<f32>>,
    padded: Vec<f32>,
    stimulus_spectrum: Vec<Complex32>,
    response_spectrum: Vec<Complex32>,
    quotient: Vec<Complex32>,
    result: Vec<f32>,
    forward_scratch: Vec<Complex32>,
    inverse_scratch: Vec<Complex32>,
}

impl Deconvolver {
    /// Build a deconvolver for signals up to `max_length` samples.
    ///
    /// The transform is sized to the next power of two at or above twice
    /// `max_length`, because circular convolution would otherwise wrap the tail
    /// of the response around onto the start — reverberation folding back onto
    /// the direct sound, which looks like a pre-echo that is not there.
    ///
    /// # Panics
    ///
    /// Panics if `max_length` is zero or `sample_rate` is not positive.
    pub fn new(sample_rate: f32, max_length: usize) -> Self {
        assert!(max_length > 0, "max_length must be non-zero");
        assert!(
            sample_rate > 0.0,
            "sample rate must be positive, got {sample_rate}"
        );

        let size = (max_length * 2).next_power_of_two();
        let mut planner = RealFftPlanner::<f32>::new();
        let forward = planner.plan_fft_forward(size);
        let inverse = planner.plan_fft_inverse(size);
        let bins = size / 2 + 1;

        Self {
            size,
            sample_rate,
            forward_scratch: forward.make_scratch_vec(),
            inverse_scratch: inverse.make_scratch_vec(),
            forward,
            inverse,
            padded: vec![0.0; size],
            stimulus_spectrum: vec![Complex32::default(); bins],
            response_spectrum: vec![Complex32::default(); bins],
            quotient: vec![Complex32::default(); bins],
            result: vec![0.0; size],
        }
    }

    /// Transform length in use.
    pub fn size(&self) -> usize {
        self.size
    }

    /// Longest input this deconvolver accepts.
    pub fn max_length(&self) -> usize {
        self.size / 2
    }

    /// Recover the impulse response.
    ///
    /// `regularisation` is the assumed noise floor as a fraction of stimulus peak
    /// power; [`DEFAULT_REGULARISATION`] is a reasonable starting point. Larger
    /// values suppress noise harder at the cost of accuracy where the stimulus
    /// was weak.
    ///
    /// Returns `None` if either signal is silent or longer than
    /// [`Deconvolver::max_length`].
    pub fn deconvolve(
        &mut self,
        stimulus: &[f32],
        response: &[f32],
        regularisation: f32,
    ) -> Option<ImpulseResponse> {
        if stimulus.is_empty() || response.is_empty() {
            return None;
        }
        if stimulus.len() > self.max_length() || response.len() > self.max_length() {
            return None;
        }

        self.transform(stimulus, true)?;
        self.transform(response, false)?;

        // Floor the denominator relative to the strongest bin, so the epsilon
        // means the same thing regardless of how loud the measurement was.
        let peak_power = self
            .stimulus_spectrum
            .iter()
            .map(Complex32::norm_sqr)
            .fold(0.0_f32, f32::max);
        if peak_power <= 0.0 {
            return None;
        }
        let floor = regularisation.max(0.0) * peak_power;

        for ((slot, y), x) in self
            .quotient
            .iter_mut()
            .zip(&self.response_spectrum)
            .zip(&self.stimulus_spectrum)
        {
            let denominator = x.norm_sqr() + floor;
            *slot = if denominator > 0.0 {
                (y * x.conj()) / denominator
            } else {
                Complex32::default()
            };
        }

        // A real inverse transform demands purely real DC and Nyquist bins;
        // rounding can leave a residue the transform rejects.
        if let Some(dc) = self.quotient.first_mut() {
            dc.im = 0.0;
        }
        if let Some(nyquist) = self.quotient.last_mut() {
            nyquist.im = 0.0;
        }

        self.inverse
            .process_with_scratch(
                &mut self.quotient,
                &mut self.result,
                &mut self.inverse_scratch,
            )
            .ok()?;

        // realfft's inverse is unnormalised.
        let scale = 1.0 / self.size as f32;
        for sample in &mut self.result {
            *sample *= scale;
        }

        let peak = locate_peak(&self.result)?;
        Some(ImpulseResponse {
            samples: self.result.clone(),
            peak_samples: peak,
            sample_rate: self.sample_rate,
        })
    }

    fn transform(&mut self, input: &[f32], is_stimulus: bool) -> Option<()> {
        self.padded.fill(0.0);
        if let Some(dst) = self.padded.get_mut(..input.len()) {
            dst.copy_from_slice(input);
        }
        let target = if is_stimulus {
            &mut self.stimulus_spectrum
        } else {
            &mut self.response_spectrum
        };
        self.forward
            .process_with_scratch(&mut self.padded, target, &mut self.forward_scratch)
            .ok()
    }
}

/// Index of the largest absolute sample, refined between samples.
fn locate_peak(samples: &[f32]) -> Option<f32> {
    let (index, peak) = samples
        .iter()
        .enumerate()
        .max_by(|a, b| a.1.abs().total_cmp(&b.1.abs()))
        .map(|(i, v)| (i, v.abs()))?;

    // Parabolic refinement on the magnitude envelope. Neighbours are clamped
    // rather than wrapped: unlike a circular correlation, an impulse response has
    // real ends and the sample before index 0 does not exist.
    let before = index
        .checked_sub(1)
        .and_then(|i| samples.get(i))
        .map(|s| s.abs())
        .unwrap_or(peak);
    let after = samples.get(index + 1).map(|s| s.abs()).unwrap_or(peak);

    let curvature = before - 2.0 * peak + after;
    let offset = if curvature.abs() > 1e-20 {
        (0.5 * (before - after) / curvature).clamp(-0.5, 0.5)
    } else {
        0.0
    };
    Some(index as f32 + offset)
}

impl std::fmt::Debug for Deconvolver {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Deconvolver")
            .field("size", &self.size)
            .field("sample_rate", &self.sample_rate)
            .finish_non_exhaustive()
    }
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::{Generator, Signal};

    const RATE: f32 = 48_000.0;
    const LENGTH: usize = 16_384;

    fn sweep(samples: usize) -> Vec<f32> {
        let mut generator = Generator::new(
            RATE,
            Signal::Sweep {
                start_hz: 20.0,
                end_hz: 20_000.0,
                seconds: samples as f32 / RATE,
                amplitude: 0.5,
                repeat: false,
            },
            1,
        );
        let mut out = vec![0.0; samples];
        generator.fill(&mut out);
        out
    }

    /// A recording of `source` arriving `delay` samples late.
    ///
    /// The buffer is *longer* than the stimulus, because a real recording keeps
    /// running after the stimulus stops. Truncating it to the stimulus length
    /// would throw away exactly the part that arrived late.
    fn delayed(source: &[f32], delay: usize, gain: f32) -> Vec<f32> {
        let mut out = vec![0.0; source.len() + delay];
        for (i, sample) in source.iter().enumerate() {
            out[i + delay] = sample * gain;
        }
        out
    }

    /// A deconvolver large enough for a stimulus of `LENGTH` plus a late tail.
    fn deconvolver() -> Deconvolver {
        Deconvolver::new(RATE, LENGTH + 8192)
    }

    /// The defining behaviour: a system that only delays must deconvolve to a
    /// single spike at that delay.
    #[test]
    fn a_pure_delay_deconvolves_to_a_spike() {
        let stimulus = sweep(LENGTH);
        for delay in [0_usize, 1, 64, 500, 3000] {
            let response = delayed(&stimulus, delay, 1.0);
            let mut deconvolver = deconvolver();
            let ir = deconvolver
                .deconvolve(&stimulus, &response, DEFAULT_REGULARISATION)
                .unwrap();

            assert!(
                (ir.peak_samples - delay as f32).abs() < 1.0,
                "delay {delay} peaked at {}",
                ir.peak_samples
            );
        }
    }

    /// And the spike must be a spike: energy concentrated, not smeared.
    #[test]
    fn the_recovered_impulse_is_concentrated() {
        let stimulus = sweep(LENGTH);
        let response = delayed(&stimulus, 100, 1.0);
        let mut deconvolver = deconvolver();
        let ir = deconvolver
            .deconvolve(&stimulus, &response, DEFAULT_REGULARISATION)
            .unwrap();

        let peak = ir.peak_amplitude();
        let centre = ir.peak_samples.round() as usize;

        // Almost all the energy should sit within a few samples of the arrival.
        let total: f32 = ir.samples.iter().map(|s| s * s).sum();
        let near: f32 = ir.samples[centre.saturating_sub(8)..(centre + 8).min(ir.samples.len())]
            .iter()
            .map(|s| s * s)
            .sum();
        assert!(
            near / total > 0.9,
            "only {:.1}% of energy is near the peak",
            100.0 * near / total
        );
        assert!(peak > 0.0);
    }

    #[test]
    fn a_gain_scales_the_impulse() {
        let stimulus = sweep(LENGTH);
        let mut deconvolver = deconvolver();

        let unity = deconvolver
            .deconvolve(
                &stimulus,
                &delayed(&stimulus, 50, 1.0),
                DEFAULT_REGULARISATION,
            )
            .unwrap()
            .peak_amplitude();
        let halved = deconvolver
            .deconvolve(
                &stimulus,
                &delayed(&stimulus, 50, 0.5),
                DEFAULT_REGULARISATION,
            )
            .unwrap()
            .peak_amplitude();

        let ratio = 20.0 * (halved / unity).log10();
        assert!((ratio + 6.0206).abs() < 0.3, "expected -6 dB, got {ratio}");
    }

    /// Several arrivals must all appear, in the right places and at the right
    /// relative levels - this is a room impulse response in miniature.
    #[test]
    fn multiple_reflections_all_appear() {
        let stimulus = sweep(LENGTH);
        let arrivals = [(200_usize, 1.0_f32), (450, 0.5), (900, 0.25)];

        let mut response = vec![0.0; stimulus.len() + 1024];
        for (delay, gain) in arrivals {
            for (i, sample) in stimulus.iter().enumerate() {
                response[i + delay] += sample * gain;
            }
        }

        let mut deconvolver = deconvolver();
        let ir = deconvolver
            .deconvolve(&stimulus, &response, DEFAULT_REGULARISATION)
            .unwrap();

        let peak = ir.peak_amplitude();
        for (delay, gain) in arrivals {
            let local = ir.samples[delay - 3..delay + 4]
                .iter()
                .fold(0.0_f32, |m, s| m.max(s.abs()));
            let relative = local / peak;
            assert!(
                (relative - gain).abs() < 0.08,
                "arrival at {delay} read {relative:.3}, expected {gain}"
            );
        }
    }

    /// Zero padding to twice the length is what stops the reverberant tail
    /// wrapping around onto the direct sound and looking like a pre-echo.
    #[test]
    fn the_transform_is_padded_against_circular_wrap() {
        let deconvolver = Deconvolver::new(RATE, 10_000);
        assert!(deconvolver.size() >= 20_000);
        assert!(deconvolver.size().is_power_of_two());
        assert!(deconvolver.max_length() >= 10_000);
    }

    #[test]
    fn a_late_arrival_does_not_wrap_onto_the_start() {
        let stimulus = sweep(4096);
        // An arrival at 3900 is almost a whole stimulus length late; with a
        // circular transform it would fold back onto the start.
        let response = delayed(&stimulus, 3900, 1.0);
        let mut deconvolver = Deconvolver::new(RATE, response.len());
        let ir = deconvolver
            .deconvolve(&stimulus, &response, DEFAULT_REGULARISATION)
            .unwrap();

        assert!(
            (ir.peak_samples - 3900.0).abs() < 2.0,
            "peaked at {} instead of 3900",
            ir.peak_samples
        );
    }

    /// Regularisation is the difference between a usable measurement and noise
    /// amplified into nonsense where the stimulus had no energy.
    #[test]
    fn regularisation_suppresses_noise_outside_the_sweep_band() {
        // A sweep that stops at 1 kHz leaves everything above it unexcited.
        let mut generator = Generator::new(
            RATE,
            Signal::Sweep {
                start_hz: 100.0,
                end_hz: 1000.0,
                seconds: LENGTH as f32 / RATE,
                amplitude: 0.5,
                repeat: false,
            },
            2,
        );
        let mut stimulus = vec![0.0; LENGTH];
        generator.fill(&mut stimulus);

        // Response is the stimulus plus broadband noise the stimulus cannot
        // explain above 1 kHz.
        let mut noise_gen = Generator::new(RATE, Signal::WhiteNoise { amplitude: 0.01 }, 3);
        let mut noise = vec![0.0; LENGTH];
        noise_gen.fill(&mut noise);
        let response: Vec<f32> = delayed(&stimulus, 100, 1.0)
            .iter()
            .zip(&noise)
            .map(|(s, n)| s + n)
            .collect();

        let mut deconvolver = deconvolver();
        let regularised = deconvolver
            .deconvolve(&stimulus, &response, DEFAULT_REGULARISATION)
            .unwrap();
        let barely = deconvolver.deconvolve(&stimulus, &response, 1e-12).unwrap();

        // With almost no regularisation the unexcited bands amplify noise, so
        // the peak stands out far less against the rest of the trace.
        let clarity = |ir: &ImpulseResponse| {
            let peak = ir.peak_amplitude();
            let mean = ir.samples.iter().map(|s| s.abs()).sum::<f32>() / ir.samples.len() as f32;
            peak / mean.max(1e-20)
        };
        // The improvement is real but modest - roughly 1.5x on this signal.
        // Asserting a bigger number would be fitting the test to one input.
        assert!(
            clarity(&regularised) > clarity(&barely) * 1.3,
            "regularisation should sharpen the impulse: {:.0} vs {:.0}",
            clarity(&regularised),
            clarity(&barely)
        );
    }

    #[test]
    fn silence_yields_nothing() {
        let mut deconvolver = deconvolver();
        let silence = vec![0.0; 1000];
        let stimulus = sweep(1000);

        assert!(
            deconvolver
                .deconvolve(&silence, &silence, DEFAULT_REGULARISATION)
                .is_none()
        );
        assert!(
            deconvolver
                .deconvolve(&[], &stimulus, DEFAULT_REGULARISATION)
                .is_none()
        );
        assert!(
            deconvolver
                .deconvolve(&stimulus, &[], DEFAULT_REGULARISATION)
                .is_none()
        );
    }

    #[test]
    fn oversized_input_is_refused_rather_than_truncated() {
        let mut deconvolver = Deconvolver::new(RATE, 1024);
        let long = vec![0.5; 4096];
        assert!(
            deconvolver
                .deconvolve(&long, &long, DEFAULT_REGULARISATION)
                .is_none(),
            "silently truncating would produce a plausible but wrong response"
        );
    }

    #[test]
    fn timing_helpers_are_relative_to_the_arrival() {
        let stimulus = sweep(LENGTH);
        let response = delayed(&stimulus, 480, 1.0);
        let mut deconvolver = deconvolver();
        let ir = deconvolver
            .deconvolve(&stimulus, &response, DEFAULT_REGULARISATION)
            .unwrap();

        // The arrival itself is t = 0, and 480 samples earlier is -10 ms.
        assert!(ir.time_at(480).abs() < 1e-4, "{}", ir.time_at(480));
        assert!((ir.time_at(0) + 0.01).abs() < 1e-4, "{}", ir.time_at(0));
        assert!(ir.duration_seconds() > 0.0);
    }

    /// Deconvolution should not care what the stimulus was, which is the whole
    /// reason for doing it this way rather than with a matched filter.
    #[test]
    fn noise_works_as_a_stimulus_too() {
        let mut generator = Generator::new(RATE, Signal::WhiteNoise { amplitude: 0.5 }, 4);
        let mut stimulus = vec![0.0; LENGTH];
        generator.fill(&mut stimulus);

        let response = delayed(&stimulus, 321, 1.0);
        let mut deconvolver = deconvolver();
        let ir = deconvolver
            .deconvolve(&stimulus, &response, DEFAULT_REGULARISATION)
            .unwrap();

        assert!(
            (ir.peak_samples - 321.0).abs() < 1.0,
            "peaked at {}",
            ir.peak_samples
        );
    }

    #[test]
    #[should_panic(expected = "max_length must be non-zero")]
    fn a_zero_length_deconvolver_is_rejected() {
        let _ = Deconvolver::new(RATE, 0);
    }
}
