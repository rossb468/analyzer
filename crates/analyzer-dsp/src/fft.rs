//! Real-to-complex forward FFT behind a trait.
//!
//! The trait exists for two reasons. It keeps a vDSP-backed implementation
//! possible on Apple platforms without leaking Accelerate into the rest of the
//! core, and it keeps the non-Apple port open. Do not call platform FFT
//! libraries from anywhere but an implementation of [`Fft`].

use std::fmt;
use std::sync::Arc;

use realfft::{RealFftPlanner, RealToComplex};
use rustfft::num_complex::Complex32;

/// A real-to-complex forward FFT of a fixed size.
///
/// # Real-time contract
///
/// [`Fft::forward`] must not allocate. The analysis thread runs it on every hop
/// — several hundred times a second — and an allocation there is a latency
/// hazard with an unbounded worst case. Implementations preallocate all scratch
/// space at construction.
pub trait Fft: Send {
    /// Number of real input samples consumed per transform.
    fn size(&self) -> usize;

    /// Number of complex bins produced, `size() / 2 + 1`.
    ///
    /// The transform is one-sided: bin 0 is DC and bin `size() / 2` is Nyquist,
    /// both of which are purely real for real input.
    fn bins(&self) -> usize {
        self.size() / 2 + 1
    }

    /// Transform `input` into `output`.
    ///
    /// `input` must be exactly [`Fft::size`] samples and `output` exactly
    /// [`Fft::bins`] bins. The transform is unnormalised: a full-scale DC input
    /// produces `size()` in bin 0.
    ///
    /// # Panics
    ///
    /// Panics if either buffer length is wrong. That is a programmer error
    /// rather than a runtime condition, so it fails loudly in tests instead of
    /// being absorbed into a `Result` that the real-time path would have to
    /// handle on every hop.
    fn forward(&mut self, input: &[f32], output: &mut [Complex32]);
}

/// Portable [`Fft`] implementation over `realfft`/`rustfft`.
pub struct RealFft {
    plan: Arc<dyn RealToComplex<f32>>,
    /// `realfft` uses its input buffer as scratch, so we transform a copy and
    /// leave the caller's slice untouched.
    input: Vec<f32>,
    scratch: Vec<Complex32>,
}

impl RealFft {
    /// Plan a transform of `size` real samples.
    ///
    /// # Panics
    ///
    /// Panics if `size` is odd or less than two. A one-sided real transform is
    /// not meaningful otherwise.
    pub fn new(size: usize) -> Self {
        assert!(
            size >= 2 && size.is_multiple_of(2),
            "FFT size must be even and at least 2, got {size}"
        );
        let plan = RealFftPlanner::<f32>::new().plan_fft_forward(size);
        let input = plan.make_input_vec();
        let scratch = plan.make_scratch_vec();
        Self {
            plan,
            input,
            scratch,
        }
    }
}

impl Fft for RealFft {
    fn size(&self) -> usize {
        self.input.len()
    }

    fn forward(&mut self, input: &[f32], output: &mut [Complex32]) {
        assert_eq!(
            input.len(),
            self.size(),
            "input length must equal the planned FFT size"
        );
        assert_eq!(
            output.len(),
            self.bins(),
            "output length must equal size / 2 + 1"
        );

        self.input.copy_from_slice(input);
        self.plan
            .process_with_scratch(&mut self.input, output, &mut self.scratch)
            .expect("buffer lengths asserted above");
    }
}

impl fmt::Debug for RealFft {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("RealFft")
            .field("size", &self.size())
            .field("bins", &self.bins())
            .finish_non_exhaustive()
    }
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;
    use std::f32::consts::TAU;

    const SIZE: usize = 4096;

    fn plan() -> (RealFft, Vec<Complex32>) {
        let fft = RealFft::new(SIZE);
        let out = vec![Complex32::default(); fft.bins()];
        (fft, out)
    }

    #[test]
    fn bins_is_half_size_plus_one() {
        let fft = RealFft::new(1024);
        assert_eq!(fft.size(), 1024);
        assert_eq!(fft.bins(), 513);
    }

    #[test]
    fn dc_input_lands_entirely_in_bin_zero() {
        let (mut fft, mut out) = plan();
        let input = vec![1.0_f32; SIZE];

        fft.forward(&input, &mut out);

        // Unnormalised transform: full-scale DC gives `size` in bin 0.
        assert!((out[0].re - SIZE as f32).abs() < 1e-2, "bin 0 = {}", out[0]);
        assert!(out[0].im.abs() < 1e-3);
        for (k, bin) in out.iter().enumerate().skip(1) {
            assert!(bin.norm() < 1e-2, "bin {k} should be empty, got {bin}");
        }
    }

    #[test]
    fn bin_centred_sine_recovers_its_amplitude() {
        let (mut fft, mut out) = plan();
        let bin = 64_usize;
        let amplitude = 0.5_f32;

        // Exactly `bin` whole cycles across the frame, so all energy lands in
        // one bin and there is nothing to leak.
        let input: Vec<f32> = (0..SIZE)
            .map(|n| amplitude * (TAU * bin as f32 * n as f32 / SIZE as f32).sin())
            .collect();

        fft.forward(&input, &mut out);

        let peak = out
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.norm().total_cmp(&b.1.norm()))
            .map(|(k, _)| k)
            .unwrap();
        assert_eq!(peak, bin);

        // One-sided amplitude for 0 < k < size/2 is 2|X[k]| / size.
        let recovered = 2.0 * out[bin].norm() / SIZE as f32;
        assert!(
            (recovered - amplitude).abs() < 1e-3,
            "recovered {recovered}, expected {amplitude}"
        );
    }

    #[test]
    fn parseval_energy_is_conserved() {
        let (mut fft, mut out) = plan();
        // Two incommensurate tones plus DC, so the check is not accidentally
        // passing on a single clean bin.
        let input: Vec<f32> = (0..SIZE)
            .map(|n| {
                let t = n as f32 / SIZE as f32;
                0.1 + 0.3 * (TAU * 37.0 * t).sin() + 0.2 * (TAU * 211.0 * t).cos()
            })
            .collect();

        fft.forward(&input, &mut out);

        let time_energy: f32 = input.iter().map(|x| x * x).sum();

        // For real input the one-sided spectrum double-counts everything except
        // DC and Nyquist.
        let last = out.len() - 1;
        let mut spectral = out[0].norm_sqr() + out[last].norm_sqr();
        spectral += 2.0 * out[1..last].iter().map(Complex32::norm_sqr).sum::<f32>();
        spectral /= SIZE as f32;

        let error = (time_energy - spectral).abs() / time_energy;
        assert!(
            error < 1e-4,
            "time {time_energy}, spectral {spectral}, relative error {error}"
        );
    }

    #[test]
    fn input_slice_is_not_modified() {
        let (mut fft, mut out) = plan();
        let input: Vec<f32> = (0..SIZE).map(|n| (n as f32 * 0.01).sin()).collect();
        let before = input.clone();

        fft.forward(&input, &mut out);

        assert_eq!(input, before, "forward must not consume the caller's input");
    }

    #[test]
    #[should_panic(expected = "input length must equal")]
    fn wrong_input_length_panics() {
        let (mut fft, mut out) = plan();
        fft.forward(&vec![0.0; SIZE - 1], &mut out);
    }

    #[test]
    #[should_panic(expected = "FFT size must be even")]
    fn odd_size_is_rejected() {
        let _ = RealFft::new(1023);
    }
}
