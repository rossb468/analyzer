//! Second-order sections.
//!
//! One biquad implementation for the whole crate. The weighting filters in
//! [`crate::meter`] and the equaliser in [`crate::eq`] both build on it, which
//! matters more than it sounds: a filter that measures differently from how it
//! sounds is the single most confusing bug an audio tool can have, and two
//! implementations is how that happens.
//!
//! # Design formulas
//!
//! The equaliser sections follow Robert Bristow-Johnson's Audio EQ Cookbook,
//! which is the de facto standard for parametric EQ and what every other tool
//! in this space implements. Matching it is deliberate: a filter exported to a
//! miniDSP or a Behringer unit has to mean there what it meant here.

use std::f32::consts::TAU;

use crate::Complex32;

/// A second-order section in transposed direct form II.
///
/// Transposed form II is chosen over direct form I for its numerical
/// behaviour with `f32` coefficients: it keeps two state variables instead of
/// four and puts the accumulation where rounding hurts least, which matters at
/// low frequencies where the poles crowd the unit circle.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Biquad {
    /// Feed-forward coefficients, already normalised by `a0`.
    pub b0: f32,
    pub b1: f32,
    pub b2: f32,
    /// Feedback coefficients, already normalised by `a0`.
    pub a1: f32,
    pub a2: f32,
    s1: f32,
    s2: f32,
}

impl Default for Biquad {
    /// A pass-through.
    fn default() -> Self {
        Self::IDENTITY
    }
}

impl Biquad {
    /// A section that changes nothing.
    pub const IDENTITY: Self = Self {
        b0: 1.0,
        b1: 0.0,
        b2: 0.0,
        a1: 0.0,
        a2: 0.0,
        s1: 0.0,
        s2: 0.0,
    };

    /// Build from coefficients that are already normalised by `a0`.
    pub fn new(b0: f32, b1: f32, b2: f32, a1: f32, a2: f32) -> Self {
        Self {
            b0,
            b1,
            b2,
            a1,
            a2,
            s1: 0.0,
            s2: 0.0,
        }
    }

    /// Build from raw coefficients, dividing through by `a0`.
    ///
    /// Returns [`Self::IDENTITY`] for a zero or non-finite `a0`, because a
    /// degenerate design must not become a filter full of infinities that then
    /// poisons everything downstream.
    pub fn normalised(b0: f32, b1: f32, b2: f32, a0: f32, a1: f32, a2: f32) -> Self {
        if !a0.is_finite() || a0.abs() < f32::EPSILON {
            return Self::IDENTITY;
        }
        Self::new(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)
    }

    /// Filter one sample.
    #[inline]
    pub fn process(&mut self, x: f32) -> f32 {
        let y = self.b0 * x + self.s1;
        self.s1 = self.b1 * x - self.a1 * y + self.s2;
        self.s2 = self.b2 * x - self.a2 * y;
        y
    }

    /// Filter a block in place.
    #[inline]
    pub fn process_block(&mut self, samples: &mut [f32]) {
        for sample in samples {
            *sample = self.process(*sample);
        }
    }

    /// Clear the delay line without touching the coefficients.
    pub fn reset(&mut self) {
        self.s1 = 0.0;
        self.s2 = 0.0;
    }

    /// Complex frequency response at `hz`.
    pub fn response_at(&self, hz: f32, sample_rate: f32) -> Complex32 {
        if sample_rate <= 0.0 {
            return Complex32::new(1.0, 0.0);
        }
        let w = TAU * hz / sample_rate;
        let z1 = Complex32::from_polar(1.0, -w);
        let z2 = z1 * z1;
        let numerator = Complex32::new(self.b0, 0.0) + z1 * self.b1 + z2 * self.b2;
        let denominator = Complex32::new(1.0, 0.0) + z1 * self.a1 + z2 * self.a2;
        if denominator.norm_sqr() > 0.0 {
            numerator / denominator
        } else {
            Complex32::new(0.0, 0.0)
        }
    }

    /// Magnitude response at `hz`, as a linear ratio.
    pub fn magnitude_at(&self, hz: f32, sample_rate: f32) -> f32 {
        self.response_at(hz, sample_rate).norm()
    }

    // ---------------------------------------------------------------- RBJ --

    /// Peaking EQ: a bump or dip centred on `hz`, flat either side.
    ///
    /// The workhorse of a parametric equaliser, and the only shape that can
    /// correct a room mode without disturbing its neighbours.
    pub fn peaking(hz: f32, q: f32, gain_db: f32, sample_rate: f32) -> Self {
        let Some(design) = Design::new(hz, q, sample_rate) else {
            return Self::IDENTITY;
        };
        let a = amplitude(gain_db);
        Self::normalised(
            1.0 + design.alpha * a,
            -2.0 * design.cos_w,
            1.0 - design.alpha * a,
            1.0 + design.alpha / a,
            -2.0 * design.cos_w,
            1.0 - design.alpha / a,
        )
    }

    /// Low shelf: everything below `hz` lifted or cut by `gain_db`.
    pub fn low_shelf(hz: f32, q: f32, gain_db: f32, sample_rate: f32) -> Self {
        let Some(design) = Design::new(hz, q, sample_rate) else {
            return Self::IDENTITY;
        };
        let a = amplitude(gain_db);
        let sqrt_a = a.sqrt();
        let two_sqrt_a_alpha = 2.0 * sqrt_a * design.alpha;
        Self::normalised(
            a * ((a + 1.0) - (a - 1.0) * design.cos_w + two_sqrt_a_alpha),
            2.0 * a * ((a - 1.0) - (a + 1.0) * design.cos_w),
            a * ((a + 1.0) - (a - 1.0) * design.cos_w - two_sqrt_a_alpha),
            (a + 1.0) + (a - 1.0) * design.cos_w + two_sqrt_a_alpha,
            -2.0 * ((a - 1.0) + (a + 1.0) * design.cos_w),
            (a + 1.0) + (a - 1.0) * design.cos_w - two_sqrt_a_alpha,
        )
    }

    /// High shelf: everything above `hz` lifted or cut by `gain_db`.
    pub fn high_shelf(hz: f32, q: f32, gain_db: f32, sample_rate: f32) -> Self {
        let Some(design) = Design::new(hz, q, sample_rate) else {
            return Self::IDENTITY;
        };
        let a = amplitude(gain_db);
        let sqrt_a = a.sqrt();
        let two_sqrt_a_alpha = 2.0 * sqrt_a * design.alpha;
        Self::normalised(
            a * ((a + 1.0) + (a - 1.0) * design.cos_w + two_sqrt_a_alpha),
            -2.0 * a * ((a - 1.0) + (a + 1.0) * design.cos_w),
            a * ((a + 1.0) + (a - 1.0) * design.cos_w - two_sqrt_a_alpha),
            (a + 1.0) - (a - 1.0) * design.cos_w + two_sqrt_a_alpha,
            2.0 * ((a - 1.0) - (a + 1.0) * design.cos_w),
            (a + 1.0) - (a - 1.0) * design.cos_w - two_sqrt_a_alpha,
        )
    }

    /// Second-order lowpass, -3 dB at `hz` for `q` of 1/sqrt(2).
    pub fn low_pass(hz: f32, q: f32, sample_rate: f32) -> Self {
        let Some(design) = Design::new(hz, q, sample_rate) else {
            return Self::IDENTITY;
        };
        let shared = 1.0 - design.cos_w;
        Self::normalised(
            shared * 0.5,
            shared,
            shared * 0.5,
            1.0 + design.alpha,
            -2.0 * design.cos_w,
            1.0 - design.alpha,
        )
    }

    /// Second-order highpass.
    pub fn high_pass(hz: f32, q: f32, sample_rate: f32) -> Self {
        let Some(design) = Design::new(hz, q, sample_rate) else {
            return Self::IDENTITY;
        };
        let shared = 1.0 + design.cos_w;
        Self::normalised(
            shared * 0.5,
            -shared,
            shared * 0.5,
            1.0 + design.alpha,
            -2.0 * design.cos_w,
            1.0 - design.alpha,
        )
    }

    /// Bandpass with unity gain at the centre.
    pub fn band_pass(hz: f32, q: f32, sample_rate: f32) -> Self {
        let Some(design) = Design::new(hz, q, sample_rate) else {
            return Self::IDENTITY;
        };
        Self::normalised(
            design.alpha,
            0.0,
            -design.alpha,
            1.0 + design.alpha,
            -2.0 * design.cos_w,
            1.0 - design.alpha,
        )
    }

    /// Notch: a null at `hz`, unity elsewhere.
    pub fn notch(hz: f32, q: f32, sample_rate: f32) -> Self {
        let Some(design) = Design::new(hz, q, sample_rate) else {
            return Self::IDENTITY;
        };
        Self::normalised(
            1.0,
            -2.0 * design.cos_w,
            1.0,
            1.0 + design.alpha,
            -2.0 * design.cos_w,
            1.0 - design.alpha,
        )
    }

    /// Allpass: flat magnitude, phase rotated through 360 degrees about `hz`.
    ///
    /// Useful for time alignment between drivers, where the goal is to move
    /// phase without touching level.
    pub fn all_pass(hz: f32, q: f32, sample_rate: f32) -> Self {
        let Some(design) = Design::new(hz, q, sample_rate) else {
            return Self::IDENTITY;
        };
        Self::normalised(
            1.0 - design.alpha,
            -2.0 * design.cos_w,
            1.0 + design.alpha,
            1.0 + design.alpha,
            -2.0 * design.cos_w,
            1.0 - design.alpha,
        )
    }

    // ------------------------------------------------- weighting building --

    /// Two zeros at DC and a double pole at `omega`, bilinear-transformed.
    ///
    /// `H(s) = s^2 / (s + w)^2`, the block both weighting curves are made of.
    pub fn double_pole_highpass(omega: f32, sample_rate: f32) -> Self {
        let c = 2.0 * sample_rate;
        let omega = prewarp(omega, sample_rate);
        let a = c + omega;
        let b = omega - c;
        let gain = (c * c) / (a * a);
        Self::new(gain, -2.0 * gain, gain, 2.0 * b / a, (b * b) / (a * a))
    }

    /// Two real poles and two zeros at Nyquist, bilinear-transformed.
    ///
    /// `H(s) = 1 / ((s + w1)(s + w2))`.
    pub fn two_pole_lowpass(omega_a: f32, omega_b: f32, sample_rate: f32) -> Self {
        let c = 2.0 * sample_rate;
        let omega_a = prewarp(omega_a, sample_rate);
        let omega_b = prewarp(omega_b, sample_rate);
        let (pa, qa) = (c + omega_a, omega_a - c);
        let (pb, qb) = (c + omega_b, omega_b - c);
        let norm = pa * pb;
        Self::new(
            1.0 / norm,
            2.0 / norm,
            1.0 / norm,
            (pa * qb + pb * qa) / norm,
            (qa * qb) / norm,
        )
    }
}

/// The three quantities every cookbook formula shares.
struct Design {
    cos_w: f32,
    alpha: f32,
}

impl Design {
    /// Returns `None` for a design that cannot be realised.
    ///
    /// A centre frequency at or above Nyquist has no meaning, and the caller
    /// gets a pass-through rather than a filter full of NaNs. This is not
    /// theoretical: an equaliser preset written at 96 kHz and loaded at 44.1
    /// puts its top band above Nyquist, and silently producing NaNs there would
    /// take the whole output with it.
    fn new(hz: f32, q: f32, sample_rate: f32) -> Option<Self> {
        if !hz.is_finite() || !q.is_finite() || sample_rate <= 0.0 {
            return None;
        }
        // Just under Nyquist. Exactly at it, sin(w) is zero and alpha collapses.
        let nyquist = sample_rate * 0.5;
        if hz <= 0.0 || hz >= nyquist * 0.999 {
            return None;
        }
        let q = q.max(1e-3);
        let w = TAU * hz / sample_rate;
        let (sin_w, cos_w) = w.sin_cos();
        Some(Self {
            cos_w,
            alpha: sin_w / (2.0 * q),
        })
    }
}

/// Cookbook `A`: the square root of the linear gain, because a peaking filter
/// applies it twice.
fn amplitude(gain_db: f32) -> f32 {
    10.0_f32.powf(gain_db / 40.0)
}

/// Pre-warp an analog pole so the bilinear transform lands it on the intended
/// digital frequency.
///
/// The bilinear transform compresses the frequency axis towards Nyquist. The
/// 12194 Hz pole is halfway there at a 48 kHz sample rate, so without this it
/// ends up well below where it belongs and C-weighting reads about 0.6 dB low at
/// 8 kHz - inside the class 1 tolerance, but wrong for no good reason.
pub fn prewarp(omega: f32, sample_rate: f32) -> f32 {
    let c = 2.0 * sample_rate;
    // Guard against the tangent blowing up for a pole at or beyond Nyquist.
    let normalised = (omega / c).clamp(0.0, 1.55);
    c * normalised.tan()
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::indexing_slicing)]
mod tests {
    use super::*;

    const RATE: f32 = 48_000.0;

    fn db(linear: f32) -> f32 {
        20.0 * linear.max(1e-12).log10()
    }

    /// The defining property: a peaking filter hits its gain at the centre and
    /// leaves the ends alone.
    #[test]
    fn a_peaking_filter_hits_its_gain_at_the_centre() {
        for gain in [-12.0_f32, -6.0, 3.0, 9.0] {
            let filter = Biquad::peaking(1000.0, 2.0, gain, RATE);
            let centre = db(filter.magnitude_at(1000.0, RATE));
            assert!(
                (centre - gain).abs() < 0.01,
                "asked for {gain} dB, measured {centre}"
            );
            assert!(db(filter.magnitude_at(20.0, RATE)).abs() < 0.2);
            assert!(db(filter.magnitude_at(18_000.0, RATE)).abs() < 0.2);
        }
    }

    /// Q sets the width, and a higher Q must be narrower.
    #[test]
    fn a_higher_q_is_narrower() {
        let wide = Biquad::peaking(1000.0, 0.7, 12.0, RATE);
        let narrow = Biquad::peaking(1000.0, 8.0, 12.0, RATE);
        // An octave away from the centre.
        let at_wide = db(wide.magnitude_at(2000.0, RATE));
        let at_narrow = db(narrow.magnitude_at(2000.0, RATE));
        assert!(
            at_wide > at_narrow + 3.0,
            "wide {at_wide} should still be lifted where narrow {at_narrow} has decayed"
        );
    }

    /// The half-gain point defines Q for a peaking filter: at the edges of the
    /// bandwidth the response should be half the peak gain in decibels.
    #[test]
    fn the_bandwidth_edges_sit_at_half_gain() {
        let gain = 12.0_f32;
        let q = 1.414_f32;
        let filter = Biquad::peaking(1000.0, q, gain, RATE);
        // Bandwidth in octaves for this Q, from the cookbook's own relation
        // 1/Q = 2*sinh(ln(2)/2 * BW). Q of 1.414 is one octave, which is what
        // makes it the conventional choice for a graphic EQ.
        let bw = 2.0 * (1.0 / (2.0 * q)).asinh() / 2.0_f32.ln();
        assert!(
            (bw - 1.0).abs() < 0.01,
            "Q {q} should be one octave, got {bw}"
        );
        let upper = 1000.0 * 2.0_f32.powf(bw / 2.0);
        let measured = db(filter.magnitude_at(upper, RATE));
        assert!(
            (measured - gain / 2.0).abs() < 0.5,
            "at the band edge expected {} dB, measured {measured}",
            gain / 2.0
        );
    }

    /// Shelves must reach their gain in the shelf and unity in the passband.
    #[test]
    fn shelves_reach_their_gain() {
        let low = Biquad::low_shelf(200.0, 0.707, 6.0, RATE);
        assert!((db(low.magnitude_at(20.0, RATE)) - 6.0).abs() < 0.3);
        assert!(db(low.magnitude_at(10_000.0, RATE)).abs() < 0.3);
        // The corner of a shelf is the half-gain point, by definition.
        assert!((db(low.magnitude_at(200.0, RATE)) - 3.0).abs() < 0.3);

        let high = Biquad::high_shelf(4000.0, 0.707, -8.0, RATE);
        assert!((db(high.magnitude_at(20_000.0, RATE)) + 8.0).abs() < 0.5);
        assert!(db(high.magnitude_at(50.0, RATE)).abs() < 0.3);
    }

    /// A lowpass at Q = 1/sqrt(2) is Butterworth: -3 dB at the corner.
    #[test]
    fn a_butterworth_lowpass_is_three_decibels_down_at_the_corner() {
        let filter = Biquad::low_pass(1000.0, std::f32::consts::FRAC_1_SQRT_2, RATE);
        assert!((db(filter.magnitude_at(1000.0, RATE)) + 3.01).abs() < 0.1);
        assert!(db(filter.magnitude_at(20.0, RATE)).abs() < 0.01);
        // Two poles: 12 dB per octave above the corner. Measured well below
        // Nyquist, because the double zero there steepens the digital slope -
        // at 8 kHz against a 48 kHz rate it already reads over 13 dB.
        let one = db(filter.magnitude_at(2000.0, RATE));
        let two = db(filter.magnitude_at(4000.0, RATE));
        assert!((one - two - 12.0).abs() < 0.5, "{one} then {two}");
    }

    #[test]
    fn a_highpass_mirrors_the_lowpass() {
        let filter = Biquad::high_pass(1000.0, std::f32::consts::FRAC_1_SQRT_2, RATE);
        assert!((db(filter.magnitude_at(1000.0, RATE)) + 3.01).abs() < 0.1);
        assert!(db(filter.magnitude_at(20_000.0, RATE)).abs() < 0.1);
    }

    #[test]
    fn a_notch_nulls_its_centre() {
        let filter = Biquad::notch(1000.0, 4.0, RATE);
        assert!(db(filter.magnitude_at(1000.0, RATE)) < -60.0);
        assert!(db(filter.magnitude_at(200.0, RATE)).abs() < 0.5);
        assert!(db(filter.magnitude_at(5000.0, RATE)).abs() < 0.5);
    }

    #[test]
    fn a_bandpass_peaks_at_unity() {
        let filter = Biquad::band_pass(1000.0, 2.0, RATE);
        assert!(db(filter.magnitude_at(1000.0, RATE)).abs() < 0.01);
        assert!(db(filter.magnitude_at(50.0, RATE)) < -20.0);
        assert!(db(filter.magnitude_at(20_000.0, RATE)) < -20.0);
    }

    /// An allpass must be exactly flat and must still rotate phase.
    #[test]
    fn an_allpass_is_flat_but_turns_the_phase() {
        let filter = Biquad::all_pass(1000.0, 0.707, RATE);
        for hz in [20.0_f32, 200.0, 1000.0, 5000.0, 20_000.0] {
            assert!(
                db(filter.magnitude_at(hz, RATE)).abs() < 0.01,
                "allpass was not flat at {hz} Hz"
            );
        }
        let phase = filter.response_at(1000.0, RATE).arg().to_degrees();
        assert!(
            (phase.abs() - 180.0).abs() < 1.0,
            "expected half a turn at the centre, got {phase}"
        );
    }

    /// Zero gain must produce an exact pass-through, not something close to one.
    /// A band left at 0 dB is the common case and must cost nothing.
    #[test]
    fn zero_gain_is_a_pass_through() {
        let filter = Biquad::peaking(1000.0, 2.0, 0.0, RATE);
        for hz in [20.0_f32, 1000.0, 20_000.0] {
            assert!((filter.magnitude_at(hz, RATE) - 1.0).abs() < 1e-5);
        }
    }

    /// A frequency above Nyquist is unrealisable, and the answer must be a
    /// pass-through rather than a filter full of NaNs.
    #[test]
    fn an_unrealisable_design_becomes_a_pass_through() {
        for filter in [
            Biquad::peaking(30_000.0, 2.0, 6.0, RATE),
            Biquad::peaking(0.0, 2.0, 6.0, RATE),
            Biquad::peaking(f32::NAN, 2.0, 6.0, RATE),
            Biquad::low_pass(1000.0, 2.0, 0.0),
        ] {
            assert_eq!(filter, Biquad::IDENTITY);
        }
    }

    /// The measured response of the running filter must match the computed one.
    /// This is the check that catches a coefficient typo, which a magnitude
    /// formula sharing the same typo would not.
    #[test]
    fn the_running_filter_matches_its_computed_response() {
        for (name, mut filter, hz) in [
            ("peak", Biquad::peaking(1000.0, 1.5, 9.0, RATE), 1000.0_f32),
            ("shelf", Biquad::low_shelf(300.0, 0.7, -6.0, RATE), 60.0),
            ("lowpass", Biquad::low_pass(2000.0, 0.707, RATE), 4000.0),
            ("highpass", Biquad::high_pass(500.0, 0.707, RATE), 200.0),
        ] {
            let predicted = db(filter.magnitude_at(hz, RATE));

            // Run a tone through and measure the settled level.
            //
            // RMS, not peak. Sampling a sine rarely lands on its crest, so the
            // largest sample understates the amplitude by up to
            // 1 - cos(pi/samples_per_cycle) - 0.3 dB at 4 kHz here, which is
            // twice the tolerance this test is trying to enforce.
            let frames = 48_000;
            let mut sum = 0.0_f64;
            let mut counted = 0_u32;
            for frame in 0..frames {
                let x = (TAU * hz * frame as f32 / RATE).sin();
                let y = filter.process(x);
                // Skip the transient; the tail is the steady state.
                if frame > frames / 2 {
                    sum += f64::from(y) * f64::from(y);
                    counted += 1;
                }
            }
            let rms = (sum / f64::from(counted)).sqrt() as f32;
            // Referenced to the input's RMS, which for a unit sine is 1/sqrt(2).
            let measured = db(rms / std::f32::consts::FRAC_1_SQRT_2);
            assert!(
                (measured - predicted).abs() < 0.15,
                "{name}: computed {predicted} dB but measured {measured} dB"
            );
        }
    }

    /// Resetting must clear the tail without changing the filter.
    #[test]
    fn reset_clears_the_state_but_not_the_coefficients() {
        let mut filter = Biquad::low_pass(500.0, 0.707, RATE);
        let before = filter;
        for _ in 0..100 {
            filter.process(1.0);
        }
        filter.reset();
        assert_eq!(
            filter.process(0.0),
            0.0,
            "a cleared filter must output zero"
        );
        assert_eq!(filter.b0, before.b0);
        assert_eq!(filter.a1, before.a1);
    }
}
