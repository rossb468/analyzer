//! Fitting parametric filters to the gap between a measurement and a target.
//!
//! The algorithm is greedy with local refinement, which is what most room
//! correction does and is chosen here for a reason that matters more than
//! elegance: the result has to be explainable. Every filter it produces
//! corresponds to a feature you can point at on the measurement, so a user who
//! disagrees with one can delete it and keep the rest. A global fit that solved
//! for all filters at once would score better on residual error and produce
//! filters that individually mean nothing.
//!
//! Each round finds the largest remaining error, places a peaking filter on it,
//! refines that filter's three parameters by coordinate descent, subtracts its
//! response from the residual, and repeats. Because filters cascade, their
//! decibel responses add, so subtracting is exact rather than an approximation.
//!
//! ## Boost is capped far harder than cut
//!
//! This is the part that separates a correction that works from one that makes
//! things worse. A dip in a room measurement is usually a cancellation — two
//! paths arriving out of phase — and no amount of electrical gain fills it in,
//! because the cancellation scales with the signal. What boosting a null does
//! achieve is consuming headroom and driving the woofer harder at exactly the
//! frequency where it is doing the least good.
//!
//! So [`OptimiserConfig::max_boost_db`] defaults well below
//! [`OptimiserConfig::max_cut_db`]. Cutting a peak is nearly free and nearly
//! always right; boosting a null is the classic way to burn excursion.
//!
//! ## It corrects the modal region by default
//!
//! [`OptimiserConfig::to_hz`] defaults to 500 Hz. Above the room's transition
//! frequency the response varies enormously with microphone position, so a
//! filter fitted to one position is fitted to noise as far as any other position
//! is concerned. Below it the modes are properties of the room and correcting
//! them helps everywhere. The band is configurable because the same machinery is
//! useful for correcting a loudspeaker measured close up, where the whole range
//! is meaningful.

use crate::eq::{FilterBand, FilterKind};
use crate::target::TargetCurve;

/// How the fit is constrained.
#[derive(Debug, Clone, PartialEq)]
pub struct OptimiserConfig {
    /// Most filters to produce. The fit stops early once nothing is left worth
    /// correcting.
    pub max_filters: usize,
    /// Low end of the corrected band, in hertz.
    pub from_hz: f32,
    /// High end of the corrected band, in hertz.
    pub to_hz: f32,
    /// Largest boost any one filter may apply. See the module note.
    pub max_boost_db: f32,
    /// Largest cut any one filter may apply.
    pub max_cut_db: f32,
    /// Narrowest filter allowed. Very high Q corrects a single measurement
    /// point, which is a property of the microphone position rather than of
    /// the room.
    pub max_q: f32,
    /// Widest filter allowed.
    pub min_q: f32,
    /// Errors smaller than this are left alone.
    pub threshold_db: f32,
    /// Rate the filters are designed at.
    pub sample_rate: f32,
}

impl Default for OptimiserConfig {
    fn default() -> Self {
        Self {
            max_filters: 8,
            from_hz: 20.0,
            to_hz: 500.0,
            // Deliberately asymmetric - see the module note.
            max_boost_db: 3.0,
            max_cut_db: 12.0,
            max_q: 8.0,
            min_q: 0.5,
            threshold_db: 0.5,
            sample_rate: 48_000.0,
        }
    }
}

impl OptimiserConfig {
    /// Force the configuration into a shape the fit can use.
    fn validated(mut self) -> Self {
        let default = Self::default();
        if !self.from_hz.is_finite() || self.from_hz <= 0.0 {
            self.from_hz = default.from_hz;
        }
        if !self.to_hz.is_finite() || self.to_hz <= self.from_hz {
            self.to_hz = (self.from_hz * 2.0).max(default.to_hz);
        }
        if !self.min_q.is_finite() || self.min_q <= 0.0 {
            self.min_q = default.min_q;
        }
        if !self.max_q.is_finite() || self.max_q < self.min_q {
            self.max_q = self.min_q.max(default.max_q);
        }
        self.max_boost_db = self.max_boost_db.max(0.0);
        self.max_cut_db = self.max_cut_db.max(0.0);
        if !self.threshold_db.is_finite() || self.threshold_db < 0.0 {
            self.threshold_db = default.threshold_db;
        }
        if !self.sample_rate.is_finite() || self.sample_rate <= 0.0 {
            self.sample_rate = default.sample_rate;
        }
        self.max_filters = self.max_filters.min(64);
        self
    }
}

/// What a fit produced.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct Optimisation {
    /// The filters, in the order they were placed — largest error first.
    pub bands: Vec<FilterBand>,
    /// Root-mean-square error across the corrected band before any filter.
    pub initial_error_db: f32,
    /// The same after every filter. Reported rather than asserted: how much
    /// improvement is achievable depends entirely on the measurement.
    pub final_error_db: f32,
}

impl Optimisation {
    /// Improvement in the RMS error, in decibels. Negative would mean the fit
    /// made things worse, which is worth being able to see.
    pub fn improvement_db(&self) -> f32 {
        self.initial_error_db - self.final_error_db
    }
}

/// Fit filters to the gap between `measured_db` and `target`.
///
/// `frequencies` and `measured_db` are read pairwise up to the shorter of the
/// two. Frequencies are expected to be ascending and log-spaced — which is what
/// the plot's own column frequencies are — because every point then carries
/// roughly equal weight per octave. Linearly spaced input would work but would
/// weight the top octave about as heavily as everything below it put together.
pub fn optimise(
    frequencies: &[f32],
    measured_db: &[f32],
    target: &TargetCurve,
    config: &OptimiserConfig,
) -> Optimisation {
    let config = config.clone().validated();

    // Residual starts as the error the correction has to remove, restricted to
    // points that are usable and inside the band.
    let mut points: Vec<(f32, f32)> = frequencies
        .iter()
        .zip(measured_db)
        .filter(|(hz, measured)| {
            **hz >= config.from_hz && **hz <= config.to_hz && hz.is_finite() && measured.is_finite()
        })
        .map(|(hz, measured)| (*hz, measured - target.db_at(*hz)))
        .collect();

    if points.is_empty() {
        return Optimisation::default();
    }

    let initial_error_db = rms(&points);
    let mut bands = Vec::new();

    for _ in 0..config.max_filters {
        let Some(band) = place_filter(&points, &config) else {
            break;
        };
        apply(&mut points, &band, config.sample_rate);
        bands.push(band);
    }

    Optimisation {
        final_error_db: rms(&points),
        initial_error_db,
        bands,
    }
}

/// Root-mean-square of the residual.
fn rms(points: &[(f32, f32)]) -> f32 {
    if points.is_empty() {
        return 0.0;
    }
    let sum: f64 = points
        .iter()
        .map(|(_, error)| f64::from(*error) * f64::from(*error))
        .sum();
    (sum / points.len() as f64).sqrt() as f32
}

/// Subtract a band's response from the residual.
///
/// Filters cascade, so their decibel responses add and this is exact.
fn apply(points: &mut [(f32, f32)], band: &FilterBand, sample_rate: f32) {
    let section = band.design(sample_rate);
    for (hz, error) in points.iter_mut() {
        let magnitude = section.magnitude_at(*hz, sample_rate);
        if magnitude > 0.0 {
            *error += 20.0 * magnitude.log10();
        }
    }
}

/// Place and refine one filter on the largest remaining error.
///
/// Returns `None` once nothing exceeds the threshold, which is what stops the
/// fit adding filters that correct half a decibel of nothing.
fn place_filter(points: &[(f32, f32)], config: &OptimiserConfig) -> Option<FilterBand> {
    let (peak_hz, peak_error) = points
        .iter()
        .copied()
        .max_by(|a, b| a.1.abs().total_cmp(&b.1.abs()))?;

    if peak_error.abs() < config.threshold_db {
        return None;
    }

    // A positive error is too much energy and wants a cut. The asymmetric caps
    // are the whole point - see the module note.
    let gain = if peak_error > 0.0 {
        (-peak_error).max(-config.max_cut_db)
    } else {
        (-peak_error).min(config.max_boost_db)
    };

    let start = FilterBand {
        kind: FilterKind::Peaking,
        hz: peak_hz,
        gain_db: gain,
        q: estimate_q(points, peak_hz, peak_error, config),
        enabled: true,
    };

    let refined = refine(start, points, config);
    // Refinement can shrink a filter to nothing if the feature was noise.
    (refined.gain_db.abs() >= 0.05).then_some(refined)
}

/// Guess a Q from how wide the feature is.
///
/// Walks outwards from the peak until the error falls to half of it, and
/// converts that width to a Q. A guess is enough because refinement moves it,
/// but a good guess keeps refinement from having to travel far.
fn estimate_q(
    points: &[(f32, f32)],
    peak_hz: f32,
    peak_error: f32,
    config: &OptimiserConfig,
) -> f32 {
    let half = peak_error / 2.0;
    let crossed = |error: f32| {
        if peak_error > 0.0 {
            error < half
        } else {
            error > half
        }
    };

    let peak_index = points
        .iter()
        .position(|(hz, _)| *hz >= peak_hz)
        .unwrap_or(0);

    let mut low = points.first().map_or(peak_hz, |(hz, _)| *hz);
    for (hz, error) in points[..peak_index].iter().rev() {
        if crossed(*error) {
            low = *hz;
            break;
        }
    }

    let mut high = points.last().map_or(peak_hz, |(hz, _)| *hz);
    for (hz, error) in points.iter().skip(peak_index + 1) {
        if crossed(*error) {
            high = *hz;
            break;
        }
    }

    if high <= low || low <= 0.0 {
        return 4.0f32.clamp(config.min_q, config.max_q);
    }
    // Q is centre frequency over bandwidth, with the centre taken as the
    // geometric mean because the axis is logarithmic.
    let bandwidth = high - low;
    let centre = (low * high).sqrt();
    (centre / bandwidth).clamp(config.min_q, config.max_q)
}

/// Coordinate descent over frequency, Q and gain.
///
/// Deterministic by construction: fixed candidate multipliers, fixed round
/// count, no randomness. Two runs on the same measurement must produce the same
/// filters, or comparing two corrections becomes impossible.
fn refine(mut band: FilterBand, points: &[(f32, f32)], config: &OptimiserConfig) -> FilterBand {
    const ROUNDS: usize = 4;
    let mut step = 1.0f32;

    for _ in 0..ROUNDS {
        let mut best = band;
        let mut best_score = score(&band, points, config);

        for factor in [
            1.0 - 0.10 * step,
            1.0 - 0.04 * step,
            1.0 + 0.04 * step,
            1.0 + 0.10 * step,
        ] {
            let mut candidate = band;
            candidate.hz = (band.hz * factor).clamp(config.from_hz, config.to_hz);
            try_candidate(candidate, points, config, &mut best, &mut best_score);

            let mut candidate = band;
            candidate.q = (band.q * factor).clamp(config.min_q, config.max_q);
            try_candidate(candidate, points, config, &mut best, &mut best_score);

            let mut candidate = band;
            candidate.gain_db =
                (band.gain_db * factor).clamp(-config.max_cut_db, config.max_boost_db);
            try_candidate(candidate, points, config, &mut best, &mut best_score);
        }

        band = best;
        step *= 0.5;
    }

    band
}

fn try_candidate(
    candidate: FilterBand,
    points: &[(f32, f32)],
    config: &OptimiserConfig,
    best: &mut FilterBand,
    best_score: &mut f32,
) {
    let candidate_score = score(&candidate, points, config);
    if candidate_score < *best_score {
        *best = candidate;
        *best_score = candidate_score;
    }
}

/// Residual RMS if this band were applied.
fn score(band: &FilterBand, points: &[(f32, f32)], config: &OptimiserConfig) -> f32 {
    let section = band.design(config.sample_rate);
    let sum: f64 = points
        .iter()
        .map(|(hz, error)| {
            let magnitude = section.magnitude_at(*hz, config.sample_rate);
            let corrected = if magnitude > 0.0 {
                error + 20.0 * magnitude.log10()
            } else {
                *error
            };
            f64::from(corrected) * f64::from(corrected)
        })
        .sum();
    (sum / points.len().max(1) as f64).sqrt() as f32
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::target::{TargetCurve, TargetShape};

    const RATE: f32 = 48_000.0;

    fn log_sweep(from: f32, to: f32, count: usize) -> Vec<f32> {
        (0..count)
            .map(|i| {
                let t = i as f32 / (count - 1) as f32;
                from * (to / from).powf(t)
            })
            .collect()
    }

    /// A measurement that is flat except for one resonance.
    fn with_peak(frequencies: &[f32], hz: f32, gain_db: f32, q: f32) -> Vec<f32> {
        let section = crate::Biquad::peaking(hz, q, gain_db, RATE);
        frequencies
            .iter()
            .map(|f| 20.0 * section.magnitude_at(*f, RATE).log10())
            .collect()
    }

    fn flat_target() -> TargetCurve {
        TargetCurve::new(TargetShape::Flat)
    }

    #[test]
    fn a_single_resonance_is_corrected_nearly_flat() {
        let frequencies = log_sweep(20.0, 500.0, 400);
        let measured = with_peak(&frequencies, 63.0, 8.0, 4.0);

        let result = optimise(
            &frequencies,
            &measured,
            &flat_target(),
            &OptimiserConfig::default(),
        );

        assert!(!result.bands.is_empty());
        assert!(
            result.final_error_db < 0.5,
            "residual {} dB from {}",
            result.final_error_db,
            result.initial_error_db
        );
        assert!(result.improvement_db() > 1.0);
    }

    #[test]
    fn the_first_filter_lands_on_the_resonance() {
        let frequencies = log_sweep(20.0, 500.0, 400);
        let measured = with_peak(&frequencies, 63.0, 8.0, 4.0);

        let result = optimise(
            &frequencies,
            &measured,
            &flat_target(),
            &OptimiserConfig::default(),
        );

        let first = result.bands.first().unwrap();
        assert!((first.hz / 63.0).log2().abs() < 0.25, "at {} Hz", first.hz);
        assert!(first.gain_db < 0.0, "a peak wants a cut");
    }

    #[test]
    fn two_resonances_get_two_filters() {
        let frequencies = log_sweep(20.0, 500.0, 600);
        let first = with_peak(&frequencies, 45.0, 7.0, 6.0);
        let second = with_peak(&frequencies, 180.0, -6.0, 5.0);
        let measured: Vec<f32> = first.iter().zip(&second).map(|(a, b)| a + b).collect();

        let result = optimise(
            &frequencies,
            &measured,
            &flat_target(),
            &OptimiserConfig::default(),
        );

        assert!(result.bands.len() >= 2, "{:?}", result.bands);
        assert!(result.improvement_db() > 1.0);
    }

    /// The rule the module exists to enforce. A null is a cancellation, and
    /// boosting it burns headroom without filling it.
    #[test]
    fn a_deep_null_is_not_boosted_past_the_cap() {
        let frequencies = log_sweep(20.0, 500.0, 400);
        let measured = with_peak(&frequencies, 80.0, -20.0, 8.0);

        let config = OptimiserConfig {
            max_boost_db: 3.0,
            ..OptimiserConfig::default()
        };
        let result = optimise(&frequencies, &measured, &flat_target(), &config);

        for band in &result.bands {
            assert!(
                band.gain_db <= 3.0 + 1e-4,
                "boosted {} dB into a null",
                band.gain_db
            );
        }
    }

    #[test]
    fn cuts_and_boosts_respect_their_separate_caps() {
        let frequencies = log_sweep(20.0, 500.0, 400);
        let peak = with_peak(&frequencies, 60.0, 18.0, 5.0);
        let dip = with_peak(&frequencies, 200.0, -18.0, 5.0);
        let measured: Vec<f32> = peak.iter().zip(&dip).map(|(a, b)| a + b).collect();

        let config = OptimiserConfig {
            max_boost_db: 2.0,
            max_cut_db: 9.0,
            ..OptimiserConfig::default()
        };
        let result = optimise(&frequencies, &measured, &flat_target(), &config);

        for band in &result.bands {
            assert!(band.gain_db <= 2.0 + 1e-4, "{}", band.gain_db);
            assert!(band.gain_db >= -9.0 - 1e-4, "{}", band.gain_db);
        }
    }

    #[test]
    fn filters_stay_inside_the_q_limits() {
        let frequencies = log_sweep(20.0, 500.0, 400);
        let measured = with_peak(&frequencies, 63.0, 10.0, 20.0);

        let config = OptimiserConfig {
            min_q: 1.0,
            max_q: 6.0,
            ..OptimiserConfig::default()
        };
        let result = optimise(&frequencies, &measured, &flat_target(), &config);

        for band in &result.bands {
            assert!(band.q >= 1.0 - 1e-4 && band.q <= 6.0 + 1e-4, "q {}", band.q);
        }
    }

    #[test]
    fn nothing_outside_the_band_is_corrected() {
        let frequencies = log_sweep(20.0, 20_000.0, 800);
        let measured = with_peak(&frequencies, 5000.0, 10.0, 4.0);

        let result = optimise(
            &frequencies,
            &measured,
            &flat_target(),
            &OptimiserConfig::default(),
        );
        assert!(
            result.bands.is_empty(),
            "corrected above the band: {:?}",
            result.bands
        );
    }

    /// A flat measurement needs no filters, and producing some anyway would be
    /// the optimiser inventing work.
    #[test]
    fn an_already_flat_measurement_gets_no_filters() {
        let frequencies = log_sweep(20.0, 500.0, 400);
        let measured = vec![0.0; frequencies.len()];
        let result = optimise(
            &frequencies,
            &measured,
            &flat_target(),
            &OptimiserConfig::default(),
        );
        assert!(result.bands.is_empty());
    }

    #[test]
    fn the_fit_never_makes_the_error_worse() {
        let frequencies = log_sweep(20.0, 500.0, 400);
        for (hz, gain, q) in [(40.0, 9.0, 3.0), (120.0, -7.0, 6.0), (300.0, 4.0, 1.5)] {
            let measured = with_peak(&frequencies, hz, gain, q);
            let result = optimise(
                &frequencies,
                &measured,
                &flat_target(),
                &OptimiserConfig::default(),
            );
            assert!(
                result.final_error_db <= result.initial_error_db + 1e-4,
                "{hz} Hz: {} -> {}",
                result.initial_error_db,
                result.final_error_db
            );
        }
    }

    /// Comparing two corrections is impossible if the same input can produce
    /// different filters.
    #[test]
    fn the_fit_is_deterministic() {
        let frequencies = log_sweep(20.0, 500.0, 400);
        let measured = with_peak(&frequencies, 63.0, 8.0, 4.0);
        let run = || {
            optimise(
                &frequencies,
                &measured,
                &flat_target(),
                &OptimiserConfig::default(),
            )
        };
        assert_eq!(run(), run());
    }

    #[test]
    fn it_honours_a_non_flat_target() {
        let frequencies = log_sweep(20.0, 500.0, 400);
        // The measurement already has the bass lift the target asks for, so
        // there is nothing to correct.
        let target = TargetCurve::new(TargetShape::room());
        let measured: Vec<f32> = frequencies.iter().map(|hz| target.db_at(*hz)).collect();

        let result = optimise(
            &frequencies,
            &measured,
            &target,
            &OptimiserConfig::default(),
        );
        assert!(
            result.bands.is_empty(),
            "corrected a measurement that already matched: {:?}",
            result.bands
        );
    }

    #[test]
    fn a_measurement_missing_the_targets_bass_lift_gets_boosted() {
        let frequencies = log_sweep(20.0, 500.0, 400);
        let target = TargetCurve::new(TargetShape::room());
        let measured = vec![0.0; frequencies.len()];

        let result = optimise(
            &frequencies,
            &measured,
            &target,
            &OptimiserConfig::default(),
        );
        assert!(!result.bands.is_empty());
        assert!(
            result.bands.iter().any(|band| band.gain_db > 0.0),
            "expected a boost toward the target's bass lift"
        );
    }

    #[test]
    fn max_filters_is_respected() {
        let frequencies = log_sweep(20.0, 500.0, 600);
        let mut measured = vec![0.0f32; frequencies.len()];
        for (index, hz) in [30.0, 55.0, 90.0, 140.0, 220.0, 350.0].iter().enumerate() {
            let gain = if index % 2 == 0 { 8.0 } else { -8.0 };
            for (slot, value) in with_peak(&frequencies, *hz, gain, 6.0).iter().enumerate() {
                measured[slot] += value;
            }
        }

        let config = OptimiserConfig {
            max_filters: 3,
            ..OptimiserConfig::default()
        };
        let result = optimise(&frequencies, &measured, &flat_target(), &config);
        assert!(result.bands.len() <= 3);
    }

    #[test]
    fn empty_and_degenerate_input_is_survivable() {
        let config = OptimiserConfig::default();
        assert_eq!(
            optimise(&[], &[], &flat_target(), &config),
            Optimisation::default()
        );
        // Mismatched lengths truncate to the shorter side.
        let result = optimise(&[100.0, 200.0], &[0.0], &flat_target(), &config);
        assert!(result.bands.is_empty());
        // Everything filtered out by the band.
        let result = optimise(&[5.0], &[10.0], &flat_target(), &config);
        assert_eq!(result, Optimisation::default());
    }

    #[test]
    fn a_nonsense_configuration_is_repaired_rather_than_obeyed() {
        let frequencies = log_sweep(20.0, 500.0, 200);
        let measured = with_peak(&frequencies, 63.0, 8.0, 4.0);
        let config = OptimiserConfig {
            from_hz: 0.0,
            to_hz: -1.0,
            min_q: -3.0,
            max_q: f32::NAN,
            threshold_db: f32::NAN,
            sample_rate: 0.0,
            ..OptimiserConfig::default()
        };
        let result = optimise(&frequencies, &measured, &flat_target(), &config);
        for band in &result.bands {
            assert!(band.hz.is_finite() && band.hz > 0.0);
            assert!(band.q.is_finite() && band.q > 0.0);
            assert!(band.gain_db.is_finite());
        }
    }

    #[test]
    fn non_finite_measurement_points_are_skipped() {
        let frequencies = log_sweep(20.0, 500.0, 400);
        let mut measured = with_peak(&frequencies, 63.0, 8.0, 4.0);
        measured[10] = f32::NAN;
        measured[20] = f32::NEG_INFINITY;

        let result = optimise(
            &frequencies,
            &measured,
            &flat_target(),
            &OptimiserConfig::default(),
        );
        assert!(result.final_error_db.is_finite());
        assert!(result.bands.iter().all(|band| band.gain_db.is_finite()));
    }
}
