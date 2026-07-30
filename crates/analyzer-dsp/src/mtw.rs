//! Multi-time-window transfer function analysis.
//!
//! A single FFT forces one choice of resolution for the whole spectrum, and
//! there is no good answer. Pick 32768 points for 1.5 Hz in the bass and the
//! frame spans 680 ms, so the display lags badly and the top octave gets
//! thousands of bins nobody needs. Pick 1024 for a responsive top end and the
//! bass is 47 Hz per bin, which cannot resolve a room mode at all.
//!
//! MTW runs several transfer function engines at once, each with its own FFT
//! size, and splices their outputs — a long window where frequencies are close
//! together and change slowly, a short one where they are far apart and change
//! fast. That is the technique Smaart is built around, and it is the main thing
//! this offers over REW's real-time side.
//!
//! # Why the splices line up
//!
//! It is not obvious that independently windowed engines should agree on phase.
//! They do, and the reason is worth stating: the transfer function is a *ratio*.
//! Shifting the analysis window shifts both `X` and `Y` by the same amount,
//! multiplying both by the same `e^(-jωτ)`, which cancels in `Y/X`. Each engine
//! is therefore self-consistent, and all of them are consistent with each other,
//! provided each one sees its two channels sample-aligned.
//!
//! Magnitude and coherence line up for the same reason.
//!
//! # What does not line up
//!
//! Coherence still means something slightly different in each band, because it
//! is measured over a different window length. A band using a 256-point window
//! is asking "are these correlated over 5 ms", and one using 32768 points is
//! asking about 680 ms. Both are useful, neither is wrong, and no amount of
//! splicing makes them the same question. This is documented rather than papered
//! over.

use crate::Complex32;
use crate::spectrum::Overlap;
use crate::transfer::{TransferAveraging, TransferConfig, TransferFunction};
use crate::window::WindowKind;

/// Floor for a point with no usable reference energy.
pub const MTW_FLOOR_DB: f32 = -200.0;

/// One spliced output point.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct MtwPoint {
    /// Frequency in hertz.
    pub hz: f32,
    /// Magnitude in decibels.
    pub magnitude_db: f32,
    /// Phase in degrees, wrapped to `-180..=180`.
    pub phase_degrees: f32,
    /// Coherence, `0..=1`. Comparable within a band, only roughly across bands.
    pub coherence: f32,
    /// FFT size of the band this point came from, for display and diagnosis.
    pub fft_size: usize,
}

/// Setup for [`MultiTimeWindow`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct MtwConfig {
    /// Sample rate in hertz.
    pub sample_rate: f32,
    /// FFT size for the lowest band. Larger means finer bass resolution and more
    /// latency: 32768 gives 1.5 Hz and 680 ms at 48 kHz, 65536 gives 0.73 Hz and
    /// 1.4 s.
    pub largest_fft: usize,
    /// FFT size for the highest band.
    pub smallest_fft: usize,
    /// Analysis window, used by every band.
    pub window: WindowKind,
    /// Frame overlap, used by every band.
    pub overlap: Overlap,
    /// Averaging mode, used by every band.
    pub averaging: TransferAveraging,
    /// Output points per octave. 48 is plenty for a display and keeps the total
    /// in the hundreds rather than the tens of thousands.
    pub points_per_octave: usize,
    /// Lowest output frequency.
    pub min_hz: f32,
}

impl Default for MtwConfig {
    fn default() -> Self {
        Self {
            sample_rate: 48_000.0,
            largest_fft: 32_768,
            smallest_fft: 256,
            window: WindowKind::Hann,
            overlap: Overlap::Half,
            averaging: TransferAveraging::Exponential { alpha: 0.2 },
            points_per_octave: 48,
            min_hz: 20.0,
        }
    }
}

/// One band: an engine plus the frequency range it is responsible for.
#[derive(Debug)]
struct BandEngine {
    engine: TransferFunction,
    response: Vec<Complex32>,
    coherence: Vec<f32>,
    lower_hz: f32,
    upper_hz: f32,
}

/// Spliced multi-resolution transfer function.
#[derive(Debug)]
pub struct MultiTimeWindow {
    sample_rate: f32,
    bands: Vec<BandEngine>,
    points: Vec<MtwPoint>,
    frames: u32,
}

impl MultiTimeWindow {
    /// Build the band engines and the output grid.
    ///
    /// # Panics
    ///
    /// Panics if the FFT sizes are not powers of two with `smallest <= largest`,
    /// or if the sample rate or grid density is not positive.
    pub fn new(config: MtwConfig) -> Self {
        assert!(
            config.sample_rate > 0.0,
            "sample rate must be positive, got {}",
            config.sample_rate
        );
        assert!(
            config.smallest_fft.is_power_of_two() && config.largest_fft.is_power_of_two(),
            "FFT sizes must be powers of two, got {} and {}",
            config.smallest_fft,
            config.largest_fft
        );
        assert!(
            config.smallest_fft >= 64 && config.smallest_fft <= config.largest_fft,
            "need 64 <= smallest_fft <= largest_fft"
        );
        assert!(
            config.points_per_octave > 0,
            "points per octave must be at least 1"
        );
        assert!(config.min_hz > 0.0, "min_hz must be positive");

        let nyquist = config.sample_rate / 2.0;

        // The top band takes the top octave with the shortest window; each
        // octave down doubles the FFT size, which keeps the number of bins per
        // octave roughly constant instead of piling them up at the top.
        let mut bands = Vec::new();
        let mut size = config.smallest_fft;
        let mut upper = nyquist;

        loop {
            let is_last = size >= config.largest_fft;
            // The lowest band runs all the way down to DC; it has the resolution
            // to, and nothing below it would otherwise be covered.
            let lower = if is_last { 0.0 } else { upper / 2.0 };

            bands.push(BandEngine {
                engine: TransferFunction::new(TransferConfig {
                    sample_rate: config.sample_rate,
                    size,
                    window: config.window,
                    overlap: config.overlap,
                    averaging: config.averaging,
                }),
                response: vec![Complex32::default(); size / 2 + 1],
                coherence: vec![0.0; size / 2 + 1],
                lower_hz: lower,
                upper_hz: upper,
            });

            if is_last {
                break;
            }
            upper = lower;
            size *= 2;
        }

        // Log-spaced output grid. This is where the point count collapses: a
        // 32768-point FFT has 16385 bins, and 48 per octave across the audio
        // band is a few hundred.
        let octaves = (nyquist / config.min_hz).log2();
        let count = (octaves * config.points_per_octave as f32).ceil() as usize;
        let step = 2.0_f32.powf(1.0 / config.points_per_octave as f32);

        let mut points = Vec::with_capacity(count + 1);
        let mut hz = config.min_hz;
        while hz <= nyquist && points.len() <= count + 1 {
            points.push(MtwPoint {
                hz,
                magnitude_db: MTW_FLOOR_DB,
                phase_degrees: 0.0,
                coherence: 0.0,
                fft_size: 0,
            });
            hz *= step;
        }

        Self {
            sample_rate: config.sample_rate,
            bands,
            points,
            frames: 0,
        }
    }

    /// Feed matched samples to every band.
    ///
    /// Returns the number of frames the *lowest* band completed, since that is
    /// the one that gates a full-bandwidth result.
    ///
    /// # Panics
    ///
    /// Panics if the slices differ in length.
    pub fn push(&mut self, reference: &[f32], measurement: &[f32]) -> usize {
        assert_eq!(
            reference.len(),
            measurement.len(),
            "reference and measurement must be sample aligned"
        );
        let mut slowest = 0;
        for band in &mut self.bands {
            let produced = band.engine.push(reference, measurement);
            // The last band is the largest FFT and the slowest to fill.
            slowest = produced;
        }
        self.frames = self
            .bands
            .last()
            .map(|band| band.engine.frames())
            .unwrap_or(0);
        slowest
    }

    /// Recompute the spliced output.
    ///
    /// Call before reading [`MultiTimeWindow::points`]; it is separate from
    /// `push` so a display running at 120 Hz does not force a resplice on every
    /// audio block.
    pub fn resolve(&mut self) {
        for band in &mut self.bands {
            band.engine.write_response(&mut band.response);
            band.engine.write_coherence(&mut band.coherence);
        }

        for point in &mut self.points {
            let Some(band) = self
                .bands
                .iter()
                .find(|band| point.hz >= band.lower_hz && point.hz < band.upper_hz)
            else {
                continue;
            };

            let spacing = band.engine.bin_spacing_hz();
            if spacing <= 0.0 {
                continue;
            }
            let exact = point.hz / spacing;
            let lower = exact.floor() as usize;
            let fraction = exact - lower as f32;

            let a = band.response.get(lower).copied().unwrap_or_default();
            let b = band.response.get(lower + 1).copied().unwrap_or(a);

            // Interpolate magnitude and angle separately, taking the shortest
            // angular path. Two obvious alternatives are both wrong:
            //
            // - Blending phase as a plain number breaks across a ±180° wrap and
            //   sends the result the long way round.
            // - Blending the complex values linearly chords across the arc, so
            //   magnitude sags wherever phase rotates fast between bins. With a
            //   200-sample delay that is a 35° step and a 0.6 dB dip - small,
            //   but a systematic error in the magnitude curve.
            //
            // (b · conj(a)).arg() is the signed angle from a to b already
            // wrapped into (-π, π], which is exactly the shortest path. It is
            // still wrong if the true step exceeds 180°, but that is aliasing in
            // the underlying delay and no interpolation recovers from it.
            let magnitude_a = a.norm();
            let magnitude_b = b.norm();
            let magnitude = magnitude_a + (magnitude_b - magnitude_a) * fraction;
            let step = (b * a.conj()).arg();
            let angle = a.arg() + step * fraction;

            let ca = band.coherence.get(lower).copied().unwrap_or(0.0);
            let cb = band.coherence.get(lower + 1).copied().unwrap_or(ca);

            point.magnitude_db = if magnitude > 0.0 {
                (20.0 * magnitude.log10()).max(MTW_FLOOR_DB)
            } else {
                MTW_FLOOR_DB
            };
            // Re-wrap, since a.arg() + step can leave the principal range.
            let wrapped = (angle.to_degrees() + 180.0).rem_euclid(360.0) - 180.0;
            point.phase_degrees = wrapped;
            point.coherence = (ca + (cb - ca) * fraction).clamp(0.0, 1.0);
            point.fft_size = band.engine.size();
        }
    }

    /// The spliced result. Call [`MultiTimeWindow::resolve`] first.
    pub fn points(&self) -> &[MtwPoint] {
        &self.points
    }

    /// Frames the lowest band has averaged, which gates a trustworthy bass reading.
    pub fn frames(&self) -> u32 {
        self.frames
    }

    /// FFT sizes in use, from the highest band down to the lowest.
    pub fn fft_sizes(&self) -> Vec<usize> {
        self.bands.iter().map(|band| band.engine.size()).collect()
    }

    /// Bin spacing of the lowest band — the finest resolution available.
    pub fn finest_resolution_hz(&self) -> f32 {
        self.bands
            .last()
            .map(|band| band.engine.bin_spacing_hz())
            .unwrap_or(0.0)
    }

    /// Seconds of audio the lowest band's window spans, which is the latency
    /// before the bass reading settles.
    pub fn longest_window_seconds(&self) -> f32 {
        self.bands
            .last()
            .map(|band| band.engine.size() as f32 / self.sample_rate)
            .unwrap_or(0.0)
    }

    /// Discard all averages.
    pub fn reset(&mut self) {
        for band in &mut self.bands {
            band.engine.reset();
        }
        self.frames = 0;
        for point in &mut self.points {
            point.magnitude_db = MTW_FLOOR_DB;
            point.phase_degrees = 0.0;
            point.coherence = 0.0;
        }
    }
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::{Generator, Signal};

    const RATE: f32 = 48_000.0;

    fn config() -> MtwConfig {
        MtwConfig {
            sample_rate: RATE,
            largest_fft: 16_384,
            smallest_fft: 256,
            averaging: TransferAveraging::Infinite,
            ..MtwConfig::default()
        }
    }

    fn pink(samples: usize, seed: u64) -> Vec<f32> {
        let mut generator = Generator::new(RATE, Signal::PinkNoise { amplitude: 0.5 }, seed);
        let mut out = vec![0.0; samples];
        generator.fill(&mut out);
        out
    }

    /// Points with enough reference energy and enough averaging to trust.
    fn usable(points: &[MtwPoint]) -> Vec<&MtwPoint> {
        points
            .iter()
            .filter(|p| p.hz >= 100.0 && p.hz <= 10_000.0 && p.fft_size > 0)
            .collect()
    }

    #[test]
    fn bands_double_in_size_and_cover_the_whole_spectrum() {
        let mtw = MultiTimeWindow::new(config());
        let sizes = mtw.fft_sizes();
        assert_eq!(sizes, vec![256, 512, 1024, 2048, 4096, 8192, 16_384]);
        assert!((mtw.finest_resolution_hz() - RATE / 16_384.0).abs() < 1e-3);
    }

    /// The headline claim: far fewer points than one big FFT, while keeping the
    /// big FFT's resolution where it matters.
    #[test]
    fn the_point_count_collapses_against_a_single_large_fft() {
        let mtw = MultiTimeWindow::new(config());
        let single_fft_bins = 16_384 / 2 + 1;
        assert!(
            mtw.points().len() < single_fft_bins / 10,
            "{} points vs {single_fft_bins} bins",
            mtw.points().len()
        );
        // And still finer than 3 Hz down low.
        assert!(mtw.finest_resolution_hz() < 3.0);
    }

    #[test]
    fn a_wire_measures_flat_across_every_band() {
        let signal = pink(16_384 * 8, 1);
        let mut mtw = MultiTimeWindow::new(config());
        mtw.push(&signal, &signal);
        mtw.resolve();

        for point in usable(mtw.points()) {
            assert!(
                point.magnitude_db.abs() < 0.2,
                "{:.0} Hz (fft {}): {} dB",
                point.hz,
                point.fft_size,
                point.magnitude_db
            );
            assert!(
                point.phase_degrees.abs() < 2.0,
                "{:.0} Hz: {}°",
                point.hz,
                point.phase_degrees
            );
            assert!(
                point.coherence > 0.98,
                "{:.0} Hz: γ² {}",
                point.hz,
                point.coherence
            );
        }
    }

    /// The test that decides whether splicing works at all. A flat gain must be
    /// flat *through* the crossovers - a discontinuity there is the classic MTW
    /// failure and it is immediately visible on a display.
    #[test]
    fn a_gain_is_continuous_across_the_splices() {
        let reference = pink(16_384 * 8, 2);
        let measurement: Vec<f32> = reference.iter().map(|s| s * 2.0).collect();

        let mut mtw = MultiTimeWindow::new(config());
        mtw.push(&reference, &measurement);
        mtw.resolve();

        let points = usable(mtw.points());
        for point in &points {
            assert!(
                (point.magnitude_db - 6.0206).abs() < 0.3,
                "{:.0} Hz (fft {}): {} dB",
                point.hz,
                point.fft_size,
                point.magnitude_db
            );
        }

        // Specifically check the steps where the FFT size changes.
        for pair in points.windows(2) {
            if pair[0].fft_size != pair[1].fft_size {
                let step = (pair[1].magnitude_db - pair[0].magnitude_db).abs();
                assert!(
                    step < 0.3,
                    "discontinuity of {step:.2} dB at the {}/{} crossover near {:.0} Hz",
                    pair[0].fft_size,
                    pair[1].fft_size,
                    pair[0].hz
                );
            }
        }
    }

    /// The hardest continuity test. Each engine windows the signal differently,
    /// so if phase were referenced to the window rather than cancelling in the
    /// ratio, a delay would produce a visible jump at every crossover.
    #[test]
    fn phase_from_a_delay_is_continuous_across_the_splices() {
        let delay = 24_usize;
        let reference = pink(16_384 * 8, 3);
        let mut measurement = vec![0.0; reference.len()];
        measurement[delay..].copy_from_slice(&reference[..reference.len() - delay]);

        let mut mtw = MultiTimeWindow::new(config());
        mtw.push(&reference, &measurement);
        mtw.resolve();

        // Expected phase for a pure delay, wrapped the same way.
        let expected_at = |hz: f32| -> f32 {
            let raw = -360.0 * hz * delay as f32 / RATE;
            let wrapped = raw.rem_euclid(360.0);
            if wrapped > 180.0 {
                wrapped - 360.0
            } else {
                wrapped
            }
        };

        for point in usable(mtw.points()) {
            let want = expected_at(point.hz);
            let mut error = (point.phase_degrees - want).abs();
            if error > 180.0 {
                error = 360.0 - error;
            }
            assert!(
                error < 6.0,
                "{:.0} Hz (fft {}): phase {:.1}°, expected {:.1}°",
                point.hz,
                point.fft_size,
                point.phase_degrees,
                want
            );
        }
    }

    /// Interpolating magnitude and phase separately breaks across a ±180° wrap.
    /// This uses a delay big enough to wrap phase repeatedly between adjacent
    /// bins, but small against the shortest window so the wrap is the only thing
    /// under test.
    #[test]
    fn interpolation_survives_phase_wrapping_between_bins() {
        let delay = 64_usize;
        let reference = pink(16_384 * 8, 4);
        let mut measurement = vec![0.0; reference.len()];
        measurement[delay..].copy_from_slice(&reference[..reference.len() - delay]);

        // Shortest window 1024, so 64 samples is 6% of even the shortest frame.
        let mut mtw = MultiTimeWindow::new(MtwConfig {
            smallest_fft: 1024,
            ..config()
        });
        mtw.push(&reference, &measurement);
        mtw.resolve();

        for point in usable(mtw.points()) {
            assert!(
                point.magnitude_db.abs() < 0.3,
                "{:.0} Hz (fft {}): magnitude {} dB - interpolation should not \
                 lose level to a wrap",
                point.hz,
                point.fft_size,
                point.magnitude_db
            );
        }
    }

    /// Documents why the delay finder is a prerequisite rather than a nicety.
    ///
    /// A delay that is a large fraction of the analysis window means the frame of
    /// measurement no longer contains the same audio as the frame of reference,
    /// so the cross-spectrum decorrelates: magnitude reads low and coherence
    /// falls. That is physics, not a defect - and it is exactly what compensating
    /// the delay before measuring avoids.
    #[test]
    fn uncompensated_delay_costs_magnitude_and_coherence() {
        let reference = pink(16_384 * 8, 20);

        let measure = |delay: usize| -> (f32, f32) {
            let mut measurement = vec![0.0; reference.len()];
            measurement[delay..].copy_from_slice(&reference[..reference.len() - delay]);
            let mut mtw = MultiTimeWindow::new(config());
            mtw.push(&reference, &measurement);
            mtw.resolve();
            // Look in the top band, where the window is shortest and the effect
            // is largest.
            let points: Vec<&MtwPoint> = mtw
                .points()
                .iter()
                .filter(|p| p.hz >= 13_000.0 && p.hz <= 20_000.0)
                .collect();
            let magnitude =
                points.iter().map(|p| p.magnitude_db).sum::<f32>() / points.len() as f32;
            let coherence = points.iter().map(|p| p.coherence).sum::<f32>() / points.len() as f32;
            (magnitude, coherence)
        };

        let (aligned_db, aligned_coherence) = measure(4);
        let (delayed_db, delayed_coherence) = measure(200);

        assert!(
            aligned_db.abs() < 0.3,
            "a nearly aligned measurement should read flat, got {aligned_db}"
        );
        assert!(
            delayed_db < aligned_db - 0.3,
            "200 samples against a 256-point window should cost level: \
             {aligned_db:.2} -> {delayed_db:.2} dB"
        );
        assert!(
            delayed_coherence < aligned_coherence - 0.05,
            "and coherence: {aligned_coherence:.3} -> {delayed_coherence:.3}"
        );
    }

    #[test]
    fn uncorrelated_input_gives_low_coherence_in_every_band() {
        let reference = pink(16_384 * 8, 5);
        let measurement = pink(16_384 * 8, 6);

        let mut mtw = MultiTimeWindow::new(config());
        mtw.push(&reference, &measurement);
        mtw.resolve();

        let points = usable(mtw.points());
        let mean: f32 = points.iter().map(|p| p.coherence).sum::<f32>() / points.len() as f32;
        assert!(mean < 0.4, "mean coherence {mean} for uncorrelated signals");
    }

    #[test]
    fn every_point_is_assigned_to_exactly_one_band() {
        let signal = pink(16_384 * 4, 7);
        let mut mtw = MultiTimeWindow::new(config());
        mtw.push(&signal, &signal);
        mtw.resolve();

        for point in mtw.points() {
            assert!(
                point.fft_size > 0,
                "{:.1} Hz was covered by no band",
                point.hz
            );
        }
    }

    /// Band assignment must go the right way round: long windows in the bass.
    #[test]
    fn low_frequencies_use_the_longest_window() {
        let signal = pink(16_384 * 4, 8);
        let mut mtw = MultiTimeWindow::new(config());
        mtw.push(&signal, &signal);
        mtw.resolve();

        let lowest = mtw.points().first().unwrap();
        let highest = mtw.points().last().unwrap();
        assert!(
            lowest.fft_size > highest.fft_size,
            "bass used fft {} and treble used {}",
            lowest.fft_size,
            highest.fft_size
        );
        assert_eq!(lowest.fft_size, 16_384);
    }

    #[test]
    fn the_grid_is_logarithmic() {
        let mtw = MultiTimeWindow::new(config());
        let points = mtw.points();
        // Consecutive ratios must be constant, which is what log spacing means.
        let first_ratio = points[1].hz / points[0].hz;
        for pair in points.windows(2) {
            let ratio = pair[1].hz / pair[0].hz;
            assert!(
                (ratio - first_ratio).abs() < 1e-4,
                "ratio drifted: {ratio} vs {first_ratio}"
            );
        }
        // 48 points per octave means each step is the 48th root of 2.
        assert!((first_ratio - 2.0_f32.powf(1.0 / 48.0)).abs() < 1e-5);
    }

    #[test]
    fn reset_clears_every_band() {
        let signal = pink(16_384 * 4, 9);
        let mut mtw = MultiTimeWindow::new(config());
        mtw.push(&signal, &signal);
        mtw.resolve();
        assert!(mtw.frames() > 0);

        mtw.reset();
        assert_eq!(mtw.frames(), 0);
        assert!(mtw.points().iter().all(|p| p.coherence == 0.0));
    }

    #[test]
    fn window_length_and_resolution_are_reported_honestly() {
        let mtw = MultiTimeWindow::new(MtwConfig {
            largest_fft: 32_768,
            ..config()
        });
        // 32768 at 48 kHz is 1.46 Hz and 683 ms - both worth surfacing, because
        // the second is the latency before a bass reading settles.
        assert!((mtw.finest_resolution_hz() - 1.4648).abs() < 0.01);
        assert!((mtw.longest_window_seconds() - 0.6827).abs() < 0.01);
    }

    #[test]
    #[should_panic(expected = "FFT sizes must be powers of two")]
    fn non_power_of_two_sizes_are_rejected() {
        let _ = MultiTimeWindow::new(MtwConfig {
            largest_fft: 12_000,
            ..config()
        });
    }

    #[test]
    #[should_panic(expected = "sample aligned")]
    fn mismatched_lengths_are_rejected() {
        let mut mtw = MultiTimeWindow::new(config());
        mtw.push(&[0.0; 100], &[0.0; 99]);
    }
}
