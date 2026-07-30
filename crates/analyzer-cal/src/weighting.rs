//! Frequency weighting curves from IEC 61672-1.
//!
//! Weighting exists because the ear is not a flat measuring instrument. A-weighting
//! approximates the 40-phon equal-loudness contour and is what noise regulations
//! are written against; C-weighting is much flatter and is used for peak levels
//! and low-frequency work; Z is no weighting at all.
//!
//! The formulae here are the standard pole-based approximations, each normalised
//! to exactly 0 dB at 1 kHz — which is the definition, not a convenience.

/// Which weighting to apply.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Weighting {
    /// No weighting. The honest choice for measurement work.
    #[default]
    Z,
    /// A-weighting: rolls off hard below 500 Hz, matching how insensitive
    /// hearing is to bass at moderate levels.
    A,
    /// C-weighting: nearly flat through the audio band, rolling off outside it.
    C,
}

/// Pole frequencies from IEC 61672-1.
const F1: f32 = 20.598_997;
const F2: f32 = 107.652_65;
const F3: f32 = 737.862_23;
const F4: f32 = 12_194.217;

/// Normalisation constants making each curve exactly 0 dB at 1 kHz.
const A_OFFSET: f32 = 2.0;
const C_OFFSET: f32 = 0.062;

impl Weighting {
    /// Weighting in decibels at `hz`.
    ///
    /// Returns a large negative value at or below zero hertz rather than a NaN,
    /// so a caller summing weighted bins cannot poison its total with DC.
    pub fn db_at(self, hz: f32) -> f32 {
        if hz <= 0.0 {
            return -200.0;
        }
        match self {
            Weighting::Z => 0.0,
            Weighting::A => a_weighting_db(hz),
            Weighting::C => c_weighting_db(hz),
        }
    }

    /// Short label for display.
    pub fn label(self) -> &'static str {
        match self {
            Weighting::Z => "Z",
            Weighting::A => "A",
            Weighting::C => "C",
        }
    }
}

fn a_weighting_db(hz: f32) -> f32 {
    let f2 = hz * hz;
    let numerator = F4 * F4 * f2 * f2;
    let denominator = (f2 + F1 * F1) * ((f2 + F2 * F2) * (f2 + F3 * F3)).sqrt() * (f2 + F4 * F4);
    if denominator <= 0.0 {
        return -200.0;
    }
    20.0 * (numerator / denominator).log10() + A_OFFSET
}

fn c_weighting_db(hz: f32) -> f32 {
    let f2 = hz * hz;
    let numerator = F4 * F4 * f2;
    let denominator = (f2 + F1 * F1) * (f2 + F4 * F4);
    if denominator <= 0.0 {
        return -200.0;
    }
    20.0 * (numerator / denominator).log10() + C_OFFSET
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Reference values from IEC 61672-1 Table 3. Tolerances match the class 1
    /// limits at each frequency, which is the only meaningful standard to hold
    /// an implementation of this to.
    #[test]
    fn a_weighting_matches_the_standard() {
        let cases = [
            (10.0, -70.4, 0.6),
            (20.0, -50.5, 0.4),
            (50.0, -30.2, 0.3),
            (100.0, -19.1, 0.2),
            (200.0, -10.9, 0.2),
            (500.0, -3.2, 0.2),
            (1000.0, 0.0, 0.05),
            (2000.0, 1.2, 0.2),
            (5000.0, 0.5, 0.2),
            (10_000.0, -2.5, 0.3),
            (20_000.0, -9.3, 0.5),
        ];
        for (hz, expected, tolerance) in cases {
            let got = Weighting::A.db_at(hz);
            assert!(
                (got - expected).abs() < tolerance,
                "A at {hz} Hz: got {got:.2}, standard says {expected}"
            );
        }
    }

    #[test]
    fn c_weighting_matches_the_standard() {
        let cases = [
            (10.0, -14.3, 0.3),
            (20.0, -6.2, 0.2),
            (50.0, -1.3, 0.1),
            (100.0, -0.3, 0.1),
            (1000.0, 0.0, 0.05),
            (5000.0, -1.3, 0.1),
            (10_000.0, -4.4, 0.2),
            (20_000.0, -11.2, 0.4),
        ];
        for (hz, expected, tolerance) in cases {
            let got = Weighting::C.db_at(hz);
            assert!(
                (got - expected).abs() < tolerance,
                "C at {hz} Hz: got {got:.2}, standard says {expected}"
            );
        }
    }

    /// The defining property of both curves.
    #[test]
    fn both_curves_are_exactly_zero_at_one_kilohertz() {
        assert!(Weighting::A.db_at(1000.0).abs() < 0.05);
        assert!(Weighting::C.db_at(1000.0).abs() < 0.05);
    }

    #[test]
    fn z_weighting_is_flat_everywhere() {
        for hz in [1.0, 20.0, 1000.0, 20_000.0, 96_000.0] {
            assert_eq!(Weighting::Z.db_at(hz), 0.0);
        }
    }

    /// A must attenuate the bass far harder than C. Getting the two swapped is
    /// an easy mistake and this catches it immediately.
    #[test]
    fn a_attenuates_bass_much_harder_than_c() {
        for hz in [10.0, 20.0, 50.0, 100.0] {
            let a = Weighting::A.db_at(hz);
            let c = Weighting::C.db_at(hz);
            assert!(
                a < c - 5.0,
                "at {hz} Hz: A {a:.1} should be well below C {c:.1}"
            );
        }
    }

    #[test]
    fn dc_and_negative_frequencies_return_a_floor_not_a_nan() {
        for weighting in [Weighting::A, Weighting::C, Weighting::Z] {
            assert!(weighting.db_at(0.0).is_finite());
            assert!(weighting.db_at(-100.0).is_finite());
        }
        assert!(Weighting::A.db_at(0.0) < -100.0);
    }

    #[test]
    fn curves_stay_finite_across_the_whole_band() {
        for step in 1..200_000 {
            let hz = step as f32 * 0.5;
            assert!(Weighting::A.db_at(hz).is_finite(), "A broke at {hz}");
            assert!(Weighting::C.db_at(hz).is_finite(), "C broke at {hz}");
        }
    }

    #[test]
    fn labels_are_stable() {
        assert_eq!(Weighting::A.label(), "A");
        assert_eq!(Weighting::C.label(), "C");
        assert_eq!(Weighting::Z.label(), "Z");
    }
}
