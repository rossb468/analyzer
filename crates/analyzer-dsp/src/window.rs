//! Analysis windows and their correction factors.
//!
//! The FFT treats its input frame as one period of an infinitely repeating
//! signal. Unless the frame happens to contain a whole number of cycles, the
//! wrap-around point is a discontinuity, and a discontinuity is broadband — a
//! single clean tone smears across the whole spectrum. Tapering the frame to
//! zero at both ends removes the discontinuity, at the cost of attenuating the
//! signal and widening the response of each bin.
//!
//! Every window therefore carries two correction factors, and using the wrong
//! one skews every reading by a constant that is easy to miss for months:
//!
//! - [`Window::coherent_gain`] — the mean of the window. A sinusoid's amplitude
//!   is scaled by this, so recovering absolute level needs
//!   [`Window::amplitude_correction`].
//! - [`Window::enbw_bins`] — equivalent noise bandwidth, in bins. Broadband
//!   signals spread over more than one bin's nominal width, so power spectral
//!   density has to be divided by this.

use std::f32::consts::PI;

/// Coefficients of a generalised cosine window, `a₀ … aₙ`.
///
/// The window is `w[n] = Σₖ (-1)ᵏ aₖ cos(2πkn/N)`, evaluated periodically —
/// denominator `N`, not `N - 1`. Periodic is the correct choice for spectral
/// analysis; the symmetric variant is for filter design.
type Cosine = &'static [f32];

const HANN: Cosine = &[0.5, 0.5];

/// Four-term Blackman-Harris. Very low sidelobes at the cost of a wider main
/// lobe, so better dynamic range and worse frequency resolution than Hann.
const BLACKMAN_HARRIS: Cosine = &[0.35875, 0.48829, 0.14128, 0.01168];

/// Five-term flat-top. Deliberately poor frequency resolution in exchange for a
/// very flat main lobe, which makes amplitude accurate regardless of where a
/// tone falls between bins. This is the window to calibrate with.
const FLAT_TOP: Cosine = &[
    0.215_578_95,
    0.416_631_58,
    0.277_263_16,
    0.083_578_95,
    0.006_947_368,
];

/// Which taper to apply to an analysis frame.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum WindowKind {
    /// No taper. Correct only when the frame contains whole cycles, which in
    /// practice means synthetic test signals.
    Rectangular,
    /// General-purpose default: a reasonable balance of resolution and leakage.
    Hann,
    /// Low leakage, wider main lobe. For resolving small signals near large ones.
    BlackmanHarris,
    /// Flat main lobe for accurate amplitude. For calibration.
    FlatTop,
    /// Tapered cosine. `alpha` is the fraction of the frame that is tapered:
    /// `0.0` is [`WindowKind::Rectangular`], `1.0` is [`WindowKind::Hann`].
    Tukey { alpha: f32 },
}

/// A precomputed analysis window with its correction factors.
#[derive(Debug, Clone)]
pub struct Window {
    kind: WindowKind,
    samples: Vec<f32>,
    coherent_gain: f32,
    enbw_bins: f32,
}

impl Window {
    /// Build a window of `size` samples.
    ///
    /// # Panics
    ///
    /// Panics if `size` is zero.
    pub fn new(kind: WindowKind, size: usize) -> Self {
        assert!(size > 0, "window size must be non-zero");

        let mut samples = vec![0.0_f32; size];
        match kind {
            WindowKind::Rectangular => samples.fill(1.0),
            WindowKind::Hann => fill_cosine(&mut samples, HANN),
            WindowKind::BlackmanHarris => fill_cosine(&mut samples, BLACKMAN_HARRIS),
            WindowKind::FlatTop => fill_cosine(&mut samples, FLAT_TOP),
            WindowKind::Tukey { alpha } => fill_tukey(&mut samples, alpha),
        }

        // Derive the factors from the samples rather than tabulating them, so
        // they cannot drift out of step with the coefficients above. The tests
        // check these against published values.
        let n = size as f32;
        let sum: f32 = samples.iter().sum();
        let sum_sq: f32 = samples.iter().map(|w| w * w).sum();

        let coherent_gain = sum / n;
        let enbw_bins = if sum == 0.0 {
            0.0
        } else {
            n * sum_sq / (sum * sum)
        };

        Self {
            kind,
            samples,
            coherent_gain,
            enbw_bins,
        }
    }

    /// Which taper this is.
    pub fn kind(&self) -> WindowKind {
        self.kind
    }

    /// Frame length in samples.
    pub fn size(&self) -> usize {
        self.samples.len()
    }

    /// The window coefficients.
    pub fn samples(&self) -> &[f32] {
        &self.samples
    }

    /// Mean of the window. A sinusoid passed through it comes out scaled by this.
    pub fn coherent_gain(&self) -> f32 {
        self.coherent_gain
    }

    /// Reciprocal of [`Window::coherent_gain`] — multiply a measured sinusoid
    /// amplitude by this to recover its true level.
    pub fn amplitude_correction(&self) -> f32 {
        if self.coherent_gain == 0.0 {
            0.0
        } else {
            1.0 / self.coherent_gain
        }
    }

    /// Equivalent noise bandwidth in bins: how many bins' worth of noise each
    /// bin actually collects. Divide power spectral density by this.
    pub fn enbw_bins(&self) -> f32 {
        self.enbw_bins
    }

    /// Apply the window in place.
    ///
    /// # Panics
    ///
    /// Panics if `frame` is not exactly [`Window::size`] samples.
    pub fn apply(&self, frame: &mut [f32]) {
        assert_eq!(
            frame.len(),
            self.size(),
            "frame length must equal the window size"
        );
        for (x, w) in frame.iter_mut().zip(&self.samples) {
            *x *= w;
        }
    }

    /// Apply the window, writing into `output` and leaving `input` untouched.
    ///
    /// # Panics
    ///
    /// Panics if either slice is not exactly [`Window::size`] samples.
    pub fn apply_to(&self, input: &[f32], output: &mut [f32]) {
        assert_eq!(
            input.len(),
            self.size(),
            "input length must equal window size"
        );
        assert_eq!(
            output.len(),
            self.size(),
            "output length must equal window size"
        );
        for ((out, x), w) in output.iter_mut().zip(input).zip(&self.samples) {
            *out = x * w;
        }
    }
}

fn fill_cosine(samples: &mut [f32], coeffs: Cosine) {
    let n = samples.len() as f32;
    for (i, w) in samples.iter_mut().enumerate() {
        let phase = 2.0 * PI * i as f32 / n;
        *w = coeffs
            .iter()
            .enumerate()
            .map(|(k, a)| {
                let sign = if k.is_multiple_of(2) { 1.0 } else { -1.0 };
                sign * a * (k as f32 * phase).cos()
            })
            .sum();
    }
}

fn fill_tukey(samples: &mut [f32], alpha: f32) {
    let alpha = alpha.clamp(0.0, 1.0);
    let n = samples.len() as f32;
    let taper = alpha * n / 2.0;

    for (i, w) in samples.iter_mut().enumerate() {
        let i = i as f32;
        *w = if taper > 0.0 && i < taper {
            0.5 * (1.0 - (PI * i / taper).cos())
        } else if taper > 0.0 && i > n - taper {
            0.5 * (1.0 - (PI * (n - i) / taper).cos())
        } else {
            1.0
        };
    }
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;

    const SIZE: usize = 4096;

    /// Published coherent gain and equivalent noise bandwidth. Getting these
    /// wrong offsets every level reading by a constant, which is exactly the
    /// kind of error that survives casual inspection.
    #[test]
    fn correction_factors_match_published_values() {
        let cases = [
            (WindowKind::Rectangular, 1.0, 1.0),
            (WindowKind::Hann, 0.5, 1.5),
            (WindowKind::BlackmanHarris, 0.35875, 2.0044),
            (WindowKind::FlatTop, 0.215_578_95, 3.7702),
        ];

        for (kind, cg, enbw) in cases {
            let w = Window::new(kind, SIZE);
            assert!(
                (w.coherent_gain() - cg).abs() < 1e-5,
                "{kind:?} coherent gain: got {}, want {cg}",
                w.coherent_gain()
            );
            assert!(
                (w.enbw_bins() - enbw).abs() < 1e-3,
                "{kind:?} ENBW: got {}, want {enbw}",
                w.enbw_bins()
            );
        }
    }

    #[test]
    fn amplitude_correction_is_reciprocal_of_coherent_gain() {
        let w = Window::new(WindowKind::Hann, SIZE);
        assert!((w.amplitude_correction() - 2.0).abs() < 1e-5);
    }

    #[test]
    fn cosine_windows_peak_at_unity_mid_frame() {
        for kind in [
            WindowKind::Hann,
            WindowKind::BlackmanHarris,
            WindowKind::FlatTop,
        ] {
            let w = Window::new(kind, SIZE);
            let peak = w.samples()[SIZE / 2];
            assert!((peak - 1.0).abs() < 1e-5, "{kind:?} peak = {peak}");
        }
    }

    #[test]
    fn cosine_windows_start_near_zero() {
        // Flat-top dips slightly negative at the edges; that is correct.
        assert!(Window::new(WindowKind::Hann, SIZE).samples()[0].abs() < 1e-6);
        assert!(Window::new(WindowKind::BlackmanHarris, SIZE).samples()[0].abs() < 1e-3);
        assert!(Window::new(WindowKind::FlatTop, SIZE).samples()[0].abs() < 1e-3);
    }

    /// A periodic window is symmetric about its midpoint for `n = 1..N`, with
    /// index 0 being the unmatched sample. Asymmetry here means the periodic and
    /// symmetric definitions have been mixed up.
    #[test]
    fn periodic_windows_are_symmetric_excluding_index_zero() {
        let w = Window::new(WindowKind::Hann, SIZE);
        let s = w.samples();
        for n in 1..SIZE / 2 {
            assert!(
                (s[n] - s[SIZE - n]).abs() < 1e-6,
                "asymmetry at {n}: {} vs {}",
                s[n],
                s[SIZE - n]
            );
        }
    }

    #[test]
    fn tukey_degenerates_to_rectangular_and_hann() {
        let rect = Window::new(WindowKind::Rectangular, SIZE);
        let tukey_0 = Window::new(WindowKind::Tukey { alpha: 0.0 }, SIZE);
        for (a, b) in tukey_0.samples().iter().zip(rect.samples()) {
            assert!((a - b).abs() < 1e-6);
        }

        let hann = Window::new(WindowKind::Hann, SIZE);
        let tukey_1 = Window::new(WindowKind::Tukey { alpha: 1.0 }, SIZE);
        for (i, (a, b)) in tukey_1.samples().iter().zip(hann.samples()).enumerate() {
            assert!((a - b).abs() < 1e-5, "differ at {i}: {a} vs {b}");
        }
    }

    #[test]
    fn tukey_alpha_is_clamped() {
        let low = Window::new(WindowKind::Tukey { alpha: -5.0 }, 64);
        assert!(low.samples().iter().all(|w| (w - 1.0).abs() < 1e-6));

        let high = Window::new(WindowKind::Tukey { alpha: 5.0 }, 64);
        let hann = Window::new(WindowKind::Hann, 64);
        for (a, b) in high.samples().iter().zip(hann.samples()) {
            assert!((a - b).abs() < 1e-5);
        }
    }

    #[test]
    fn apply_scales_in_place() {
        let w = Window::new(WindowKind::Hann, 8);
        let mut frame = vec![2.0_f32; 8];
        w.apply(&mut frame);
        for (got, want) in frame.iter().zip(w.samples()) {
            assert!((got - 2.0 * want).abs() < 1e-6);
        }
    }

    #[test]
    fn apply_to_leaves_input_alone() {
        let w = Window::new(WindowKind::Hann, 8);
        let input = vec![2.0_f32; 8];
        let mut output = vec![0.0_f32; 8];
        w.apply_to(&input, &mut output);
        assert!(input.iter().all(|x| *x == 2.0));
        for (got, want) in output.iter().zip(w.samples()) {
            assert!((got - 2.0 * want).abs() < 1e-6);
        }
    }

    #[test]
    #[should_panic(expected = "frame length must equal")]
    fn apply_rejects_wrong_length() {
        Window::new(WindowKind::Hann, 8).apply(&mut [0.0; 7]);
    }

    #[test]
    #[should_panic(expected = "window size must be non-zero")]
    fn zero_size_is_rejected() {
        let _ = Window::new(WindowKind::Hann, 0);
    }
}
