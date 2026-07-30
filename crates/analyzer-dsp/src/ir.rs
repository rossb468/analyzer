//! Getting answers out of an impulse response.
//!
//! Gating, frequency response, and reverberation time. Together these are most
//! of what an impulse response is *for*.
//!
//! # Gating and the resolution it costs
//!
//! A room's impulse response contains the direct sound, then reflections, then a
//! reverberant tail. Which of those you want depends on the question. Judging a
//! loudspeaker means gating to the direct sound before the first wall reflection
//! arrives; judging a room means keeping everything.
//!
//! Gating is not free, and the price is fixed by physics rather than by
//! implementation: a gate of length `T` cannot resolve anything finer than
//! `1/T` in frequency. A 5 ms gate — typical for a domestic room, where the
//! first reflection arrives around then — gives 200 Hz resolution, so the result
//! says nothing meaningful below a few hundred hertz. That is why quasi-anechoic
//! measurements are always spliced to a near-field or ground-plane measurement
//! in the bass, and why [`GatedResponse::resolution_hz`] is reported rather than
//! left for the user to work out.

use crate::deconv::ImpulseResponse;
use crate::fft::{Fft, RealFft};
use crate::window::{Window, WindowKind};

/// Floor for decibel output.
pub const IR_FLOOR_DB: f32 = -200.0;

/// A time window applied to an impulse response, in seconds relative to the
/// direct arrival.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Gate {
    /// Where the gate opens. Usually slightly negative, to keep the leading edge
    /// of the direct arrival rather than slicing into it.
    pub start_seconds: f32,
    /// Where the gate closes.
    pub end_seconds: f32,
    /// Length of the taper at the closing edge.
    ///
    /// A hard cut is a rectangular window, and its spectral leakage smears the
    /// response badly. Tapering the close costs a little time resolution and
    /// removes the artefact.
    pub fade_seconds: f32,
}

impl Gate {
    /// A quasi-anechoic gate: the direct sound only, out to `end_seconds`.
    pub fn anechoic(end_seconds: f32) -> Self {
        Self {
            start_seconds: -0.001,
            end_seconds,
            fade_seconds: (end_seconds * 0.25).min(0.002),
        }
    }

    /// Gate length in seconds.
    pub fn length_seconds(&self) -> f32 {
        (self.end_seconds - self.start_seconds).max(0.0)
    }

    /// The finest frequency resolution this gate permits.
    pub fn resolution_hz(&self) -> f32 {
        let length = self.length_seconds();
        if length > 0.0 {
            1.0 / length
        } else {
            f32::INFINITY
        }
    }
}

/// A frequency response derived from a gated impulse response.
#[derive(Debug, Clone, PartialEq)]
pub struct GatedResponse {
    /// Magnitude per bin in decibels.
    pub magnitude_db: Vec<f32>,
    /// Phase per bin in degrees.
    pub phase_degrees: Vec<f32>,
    /// Hertz between bins.
    pub bin_spacing_hz: f32,
    /// Finest frequency the gate can resolve. Anything below this is an artefact
    /// of the gate, not a property of the system.
    pub resolution_hz: f32,
}

impl GatedResponse {
    /// Centre frequency of bin `index`.
    pub fn bin_frequency(&self, index: usize) -> f32 {
        index as f32 * self.bin_spacing_hz
    }

    /// Whether a frequency is above the gate's resolution limit and therefore
    /// worth believing.
    pub fn is_trustworthy(&self, hz: f32) -> bool {
        hz >= self.resolution_hz
    }
}

/// Apply a gate to an impulse response, returning the windowed samples.
///
/// The output is the gated region only, not the whole response zero-padded, so
/// a caller can see how many samples actually survived.
pub fn apply_gate(ir: &ImpulseResponse, gate: &Gate) -> Vec<f32> {
    if ir.sample_rate <= 0.0 || ir.samples.is_empty() {
        return Vec::new();
    }

    let to_index =
        |seconds: f32| -> isize { (ir.peak_samples + seconds * ir.sample_rate).round() as isize };
    let start = to_index(gate.start_seconds).max(0) as usize;
    let end = (to_index(gate.end_seconds).max(0) as usize).min(ir.samples.len());
    if start >= end {
        return Vec::new();
    }

    let mut out = ir.samples.get(start..end).unwrap_or(&[]).to_vec();
    let fade = ((gate.fade_seconds * ir.sample_rate) as usize).min(out.len());

    // Taper only the closing edge. The opening edge sits in silence before the
    // arrival, so there is nothing there to discontinuity against.
    if fade > 1 {
        let start_of_fade = out.len() - fade;
        for (offset, sample) in out.iter_mut().skip(start_of_fade).enumerate() {
            let t = offset as f32 / (fade - 1) as f32;
            // Half a Hann: unity at the start of the fade, zero at the end.
            *sample *= 0.5 * (1.0 + (std::f32::consts::PI * t).cos());
        }
    }
    out
}

/// Compute the frequency response of a gated impulse response.
///
/// `fft_size` is zero-padded to, which interpolates the displayed curve without
/// adding information — the real resolution is still set by the gate.
///
/// Returns `None` if the gate keeps nothing or `fft_size` is unusable.
pub fn gated_response(ir: &ImpulseResponse, gate: &Gate, fft_size: usize) -> Option<GatedResponse> {
    if fft_size < 2 || !fft_size.is_multiple_of(2) {
        return None;
    }
    let gated = apply_gate(ir, gate);
    if gated.is_empty() {
        return None;
    }

    let mut fft = RealFft::new(fft_size);
    let mut padded = vec![0.0_f32; fft_size];
    let take = gated.len().min(fft_size);
    if let (Some(dst), Some(src)) = (padded.get_mut(..take), gated.get(..take)) {
        dst.copy_from_slice(src);
    }

    let mut spectrum = vec![crate::Complex32::default(); fft.bins()];
    fft.forward(&padded, &mut spectrum);

    Some(GatedResponse {
        magnitude_db: spectrum
            .iter()
            .map(|bin| {
                let magnitude = bin.norm();
                if magnitude > 0.0 {
                    20.0 * magnitude.log10()
                } else {
                    IR_FLOOR_DB
                }
            })
            .collect(),
        phase_degrees: spectrum.iter().map(|bin| bin.arg().to_degrees()).collect(),
        bin_spacing_hz: ir.sample_rate / fft_size as f32,
        resolution_hz: gate.resolution_hz(),
    })
}

/// Schroeder backward integration: the energy decay curve, in decibels.
///
/// Integrating the squared response *backwards* from the end is the trick that
/// makes reverberation time measurable from a single impulse. The raw squared
/// response is far too noisy to fit a line to; the reverse cumulative integral
/// of it is smooth, and its slope is the decay rate. Every RT60 measurement in
/// the field is built on this.
///
/// The curve is normalised to 0 dB at the start.
pub fn schroeder_decay(ir: &ImpulseResponse) -> Vec<f32> {
    let start = (ir.peak_samples.max(0.0)) as usize;
    let tail = ir.samples.get(start..).unwrap_or(&[]);
    if tail.is_empty() {
        return Vec::new();
    }

    // Reverse cumulative sum of energy.
    let mut curve = vec![0.0_f32; tail.len()];
    let mut running = 0.0_f64;
    for (slot, sample) in curve.iter_mut().zip(tail.iter()).rev() {
        running += f64::from(*sample) * f64::from(*sample);
        *slot = running as f32;
    }

    let total = curve.first().copied().unwrap_or(0.0);
    if total <= 0.0 {
        return vec![IR_FLOOR_DB; curve.len()];
    }
    for value in &mut curve {
        *value = if *value > 0.0 {
            (10.0 * (*value / total).log10()).max(IR_FLOOR_DB)
        } else {
            IR_FLOOR_DB
        };
    }
    curve
}

/// Reverberation time, estimated three ways.
///
/// All three are extrapolations to a 60 dB decay, because a real room almost
/// never has 60 dB of usable range above its noise floor. They are reported
/// separately rather than averaged: when they disagree, that disagreement is
/// the finding — a decay that is not a straight line means coupled spaces, a
/// noise floor reached too early, or a measurement not worth trusting.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct ReverbTime {
    /// Early decay time, from the first 10 dB. Correlates best with what a
    /// listener perceives as reverberance.
    pub edt: Option<f32>,
    /// From the -5 to -25 dB span, extrapolated by three.
    pub t20: Option<f32>,
    /// From the -5 to -35 dB span, extrapolated by two. Needs more range above
    /// the noise floor than T20 but is less sensitive to early reflections.
    pub t30: Option<f32>,
}

impl ReverbTime {
    /// The best available estimate, preferring the longest usable span.
    pub fn best(&self) -> Option<f32> {
        self.t30.or(self.t20).or(self.edt)
    }

    /// How far the estimates disagree, as a fraction of the largest.
    ///
    /// Above roughly 0.1 the decay is not a straight line and no single number
    /// describes it.
    pub fn spread(&self) -> Option<f32> {
        let values: Vec<f32> = [self.edt, self.t20, self.t30]
            .into_iter()
            .flatten()
            .collect();
        if values.len() < 2 {
            return None;
        }
        let max = values.iter().copied().fold(f32::MIN, f32::max);
        let min = values.iter().copied().fold(f32::MAX, f32::min);
        if max > 0.0 {
            Some((max - min) / max)
        } else {
            None
        }
    }
}

/// Estimate reverberation time from a decay curve.
pub fn reverb_time(decay_db: &[f32], sample_rate: f32) -> ReverbTime {
    ReverbTime {
        edt: fit_decay(decay_db, sample_rate, 0.0, -10.0).map(|seconds| seconds * 6.0),
        t20: fit_decay(decay_db, sample_rate, -5.0, -25.0).map(|seconds| seconds * 3.0),
        t30: fit_decay(decay_db, sample_rate, -5.0, -35.0).map(|seconds| seconds * 2.0),
    }
}

/// Fraction of the decay curve at the end that carries no information.
///
/// A backward integral necessarily terminates at negative infinity: the last
/// sample integrates only itself, so the curve falls off a cliff regardless of
/// what the room did. A truncated measurement therefore *appears* to reach -35 dB
/// even when the room only decayed by ten, and a fit that reaches into the cliff
/// reports a reverberation time that is pure artefact.
///
/// Excluding the tail is the cheap guard. The thorough answer is Lundeby's
/// method, which finds where the response meets the noise floor and truncates
/// the integration there; that is worth doing later, and this is worth doing now.
const TRUNCATION_GUARD: f32 = 0.9;

/// Seconds taken to fall from `from_db` to `to_db`, by least-squares fit.
///
/// A fit rather than just reading the two crossings: the crossings alone are at
/// the mercy of whatever noise sits on the curve at those two instants, while a
/// fit over the whole span uses every point between them.
///
/// Returns `None` if the span is not reached before the truncation guard, which
/// means the measurement does not have the range to support this estimate.
fn fit_decay(decay_db: &[f32], sample_rate: f32, from_db: f32, to_db: f32) -> Option<f32> {
    if sample_rate <= 0.0 || decay_db.len() < 4 {
        return None;
    }
    let usable = ((decay_db.len() as f32) * TRUNCATION_GUARD) as usize;
    let searchable = decay_db.get(..usable.max(4))?;

    let start = searchable.iter().position(|db| *db <= from_db)?;
    let end = searchable.iter().position(|db| *db <= to_db)?;
    if end <= start + 2 {
        return None;
    }
    let decay_db = searchable;

    // Least squares over the span, x in samples.
    let span = decay_db.get(start..=end)?;
    let n = span.len() as f64;
    let mut sum_x = 0.0_f64;
    let mut sum_y = 0.0_f64;
    let mut sum_xy = 0.0_f64;
    let mut sum_xx = 0.0_f64;
    for (index, value) in span.iter().enumerate() {
        let x = index as f64;
        let y = f64::from(*value);
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_xx += x * x;
    }
    let denominator = n * sum_xx - sum_x * sum_x;
    if denominator.abs() < 1e-12 {
        return None;
    }
    // Decibels per sample; negative for a decay.
    let slope = (n * sum_xy - sum_x * sum_y) / denominator;
    if slope >= 0.0 {
        return None;
    }

    let range = f64::from(from_db - to_db);
    Some((range / -slope / f64::from(sample_rate)) as f32)
}

/// Window kind recommended for transforming a gated response.
pub fn recommended_gate_window() -> WindowKind {
    // The gate itself does the tapering, so a second window would narrow the
    // effective gate and cost resolution for nothing.
    WindowKind::Rectangular
}

/// Build a window matching a gate's length, for callers that want the taper
/// separately from the gating.
pub fn gate_window(gate: &Gate, sample_rate: f32) -> Option<Window> {
    let length = (gate.length_seconds() * sample_rate) as usize;
    if length == 0 {
        return None;
    }
    let alpha = if gate.length_seconds() > 0.0 {
        (gate.fade_seconds / gate.length_seconds()).clamp(0.0, 1.0)
    } else {
        0.0
    };
    Some(Window::new(WindowKind::Tukey { alpha }, length))
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::{Generator, Signal};

    const RATE: f32 = 48_000.0;

    /// A synthetic decay with a known reverberation time.
    ///
    /// Amplitude falls as `e^(-t/tau)`, so energy falls at `8.686/tau` dB per
    /// second and a 60 dB fall takes `6.908 * tau`.
    fn decaying_noise(rt60: f32, seconds: f32, seed: u64) -> ImpulseResponse {
        let tau = rt60 / 6.908;
        let count = (RATE * seconds) as usize;

        let mut generator = Generator::new(RATE, Signal::WhiteNoise { amplitude: 1.0 }, seed);
        let mut samples = vec![0.0; count];
        generator.fill(&mut samples);

        for (index, sample) in samples.iter_mut().enumerate() {
            let t = index as f32 / RATE;
            *sample *= (-t / tau).exp();
        }

        ImpulseResponse {
            samples,
            peak_samples: 0.0,
            sample_rate: RATE,
        }
    }

    fn spike_at(delay: usize, length: usize) -> ImpulseResponse {
        let mut samples = vec![0.0; length];
        samples[delay] = 1.0;
        ImpulseResponse {
            samples,
            peak_samples: delay as f32,
            sample_rate: RATE,
        }
    }

    /// The headline: a synthetic decay with a known RT60 must measure as that.
    #[test]
    fn a_known_decay_measures_its_reverberation_time() {
        for target in [0.3_f32, 0.6, 1.2] {
            let ir = decaying_noise(target, target * 3.0, 1);
            let decay = schroeder_decay(&ir);
            let rt = reverb_time(&decay, RATE);

            let t20 = rt.t20.unwrap_or(0.0);
            let t30 = rt.t30.unwrap_or(0.0);
            assert!(
                (t20 - target).abs() / target < 0.08,
                "T20 read {t20:.3} for a {target:.3} s decay"
            );
            assert!(
                (t30 - target).abs() / target < 0.08,
                "T30 read {t30:.3} for a {target:.3} s decay"
            );
        }
    }

    /// For a pure exponential the three estimates describe the same line, so
    /// they must agree. Disagreement is the diagnostic for a decay that is not
    /// straight, and it should not fire on one that is.
    #[test]
    fn the_estimates_agree_for_a_straight_decay() {
        let ir = decaying_noise(0.8, 3.0, 2);
        let rt = reverb_time(&schroeder_decay(&ir), RATE);

        assert!(rt.edt.is_some() && rt.t20.is_some() && rt.t30.is_some());
        let spread = rt.spread().unwrap();
        assert!(spread < 0.1, "estimates spread by {spread:.3}: {rt:?}");
        assert!((rt.best().unwrap() - 0.8).abs() < 0.08);
    }

    #[test]
    fn the_decay_curve_starts_at_zero_and_falls_monotonically() {
        let ir = decaying_noise(0.5, 2.0, 3);
        let decay = schroeder_decay(&ir);

        assert!(decay[0].abs() < 1e-4, "curve should start at 0 dB");
        for pair in decay.windows(2) {
            assert!(
                pair[1] <= pair[0] + 1e-4,
                "backward integration cannot increase: {} -> {}",
                pair[0],
                pair[1]
            );
        }
    }

    #[test]
    fn silence_produces_no_reverberation_estimate() {
        let ir = ImpulseResponse {
            samples: vec![0.0; 4096],
            peak_samples: 0.0,
            sample_rate: RATE,
        };
        let rt = reverb_time(&schroeder_decay(&ir), RATE);
        assert!(rt.best().is_none());
        assert!(rt.spread().is_none());
    }

    /// A decay that never falls far enough cannot yield T30, and must say so
    /// rather than reporting the truncation artefact as a reverberation time.
    ///
    /// This is subtler than it looks. The backward integral always terminates at
    /// negative infinity, so a truncated curve *does* pass -35 dB - just at the
    /// very end, and for reasons that have nothing to do with the room.
    #[test]
    fn a_truncated_decay_does_not_report_the_cliff_as_reverberation() {
        // A 2 s reverberation time recorded for only 0.35 s: about 10 dB of real
        // decay, then the integral falls off its cliff.
        let ir = decaying_noise(2.0, 0.35, 4);
        let decay = schroeder_decay(&ir);

        // The curve genuinely does reach -35 dB, which is the trap.
        assert!(
            decay.iter().any(|db| *db <= -35.0),
            "the terminal plunge should be present in the raw curve"
        );

        let rt = reverb_time(&decay, RATE);
        assert!(
            rt.t30.is_none(),
            "T30 must not be taken from the truncation cliff, got {:?}",
            rt.t30
        );
    }

    #[test]
    fn gating_keeps_only_the_requested_span() {
        let ir = spike_at(1000, 8192);
        let gate = Gate {
            start_seconds: -0.001,
            end_seconds: 0.005,
            fade_seconds: 0.001,
        };
        let gated = apply_gate(&ir, &gate);

        let expected = ((0.005 + 0.001) * RATE) as usize;
        assert!(
            (gated.len() as isize - expected as isize).abs() < 4,
            "kept {} samples, expected about {expected}",
            gated.len()
        );
    }

    /// The point of gating: a late reflection outside the gate must not appear
    /// in the response.
    #[test]
    fn gating_excludes_a_late_reflection() {
        let mut ir = spike_at(1000, 8192);
        // A strong reflection 10 ms later.
        ir.samples[1000 + (0.010 * RATE) as usize] = 0.8;

        let tight = Gate::anechoic(0.005);
        let wide = Gate::anechoic(0.020);

        let inside: f32 = apply_gate(&ir, &tight).iter().map(|s| s.abs()).sum();
        let outside: f32 = apply_gate(&ir, &wide).iter().map(|s| s.abs()).sum();
        assert!(
            outside > inside + 0.5,
            "the wider gate should include the reflection: {inside} vs {outside}"
        );
    }

    /// The cost of gating, stated rather than hidden. A 5 ms gate cannot resolve
    /// below 200 Hz, which is why quasi-anechoic measurements are spliced in the
    /// bass.
    #[test]
    fn a_gate_reports_the_resolution_it_costs() {
        let gate = Gate {
            start_seconds: 0.0,
            end_seconds: 0.005,
            fade_seconds: 0.001,
        };
        assert!((gate.length_seconds() - 0.005).abs() < 1e-6);
        assert!((gate.resolution_hz() - 200.0).abs() < 0.1);

        let long = Gate {
            start_seconds: 0.0,
            end_seconds: 0.5,
            fade_seconds: 0.01,
        };
        assert!((long.resolution_hz() - 2.0).abs() < 0.01);
    }

    /// A single spike is a flat system, so its gated response must be flat.
    #[test]
    fn a_spike_has_a_flat_gated_response() {
        let ir = spike_at(500, 8192);
        let response = gated_response(&ir, &Gate::anechoic(0.010), 4096).unwrap();

        // Check well above the gate's resolution limit.
        let first = (response.resolution_hz * 2.0 / response.bin_spacing_hz) as usize;
        let levels = &response.magnitude_db[first..response.magnitude_db.len() / 2];
        let mean: f32 = levels.iter().sum::<f32>() / levels.len() as f32;
        for (index, level) in levels.iter().enumerate() {
            assert!(
                (level - mean).abs() < 0.5,
                "bin {index} deviates: {level} vs mean {mean}"
            );
        }
    }

    #[test]
    fn the_response_reports_which_frequencies_to_believe() {
        let ir = spike_at(500, 8192);
        let response = gated_response(&ir, &Gate::anechoic(0.005), 4096).unwrap();

        assert!(!response.is_trustworthy(50.0), "below a 5 ms gate's limit");
        assert!(response.is_trustworthy(1000.0));
        assert!((response.bin_spacing_hz - RATE / 4096.0).abs() < 1e-3);
    }

    #[test]
    fn degenerate_gates_and_sizes_are_refused() {
        let ir = spike_at(500, 8192);

        // Closes before it opens.
        let backwards = Gate {
            start_seconds: 0.010,
            end_seconds: 0.001,
            fade_seconds: 0.0,
        };
        assert!(apply_gate(&ir, &backwards).is_empty());
        assert!(gated_response(&ir, &backwards, 4096).is_none());

        // Odd transform size.
        assert!(gated_response(&ir, &Gate::anechoic(0.005), 4095).is_none());
    }

    #[test]
    fn an_empty_impulse_response_gates_to_nothing() {
        let ir = ImpulseResponse {
            samples: Vec::new(),
            peak_samples: 0.0,
            sample_rate: RATE,
        };
        assert!(apply_gate(&ir, &Gate::anechoic(0.005)).is_empty());
        assert!(schroeder_decay(&ir).is_empty());
    }

    /// The closing taper is what stops a hard truncation smearing the response.
    #[test]
    fn the_gate_tapers_its_closing_edge() {
        let ir = ImpulseResponse {
            samples: vec![1.0; 8192],
            peak_samples: 0.0,
            sample_rate: RATE,
        };
        let gate = Gate {
            start_seconds: 0.0,
            end_seconds: 0.010,
            fade_seconds: 0.004,
        };
        let gated = apply_gate(&ir, &gate);

        assert!((gated[0] - 1.0).abs() < 1e-6, "the open edge is untouched");
        assert!(
            gated.last().unwrap().abs() < 0.01,
            "the closing edge should reach zero, got {}",
            gated.last().unwrap()
        );
        // And it should be a smooth ramp, not a step.
        let middle = gated[gated.len() - gated.len() / 8];
        assert!(middle > 0.0 && middle < 1.0, "mid-fade was {middle}");
    }

    #[test]
    fn a_gate_window_matches_the_gate_length() {
        let gate = Gate {
            start_seconds: 0.0,
            end_seconds: 0.010,
            fade_seconds: 0.002,
        };
        let window = gate_window(&gate, RATE).unwrap();
        assert_eq!(window.size(), (0.010 * RATE) as usize);
        assert_eq!(recommended_gate_window(), WindowKind::Rectangular);
    }
}
