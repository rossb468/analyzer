//! Overlapped power spectrum estimation with averaging.
//!
//! A single FFT of anything noise-like is a very noisy estimate. Welch's method
//! averages the power spectra of many overlapped frames, which trades time
//! resolution for variance. Overlapping is close to free: the window is already
//! attenuating the frame edges, so reusing samples recovers the information the
//! taper threw away and yields more averages per second.
//!
//! # Level conventions
//!
//! [`SpectrumAnalyzer::power`] is **mean-square power per bin**, scaled so that
//! summing every bin of an unwindowed frame gives the mean square of the input
//! (see the Parseval test). A bin-centred sinusoid of amplitude `A` reads
//! `A² / 2` in its peak bin, for every window, because each window's correction
//! factor is derived from that same window's DC gain.
//!
//! [`SpectrumAnalyzer::write_db_fs`] uses **0 dBFS = full-scale sine**, the
//! usual convention for audio analysers. A sine of amplitude 1.0 therefore reads
//! 0.0 dBFS, not +3.

use crate::fft::{Fft, RealFft};
use crate::window::{Window, WindowKind};

/// Floor applied by [`SpectrumAnalyzer::write_db_fs`] so that empty bins produce
/// a finite number instead of negative infinity.
pub const DB_FLOOR: f32 = -200.0;

/// Fraction of each frame reused by the next one.
///
/// More overlap means more averages per second and a smoother display, at
/// proportionally more CPU. 75% is the usual default for real-time work.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Overlap {
    /// Frames do not overlap; hop equals the frame size.
    None,
    /// 50% overlap.
    Half,
    #[default]
    /// 75% overlap.
    ThreeQuarters,
    /// 87.5% overlap.
    SevenEighths,
}

impl Overlap {
    /// Samples advanced between consecutive frames.
    pub fn hop(self, size: usize) -> usize {
        let hop = match self {
            Overlap::None => size,
            Overlap::Half => size / 2,
            Overlap::ThreeQuarters => size / 4,
            Overlap::SevenEighths => size / 8,
        };
        hop.max(1)
    }

    /// The overlap as a fraction of the frame, for display.
    pub fn fraction(self) -> f32 {
        match self {
            Overlap::None => 0.0,
            Overlap::Half => 0.5,
            Overlap::ThreeQuarters => 0.75,
            Overlap::SevenEighths => 0.875,
        }
    }
}

/// How successive frames are combined.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub enum Averaging {
    /// Each frame replaces the previous one. Noisy, but maximally responsive.
    #[default]
    None,
    /// Exponential moving average: `avg = alpha * new + (1 - alpha) * avg`.
    ///
    /// Smaller `alpha` is smoother and slower. Construct from a time constant
    /// with [`Averaging::exponential_over`].
    Exponential { alpha: f32 },
    /// Average the first `frames` frames, then hold.
    ///
    /// This is the "10 averages and stop" behaviour measurement tools use, not a
    /// sliding window — a true sliding average would need to retain every frame.
    Linear { frames: u32 },
    /// Average every frame since the last reset. Variance keeps falling, so this
    /// is the mode for a static measurement where precision matters most.
    Infinite,
    /// Retain the maximum seen in each bin since the last reset.
    PeakHold,
}

impl Averaging {
    /// Exponential averaging with a time constant of `seconds`, given the rate
    /// at which frames arrive.
    ///
    /// Returns [`Averaging::None`] for a non-positive time constant, since that
    /// is what a zero-length average means.
    pub fn exponential_over(seconds: f32, frames_per_second: f32) -> Self {
        if seconds <= 0.0 || frames_per_second <= 0.0 {
            return Averaging::None;
        }
        let alpha = 1.0 - (-1.0 / (seconds * frames_per_second)).exp();
        Averaging::Exponential {
            alpha: alpha.clamp(f32::MIN_POSITIVE, 1.0),
        }
    }
}

/// How to set up a [`SpectrumAnalyzer`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SpectrumConfig {
    /// Sample rate of the incoming audio, in hertz.
    pub sample_rate: f32,
    /// FFT size in samples. Must be even and at least two.
    ///
    /// This is the resolution/response tradeoff: bin spacing is
    /// `sample_rate / size`, but the frame also spans `size / sample_rate`
    /// seconds of time that get averaged together.
    pub size: usize,
    /// Analysis taper.
    pub window: WindowKind,
    /// Frame overlap.
    pub overlap: Overlap,
    /// Averaging mode.
    pub averaging: Averaging,
}

impl Default for SpectrumConfig {
    fn default() -> Self {
        Self {
            sample_rate: 48_000.0,
            size: 4096,
            window: WindowKind::Hann,
            overlap: Overlap::ThreeQuarters,
            averaging: Averaging::None,
        }
    }
}

/// Welch-style overlapped spectrum estimator.
///
/// Feed it samples with [`SpectrumAnalyzer::push`]; it emits a frame every hop
/// and folds each into the running average. All buffers are sized at
/// construction, so `push` does not allocate.
#[derive(Debug)]
pub struct SpectrumAnalyzer {
    sample_rate: f32,
    fft: RealFft,
    window: Window,
    hop: usize,
    averaging: Averaging,

    /// Sliding frame, `window.size()` long. `filled` samples are valid.
    frame: Vec<f32>,
    filled: usize,

    windowed: Vec<f32>,
    spectrum: Vec<crate::Complex32>,
    /// Power of the frame currently being processed.
    frame_power: Vec<f32>,
    /// The running average exposed to callers.
    power: Vec<f32>,
    frames: u32,

    /// `1 / (size * coherent_gain)`, precomputed.
    amplitude_scale: f32,
}

impl SpectrumAnalyzer {
    /// Build an analyzer.
    ///
    /// # Panics
    ///
    /// Panics if `config.size` is odd or less than two, or if `sample_rate` is
    /// not positive.
    pub fn new(config: SpectrumConfig) -> Self {
        assert!(
            config.sample_rate > 0.0,
            "sample rate must be positive, got {}",
            config.sample_rate
        );

        let fft = RealFft::new(config.size);
        let window = Window::new(config.window, config.size);
        let hop = config.overlap.hop(config.size);
        let bins = fft.bins();
        let amplitude_scale = 1.0 / (config.size as f32 * window.coherent_gain());

        Self {
            sample_rate: config.sample_rate,
            fft,
            window,
            hop,
            averaging: config.averaging,
            frame: vec![0.0; config.size],
            filled: 0,
            windowed: vec![0.0; config.size],
            spectrum: vec![crate::Complex32::default(); bins],
            frame_power: vec![0.0; bins],
            power: vec![0.0; bins],
            frames: 0,
            amplitude_scale,
        }
    }

    /// Feed samples in, returning how many frames completed.
    ///
    /// Accepts any length; partial frames are retained until the next call, so
    /// the result is identical whether audio arrives in one block or many.
    pub fn push(&mut self, samples: &[f32]) -> usize {
        let size = self.frame.len();
        let mut produced = 0;
        let mut remaining = samples;

        while !remaining.is_empty() {
            let wanted = size - self.filled;
            let taken = wanted.min(remaining.len());

            let (head, tail) = remaining.split_at(taken);
            if let Some(dst) = self.frame.get_mut(self.filled..self.filled + taken) {
                dst.copy_from_slice(head);
            }
            self.filled += taken;
            remaining = tail;

            if self.filled == size {
                self.process_frame();
                produced += 1;

                // Slide by one hop, keeping the overlapping tail.
                self.frame.copy_within(self.hop.., 0);
                self.filled = size - self.hop;
            }
        }

        produced
    }

    fn process_frame(&mut self) {
        self.window.apply_to(&self.frame, &mut self.windowed);
        self.fft.forward(&self.windowed, &mut self.spectrum);

        // Convert to mean-square power per bin. DC and Nyquist are real and
        // unpaired; every bin between them stands for a conjugate pair, so its
        // amplitude is doubled and its mean square is halved: 2 * |X * s|^2.
        let last = self.spectrum.len().saturating_sub(1);
        let scale = self.amplitude_scale;
        for (k, (out, bin)) in self
            .frame_power
            .iter_mut()
            .zip(self.spectrum.iter())
            .enumerate()
        {
            let magnitude = bin.norm() * scale;
            *out = if k == 0 || k == last {
                magnitude * magnitude
            } else {
                2.0 * magnitude * magnitude
            };
        }

        self.accumulate();
        self.frames = self.frames.saturating_add(1);
    }

    fn accumulate(&mut self) {
        let first = self.frames == 0;

        match self.averaging {
            Averaging::None => self.power.copy_from_slice(&self.frame_power),

            Averaging::Exponential { alpha } => {
                if first {
                    self.power.copy_from_slice(&self.frame_power);
                } else {
                    let alpha = alpha.clamp(0.0, 1.0);
                    for (avg, new) in self.power.iter_mut().zip(&self.frame_power) {
                        *avg += alpha * (new - *avg);
                    }
                }
            }

            // Incremental mean, which avoids a second accumulator buffer and
            // stays numerically stable as the count grows.
            Averaging::Infinite => {
                let n = self.frames as f32 + 1.0;
                for (avg, new) in self.power.iter_mut().zip(&self.frame_power) {
                    *avg += (new - *avg) / n;
                }
            }

            Averaging::Linear { frames } => {
                if self.frames < frames {
                    let n = self.frames as f32 + 1.0;
                    for (avg, new) in self.power.iter_mut().zip(&self.frame_power) {
                        *avg += (new - *avg) / n;
                    }
                }
                // Otherwise hold: the average is complete.
            }

            Averaging::PeakHold => {
                if first {
                    self.power.copy_from_slice(&self.frame_power);
                } else {
                    for (peak, new) in self.power.iter_mut().zip(&self.frame_power) {
                        *peak = peak.max(*new);
                    }
                }
            }
        }
    }

    /// Mean-square power per bin. See the module docs for the exact convention.
    pub fn power(&self) -> &[f32] {
        &self.power
    }

    /// Write the spectrum as dBFS, where 0 dBFS is a full-scale sine.
    ///
    /// # Panics
    ///
    /// Panics if `out` is not exactly [`SpectrumAnalyzer::bins`] long.
    pub fn write_db_fs(&self, out: &mut [f32]) {
        assert_eq!(out.len(), self.power.len(), "output must be one per bin");
        for (db, power) in out.iter_mut().zip(&self.power) {
            // Full-scale sine has mean square 0.5, so 2 * power normalises it
            // to unity at 0 dBFS.
            *db = if *power > 0.0 {
                (10.0 * (2.0 * power).log10()).max(DB_FLOOR)
            } else {
                DB_FLOOR
            };
        }
    }

    /// Discard the running average and the partial frame.
    pub fn reset(&mut self) {
        self.power.fill(0.0);
        self.frame.fill(0.0);
        self.filled = 0;
        self.frames = 0;
    }

    /// Change averaging mode. Resets the average, since mixing modes within one
    /// accumulation would produce a number that means nothing.
    pub fn set_averaging(&mut self, averaging: Averaging) {
        self.averaging = averaging;
        self.power.fill(0.0);
        self.frames = 0;
    }

    /// Frames folded into the current average.
    pub fn frames(&self) -> u32 {
        self.frames
    }

    /// Number of bins, `size / 2 + 1`.
    pub fn bins(&self) -> usize {
        self.power.len()
    }

    /// FFT size in samples.
    pub fn size(&self) -> usize {
        self.frame.len()
    }

    /// Samples between consecutive frames.
    pub fn hop(&self) -> usize {
        self.hop
    }

    /// Frames produced per second of audio at the configured rate.
    pub fn frame_rate(&self) -> f32 {
        self.sample_rate / self.hop as f32
    }

    /// Width of one bin in hertz.
    pub fn bin_spacing_hz(&self) -> f32 {
        self.sample_rate / self.size() as f32
    }

    /// Centre frequency of bin `k`.
    pub fn bin_frequency(&self, k: usize) -> f32 {
        k as f32 * self.bin_spacing_hz()
    }

    /// Effective noise bandwidth of each bin in hertz. Divide power by this to
    /// get power spectral density.
    pub fn enbw_hz(&self) -> f32 {
        self.window.enbw_bins() * self.bin_spacing_hz()
    }

    /// The window in use.
    pub fn window(&self) -> &Window {
        &self.window
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use std::f32::consts::TAU;

    const RATE: f32 = 48_000.0;
    const SIZE: usize = 4096;

    fn sine(bin: usize, amplitude: f32, len: usize) -> Vec<f32> {
        (0..len)
            .map(|n| amplitude * (TAU * bin as f32 * n as f32 / SIZE as f32).sin())
            .collect()
    }

    fn analyzer(window: WindowKind, averaging: Averaging) -> SpectrumAnalyzer {
        SpectrumAnalyzer::new(SpectrumConfig {
            sample_rate: RATE,
            size: SIZE,
            window,
            overlap: Overlap::None,
            averaging,
        })
    }

    fn peak_bin(power: &[f32]) -> usize {
        power
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.total_cmp(b.1))
            .map(|(k, _)| k)
            .unwrap()
    }

    #[test]
    fn hop_follows_overlap() {
        assert_eq!(Overlap::None.hop(4096), 4096);
        assert_eq!(Overlap::Half.hop(4096), 2048);
        assert_eq!(Overlap::ThreeQuarters.hop(4096), 1024);
        assert_eq!(Overlap::SevenEighths.hop(4096), 512);
        // Never zero, however small the frame.
        assert_eq!(Overlap::SevenEighths.hop(4), 1);
    }

    #[test]
    fn full_scale_sine_reads_zero_db_fs() {
        let mut a = analyzer(WindowKind::Hann, Averaging::None);
        a.push(&sine(64, 1.0, SIZE));

        let mut db = vec![0.0; a.bins()];
        a.write_db_fs(&mut db);

        let peak = peak_bin(a.power());
        assert_eq!(peak, 64);
        assert!(
            db.get(peak).unwrap().abs() < 0.01,
            "expected 0 dBFS, got {}",
            db[peak]
        );
    }

    #[test]
    fn half_amplitude_sine_reads_minus_six_db() {
        let mut a = analyzer(WindowKind::Hann, Averaging::None);
        a.push(&sine(64, 0.5, SIZE));

        let mut db = vec![0.0; a.bins()];
        a.write_db_fs(&mut db);

        let level = *db.get(peak_bin(a.power())).unwrap();
        assert!(
            (level + 6.0206).abs() < 0.01,
            "expected -6.02 dBFS, got {level}"
        );
    }

    /// The end-to-end check on the window correction factors: the same tone must
    /// read the same level through every window. This is what catches a wrong
    /// coherent gain, which otherwise just offsets everything by a constant.
    #[test]
    fn measured_level_is_independent_of_window() {
        for window in [
            WindowKind::Rectangular,
            WindowKind::Hann,
            WindowKind::BlackmanHarris,
            WindowKind::FlatTop,
            WindowKind::Tukey { alpha: 0.25 },
        ] {
            let mut a = analyzer(window, Averaging::None);
            a.push(&sine(64, 0.5, SIZE));

            let mut db = vec![0.0; a.bins()];
            a.write_db_fs(&mut db);
            let level = *db.get(64).unwrap();

            assert!(
                (level + 6.0206).abs() < 0.05,
                "{window:?} read {level} dBFS, expected -6.02"
            );
        }
    }

    /// With no taper the power spectrum must sum to the mean square of the input.
    #[test]
    fn unwindowed_power_sums_to_mean_square() {
        let mut a = analyzer(WindowKind::Rectangular, Averaging::None);
        let input: Vec<f32> = (0..SIZE)
            .map(|n| {
                let t = n as f32 / SIZE as f32;
                0.1 + 0.3 * (TAU * 37.0 * t).sin() + 0.2 * (TAU * 211.0 * t).cos()
            })
            .collect();

        a.push(&input);

        let mean_square: f32 = input.iter().map(|x| x * x).sum::<f32>() / SIZE as f32;
        let total: f32 = a.power().iter().sum();
        let error = (mean_square - total).abs() / mean_square;
        assert!(
            error < 1e-4,
            "mean square {mean_square}, spectral total {total}, error {error}"
        );
    }

    #[test]
    fn frames_are_counted_per_hop() {
        let mut a = SpectrumAnalyzer::new(SpectrumConfig {
            sample_rate: RATE,
            size: SIZE,
            window: WindowKind::Hann,
            overlap: Overlap::ThreeQuarters,
            averaging: Averaging::None,
        });
        assert_eq!(a.hop(), SIZE / 4);

        // First frame needs a full buffer; each later frame needs one hop.
        assert_eq!(a.push(&vec![0.0; SIZE]), 1);
        assert_eq!(a.push(&vec![0.0; SIZE / 4]), 1);
        assert_eq!(a.push(&vec![0.0; SIZE]), 4);
    }

    /// Chunking must not change the answer: the audio driver's buffer size is
    /// not the analyzer's business.
    #[test]
    fn streaming_in_chunks_matches_one_block() {
        let input = sine(97, 0.3, SIZE * 4);

        let mut whole = analyzer(WindowKind::Hann, Averaging::Infinite);
        whole.push(&input);

        let mut chunked = analyzer(WindowKind::Hann, Averaging::Infinite);
        for chunk in input.chunks(113) {
            chunked.push(chunk);
        }

        assert_eq!(whole.frames(), chunked.frames());
        for (k, (a, b)) in whole.power().iter().zip(chunked.power()).enumerate() {
            assert!((a - b).abs() < 1e-9, "bin {k}: {a} vs {b}");
        }
    }

    #[test]
    fn peak_hold_retains_the_maximum() {
        let mut a = analyzer(WindowKind::Hann, Averaging::PeakHold);
        a.push(&sine(64, 1.0, SIZE));
        let loud = *a.power().get(64).unwrap();

        a.push(&sine(64, 0.01, SIZE));
        assert!(
            (a.power().get(64).unwrap() - loud).abs() < 1e-9,
            "peak hold must not decay"
        );
    }

    #[test]
    fn linear_averaging_holds_after_its_frame_count() {
        let mut a = analyzer(WindowKind::Hann, Averaging::Linear { frames: 2 });
        a.push(&sine(64, 1.0, SIZE));
        a.push(&sine(64, 1.0, SIZE));
        let held = *a.power().get(64).unwrap();
        assert_eq!(a.frames(), 2);

        // Further frames are ignored once the average is complete.
        a.push(&sine(64, 0.001, SIZE));
        assert!((a.power().get(64).unwrap() - held).abs() < 1e-9);
    }

    #[test]
    fn exponential_averaging_converges_towards_the_input() {
        let mut a = analyzer(WindowKind::Hann, Averaging::Exponential { alpha: 0.5 });
        a.push(&sine(64, 1.0, SIZE));
        let start = *a.power().get(64).unwrap();

        // Silence: the average should decay towards zero, not jump there.
        a.push(&vec![0.0; SIZE]);
        let after = *a.power().get(64).unwrap();
        assert!(after < start, "should decay: {start} -> {after}");
        assert!(after > 0.0, "should not snap to zero: {after}");
    }

    #[test]
    fn exponential_from_time_constant_is_in_range() {
        let Averaging::Exponential { alpha } = Averaging::exponential_over(1.0, 46.875) else {
            panic!("expected exponential");
        };
        assert!(alpha > 0.0 && alpha < 1.0, "alpha {alpha}");

        // Degenerate inputs fall back to no averaging rather than dividing by zero.
        assert_eq!(Averaging::exponential_over(0.0, 100.0), Averaging::None);
        assert_eq!(Averaging::exponential_over(1.0, 0.0), Averaging::None);
    }

    #[test]
    fn reset_clears_average_and_partial_frame() {
        let mut a = analyzer(WindowKind::Hann, Averaging::Infinite);
        a.push(&sine(64, 1.0, SIZE * 2));
        assert!(a.frames() > 0);

        a.reset();
        assert_eq!(a.frames(), 0);
        assert!(a.power().iter().all(|p| *p == 0.0));
    }

    #[test]
    fn bin_geometry_matches_sample_rate() {
        let a = analyzer(WindowKind::Hann, Averaging::None);
        assert_eq!(a.bins(), SIZE / 2 + 1);
        assert!((a.bin_spacing_hz() - RATE / SIZE as f32).abs() < 1e-6);
        assert!((a.bin_frequency(100) - 100.0 * RATE / SIZE as f32).abs() < 1e-3);

        // Hann's ENBW is 1.5 bins.
        assert!((a.enbw_hz() - 1.5 * a.bin_spacing_hz()).abs() < 1e-3);
    }

    #[test]
    fn empty_push_is_a_no_op() {
        let mut a = analyzer(WindowKind::Hann, Averaging::None);
        assert_eq!(a.push(&[]), 0);
        assert_eq!(a.frames(), 0);
    }

    #[test]
    fn silent_input_reads_at_the_floor() {
        let mut a = analyzer(WindowKind::Hann, Averaging::None);
        a.push(&vec![0.0; SIZE]);

        let mut db = vec![0.0; a.bins()];
        a.write_db_fs(&mut db);
        assert!(db.iter().all(|d| *d <= DB_FLOOR + 1e-3));
        assert!(db.iter().all(|d| d.is_finite()), "floor must be finite");
    }
}
