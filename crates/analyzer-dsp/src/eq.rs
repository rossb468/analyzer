//! Equalisation: a cascade of second-order sections.
//!
//! Two shapes of the same thing. A **graphic** equaliser is a fixed set of
//! bands at standard centre frequencies where only the gains move; a
//! **parametric** equaliser lets every band choose its own frequency, gain,
//! Q and type. They share one implementation because they are one thing - a
//! graphic EQ is a parametric EQ with the other controls nailed down - and
//! keeping them separate is how the two would drift apart.
//!
//! # What this is for
//!
//! Two jobs, and they are different:
//!
//! - **Prediction.** Take a measured room response and show what it would look
//!   like after a filter, before committing to anything. This is the common
//!   case and needs only [`Equaliser::response_at`], which is arithmetic on the
//!   coefficients and touches no audio.
//! - **Application.** Actually filter a signal. Needs [`Equaliser::process`]
//!   and carries per-band state.
//!
//! Prediction is deliberately separate from application, so a UI can draw a
//! curve for a filter that is not running.
//!
//! # Gain staging
//!
//! Bands add. Ten bands at +6 dB is +60 dB in the region where they overlap,
//! and that clips. [`Equaliser::peak_gain_db`] reports the worst case so a UI
//! can show it and a caller can trim by it; nothing here applies that trim
//! automatically, because silently changing a level the user set is worse than
//! showing them the number.

use crate::Complex32;
use crate::biquad::Biquad;

/// Standard ISO octave centres for a ten-band graphic equaliser.
///
/// These are the preferred frequencies from IEC 61260, rounded the way every
/// graphic EQ has printed them since the 1970s. Using the exact base-ten values
/// instead would be more correct and would surprise everybody.
pub const OCTAVE_CENTRES: [f32; 10] = [
    31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0,
];

/// Q for a peaking filter one octave wide.
///
/// Exactly the square root of two, which is not a coincidence: the cookbook's
/// `1/Q = 2*sinh(ln(2)/2 * BW)` at `BW = 1` gives
/// `2 * (sqrt(2) - 1/sqrt(2)) / 2 = 1/sqrt(2)`.
///
/// Ganged faders overshoot. With every band at +4 dB the combined curve reads
/// about +5.8 dB, because octave-spaced peaking filters overlap and decibels
/// add. That is inherent to a constant-Q graphic equaliser rather than a
/// defect; every hardware unit of this shape does it. Anyone using the faders
/// as a broad tilt should expect the curve to exceed the fader marks, and
/// should check [`Equaliser::peak_gain_db`].
///
/// A sweep from Q 1.0 to 4.1 puts the flattest ganged response near 1.5 rather
/// than here, but only by 0.18 dB. Interoperability wins that trade: a band
/// exported to another tool has to mean one octave there.
pub const OCTAVE_Q: f32 = std::f32::consts::SQRT_2;

/// What shape a band has.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum FilterKind {
    /// A bump or dip centred on the frequency. The workhorse.
    #[default]
    Peaking,
    /// Everything below the frequency lifted or cut.
    LowShelf,
    /// Everything above the frequency lifted or cut.
    HighShelf,
    /// Rolls off above the frequency. Gain is ignored.
    LowPass,
    /// Rolls off below the frequency. Gain is ignored.
    HighPass,
    /// Passes a band, rejects either side. Gain is ignored.
    BandPass,
    /// A null at the frequency. Gain is ignored.
    Notch,
    /// Flat magnitude, rotated phase. Gain is ignored.
    AllPass,
}

impl FilterKind {
    /// Whether `gain_db` means anything for this shape.
    ///
    /// A UI should grey the gain control out when this is false rather than
    /// letting someone set a number that does nothing.
    pub fn uses_gain(self) -> bool {
        matches!(
            self,
            FilterKind::Peaking | FilterKind::LowShelf | FilterKind::HighShelf
        )
    }

    /// Short label, matching what the rest of the industry prints.
    pub fn label(self) -> &'static str {
        match self {
            FilterKind::Peaking => "PK",
            FilterKind::LowShelf => "LS",
            FilterKind::HighShelf => "HS",
            FilterKind::LowPass => "LP",
            FilterKind::HighPass => "HP",
            FilterKind::BandPass => "BP",
            FilterKind::Notch => "NO",
            FilterKind::AllPass => "AP",
        }
    }
}

/// One band of an equaliser.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FilterBand {
    pub kind: FilterKind,
    /// Centre or corner frequency in hertz.
    pub hz: f32,
    /// Gain in decibels. Ignored unless [`FilterKind::uses_gain`].
    pub gain_db: f32,
    /// Quality factor. Higher is narrower.
    pub q: f32,
    /// Whether the band contributes. A disabled band keeps its settings.
    pub enabled: bool,
}

impl Default for FilterBand {
    fn default() -> Self {
        Self {
            kind: FilterKind::Peaking,
            hz: 1000.0,
            gain_db: 0.0,
            q: OCTAVE_Q,
            enabled: true,
        }
    }
}

impl FilterBand {
    /// A peaking band.
    pub fn peaking(hz: f32, gain_db: f32, q: f32) -> Self {
        Self {
            kind: FilterKind::Peaking,
            hz,
            gain_db,
            q,
            ..Self::default()
        }
    }

    /// Whether this band changes anything.
    ///
    /// A peaking band at 0 dB is exactly a pass-through, and skipping it saves
    /// a section - which matters when a ten-band EQ usually has two bands set.
    pub fn is_transparent(&self) -> bool {
        !self.enabled || (self.kind.uses_gain() && self.gain_db == 0.0)
    }

    /// Design the section this band describes.
    pub fn design(&self, sample_rate: f32) -> Biquad {
        if self.is_transparent() {
            return Biquad::IDENTITY;
        }
        match self.kind {
            FilterKind::Peaking => Biquad::peaking(self.hz, self.q, self.gain_db, sample_rate),
            FilterKind::LowShelf => Biquad::low_shelf(self.hz, self.q, self.gain_db, sample_rate),
            FilterKind::HighShelf => Biquad::high_shelf(self.hz, self.q, self.gain_db, sample_rate),
            FilterKind::LowPass => Biquad::low_pass(self.hz, self.q, sample_rate),
            FilterKind::HighPass => Biquad::high_pass(self.hz, self.q, sample_rate),
            FilterKind::BandPass => Biquad::band_pass(self.hz, self.q, sample_rate),
            FilterKind::Notch => Biquad::notch(self.hz, self.q, sample_rate),
            FilterKind::AllPass => Biquad::all_pass(self.hz, self.q, sample_rate),
        }
    }
}

/// A cascade of bands.
///
/// Coefficients are designed once, when a band changes, not per sample and not
/// per query. A ten-band curve evaluated across 2048 display points is 20,480
/// complex divisions; redesigning the sections inside that loop would make it
/// twenty times more.
#[derive(Debug, Clone)]
pub struct Equaliser {
    bands: Vec<FilterBand>,
    sections: Vec<Biquad>,
    sample_rate: f32,
    /// Applied after the cascade, for trimming the gain the bands added.
    preamp_db: f32,
}

impl Equaliser {
    /// An equaliser with the given bands.
    pub fn new(sample_rate: f32, bands: Vec<FilterBand>) -> Self {
        let mut eq = Self {
            sections: vec![Biquad::IDENTITY; bands.len()],
            bands,
            sample_rate,
            preamp_db: 0.0,
        };
        eq.redesign();
        eq
    }

    /// A ten-band graphic equaliser on ISO octave centres, all flat.
    pub fn graphic(sample_rate: f32) -> Self {
        Self::new(
            sample_rate,
            OCTAVE_CENTRES
                .iter()
                .map(|hz| FilterBand::peaking(*hz, 0.0, OCTAVE_Q))
                .collect(),
        )
    }

    /// An empty parametric equaliser.
    pub fn parametric(sample_rate: f32) -> Self {
        Self::new(sample_rate, Vec::new())
    }

    pub fn bands(&self) -> &[FilterBand] {
        &self.bands
    }

    pub fn sample_rate(&self) -> f32 {
        self.sample_rate
    }

    pub fn preamp_db(&self) -> f32 {
        self.preamp_db
    }

    /// Set the output trim, in decibels.
    pub fn set_preamp_db(&mut self, db: f32) {
        self.preamp_db = if db.is_finite() { db } else { 0.0 };
    }

    /// Replace a band. Out-of-range indices are ignored.
    pub fn set_band(&mut self, index: usize, band: FilterBand) {
        let Some(slot) = self.bands.get_mut(index) else {
            return;
        };
        if *slot == band {
            return;
        }
        *slot = band;
        // Only this section changes, and only its coefficients: the state is
        // left alone so a fader move does not click.
        if let Some(section) = self.sections.get_mut(index) {
            let designed = band.design(self.sample_rate);
            section.b0 = designed.b0;
            section.b1 = designed.b1;
            section.b2 = designed.b2;
            section.a1 = designed.a1;
            section.a2 = designed.a2;
        }
    }

    /// Set just a band's gain, which is all a graphic equaliser can do.
    pub fn set_gain_db(&mut self, index: usize, gain_db: f32) {
        let Some(band) = self.bands.get(index).copied() else {
            return;
        };
        self.set_band(
            index,
            FilterBand {
                gain_db: if gain_db.is_finite() { gain_db } else { 0.0 },
                ..band
            },
        );
    }

    /// Append a band and return its index.
    pub fn push_band(&mut self, band: FilterBand) -> usize {
        self.bands.push(band);
        self.sections.push(band.design(self.sample_rate));
        self.bands.len() - 1
    }

    /// Remove a band. Out-of-range indices are ignored.
    pub fn remove_band(&mut self, index: usize) {
        if index >= self.bands.len() {
            return;
        }
        self.bands.remove(index);
        self.sections.remove(index);
    }

    /// Change the sample rate, redesigning every section.
    ///
    /// A band above the new Nyquist becomes a pass-through rather than an
    /// error: a preset written at 96 kHz is still mostly useful at 44.1, and
    /// refusing the whole thing over its top band would help nobody.
    pub fn set_sample_rate(&mut self, sample_rate: f32) {
        if sample_rate <= 0.0 || sample_rate == self.sample_rate {
            return;
        }
        self.sample_rate = sample_rate;
        self.redesign();
    }

    /// Set every band at once.
    pub fn set_bands(&mut self, bands: Vec<FilterBand>) {
        self.bands = bands;
        self.sections = vec![Biquad::IDENTITY; self.bands.len()];
        self.redesign();
    }

    /// Complex response of the whole cascade at `hz`, including the preamp.
    ///
    /// Sections multiply, which is the entire reason a cascade is the right
    /// structure: magnitudes in decibels add and phases add, so the combined
    /// curve is the sum of the individual ones on a log plot.
    pub fn response_at(&self, hz: f32) -> Complex32 {
        let mut response = Complex32::new(linear(self.preamp_db), 0.0);
        for section in &self.sections {
            response *= section.response_at(hz, self.sample_rate);
        }
        response
    }

    /// Magnitude of the whole cascade at `hz`, in decibels.
    pub fn magnitude_db_at(&self, hz: f32) -> f32 {
        decibels(self.response_at(hz).norm())
    }

    /// Magnitude of one band at `hz`, in decibels.
    ///
    /// For drawing the individual band curves under the combined one, which is
    /// what makes a parametric EQ possible to reason about.
    pub fn band_magnitude_db_at(&self, index: usize, hz: f32) -> f32 {
        self.sections
            .get(index)
            .map(|section| decibels(section.magnitude_at(hz, self.sample_rate)))
            .unwrap_or(0.0)
    }

    /// Write the combined magnitude, in decibels, for each frequency.
    pub fn write_magnitude_db(&self, frequencies: &[f32], out: &mut [f32]) {
        for (hz, slot) in frequencies.iter().zip(out.iter_mut()) {
            *slot = self.magnitude_db_at(*hz);
        }
    }

    /// Write the combined phase, in degrees, for each frequency.
    pub fn write_phase_degrees(&self, frequencies: &[f32], out: &mut [f32]) {
        for (hz, slot) in frequencies.iter().zip(out.iter_mut()) {
            *slot = self.response_at(*hz).arg().to_degrees();
        }
    }

    /// The largest gain the cascade applies anywhere in the audio band.
    ///
    /// Bands add. Ten bands at +6 dB is +60 dB where they overlap, and that
    /// clips. A UI showing this next to a `Trim` button is the difference
    /// between an equaliser that is safe to use and one that is not.
    ///
    /// Sampled on a log grid rather than solved analytically: a peak between
    /// two grid points differs from the true maximum by well under the 0.1 dB
    /// anyone can act on, and the closed form for an arbitrary cascade is not
    /// worth having.
    pub fn peak_gain_db(&self) -> f32 {
        const POINTS: usize = 512;
        let (low, high) = (10.0_f32, (self.sample_rate * 0.5).min(24_000.0));
        if high <= low {
            return self.preamp_db;
        }
        let ratio = (high / low).ln();
        (0..POINTS)
            .map(|i| low * (ratio * i as f32 / (POINTS - 1) as f32).exp())
            .map(|hz| self.magnitude_db_at(hz))
            .fold(f32::NEG_INFINITY, f32::max)
    }

    /// Set the preamp so the cascade's loudest point sits at unity.
    ///
    /// Never automatic. A caller asks for this explicitly, because quietly
    /// moving a level the user set is worse than showing them the number and
    /// letting them decide.
    pub fn trim_to_unity(&mut self) {
        let peak = self.peak_gain_db();
        // A hundredth of a decibel, not zero. A cut-only equaliser evaluates to
        // a peak a millionth of a decibel above unity through ordinary rounding,
        // and trimming by that would leave a preamp value that looks like a bug.
        if peak.is_finite() && peak > 0.01 {
            self.preamp_db -= peak;
        }
    }

    /// Filter a block in place.
    pub fn process(&mut self, samples: &mut [f32]) {
        for section in &mut self.sections {
            section.process_block(samples);
        }
        let trim = linear(self.preamp_db);
        if trim != 1.0 {
            for sample in samples.iter_mut() {
                *sample *= trim;
            }
        }
    }

    /// Clear every section's delay line.
    pub fn reset(&mut self) {
        for section in &mut self.sections {
            section.reset();
        }
    }

    /// Set every gain to zero, leaving frequencies and Qs alone.
    pub fn flatten(&mut self) {
        for index in 0..self.bands.len() {
            self.set_gain_db(index, 0.0);
        }
        self.preamp_db = 0.0;
    }

    fn redesign(&mut self) {
        self.sections.resize(self.bands.len(), Biquad::IDENTITY);
        for (section, band) in self.sections.iter_mut().zip(&self.bands) {
            let designed = band.design(self.sample_rate);
            section.b0 = designed.b0;
            section.b1 = designed.b1;
            section.b2 = designed.b2;
            section.a1 = designed.a1;
            section.a2 = designed.a2;
        }
    }
}

fn linear(db: f32) -> f32 {
    10.0_f32.powf(db / 20.0)
}

fn decibels(linear: f32) -> f32 {
    20.0 * linear.max(1e-12).log10()
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::indexing_slicing)]
mod tests {
    use super::*;

    const RATE: f32 = 48_000.0;

    #[test]
    fn a_flat_graphic_equaliser_changes_nothing() {
        let eq = Equaliser::graphic(RATE);
        assert_eq!(eq.bands().len(), 10);
        for hz in [20.0_f32, 100.0, 1000.0, 10_000.0, 20_000.0] {
            assert!(
                eq.magnitude_db_at(hz).abs() < 1e-4,
                "flat EQ was not flat at {hz} Hz"
            );
        }
    }

    /// A fader moved is a bump at that frequency and nothing elsewhere.
    #[test]
    fn a_graphic_band_lifts_its_own_octave() {
        let mut eq = Equaliser::graphic(RATE);
        // The 1 kHz fader, index 5.
        eq.set_gain_db(5, 6.0);

        assert!((eq.magnitude_db_at(1000.0) - 6.0).abs() < 0.05);
        // Two octaves away the neighbours should be essentially untouched.
        assert!(eq.magnitude_db_at(250.0).abs() < 0.6);
        assert!(eq.magnitude_db_at(4000.0).abs() < 0.6);
    }

    /// Ganged faders overshoot, and the overshoot must stay bounded.
    ///
    /// Octave-spaced peaking filters overlap, so decibels add and every fader at
    /// +4 dB reads about +5.8 dB rather than +4. That is what a constant-Q
    /// graphic equaliser does; the test pins the magnitude of the effect so a
    /// change in OCTAVE_Q cannot quietly make it worse.
    #[test]
    fn ganged_faders_overshoot_by_a_bounded_amount() {
        let mut eq = Equaliser::graphic(RATE);
        for index in 0..10 {
            eq.set_gain_db(index, 4.0);
        }
        for hz in [125.0_f32, 500.0, 1000.0, 4000.0] {
            let combined = eq.magnitude_db_at(hz);
            assert!(
                combined > 4.0 && combined < 6.0,
                "all faders at 4 dB read {combined} dB at {hz} Hz"
            );
        }
    }

    /// OCTAVE_Q must be the cookbook's exact one-octave value, since that is
    /// what makes a band mean the same thing in another tool.
    #[test]
    fn the_octave_q_is_exactly_one_octave_wide() {
        let bandwidth = 2.0 * (1.0 / (2.0 * OCTAVE_Q)).asinh() / 2.0_f32.ln();
        assert!(
            (bandwidth - 1.0).abs() < 1e-4,
            "OCTAVE_Q describes {bandwidth} octaves, not 1"
        );
    }

    /// And it must be near the flattest choice, even if not exactly it.
    ///
    /// Q 1.5 is marginally flatter when every fader is ganged, but only by a
    /// fraction of a decibel. This pins that the standard value stays close to
    /// the best, so a future change to it cannot quietly cost a decibel.
    #[test]
    fn the_octave_q_is_close_to_the_flattest_choice() {
        let worst = |q: f32| {
            let eq = Equaliser::new(
                RATE,
                OCTAVE_CENTRES
                    .iter()
                    .map(|hz| FilterBand::peaking(*hz, 4.0, q))
                    .collect(),
            );
            // Over the whole curve, not only the band centres: a Q that is flat
            // at the centres can sag badly between them.
            let (low, high) = (45.0_f32, 11_000.0_f32);
            (0..400)
                .map(|i| low * (high / low).ln().mul_add(i as f32 / 399.0, 0.0).exp())
                .map(|hz| (eq.magnitude_db_at(hz) - 4.0).abs())
                .fold(0.0_f32, f32::max)
        };
        let chosen = worst(OCTAVE_Q);
        let best = [1.0_f32, 1.2, 1.5, 1.8, 2.5, 4.1]
            .into_iter()
            .map(worst)
            .fold(f32::INFINITY, f32::min);
        assert!(
            chosen <= best + 0.5,
            "the chosen Q deviates {chosen} dB against a best of {best}"
        );
    }

    /// Cascaded sections multiply, so decibels add.
    #[test]
    fn bands_add_in_decibels() {
        let mut eq = Equaliser::parametric(RATE);
        eq.push_band(FilterBand::peaking(1000.0, 6.0, 4.0));
        eq.push_band(FilterBand::peaking(1000.0, 6.0, 4.0));
        assert!(
            (eq.magnitude_db_at(1000.0) - 12.0).abs() < 0.05,
            "two 6 dB bands should make 12, got {}",
            eq.magnitude_db_at(1000.0)
        );
    }

    /// A disabled band keeps its settings and stops contributing.
    #[test]
    fn disabling_a_band_removes_it_from_the_curve_but_not_the_list() {
        let mut eq = Equaliser::parametric(RATE);
        let index = eq.push_band(FilterBand::peaking(1000.0, 9.0, 2.0));
        assert!((eq.magnitude_db_at(1000.0) - 9.0).abs() < 0.05);

        eq.set_band(
            index,
            FilterBand {
                enabled: false,
                ..eq.bands()[index]
            },
        );
        assert!(eq.magnitude_db_at(1000.0).abs() < 1e-4);
        assert_eq!(eq.bands().len(), 1, "the band is still there");
        assert_eq!(eq.bands()[index].gain_db, 9.0, "and keeps its gain");
    }

    #[test]
    fn removing_a_band_removes_its_section() {
        let mut eq = Equaliser::parametric(RATE);
        eq.push_band(FilterBand::peaking(1000.0, 6.0, 2.0));
        eq.push_band(FilterBand::peaking(100.0, 6.0, 2.0));
        eq.remove_band(0);
        assert_eq!(eq.bands().len(), 1);
        assert!(eq.magnitude_db_at(1000.0).abs() < 0.5);
        assert!((eq.magnitude_db_at(100.0) - 6.0).abs() < 0.05);
        // Out of range must be ignored, not panic.
        eq.remove_band(99);
        assert_eq!(eq.bands().len(), 1);
    }

    /// The headroom figure has to be the real worst case, since it is what a
    /// caller trims by.
    #[test]
    fn peak_gain_finds_the_worst_case() {
        let mut eq = Equaliser::parametric(RATE);
        eq.push_band(FilterBand::peaking(1000.0, 6.0, 1.0));
        eq.push_band(FilterBand::peaking(1200.0, 6.0, 1.0));
        let peak = eq.peak_gain_db();
        assert!(
            peak > 10.0,
            "two overlapping 6 dB bands must exceed 10 dB somewhere, got {peak}"
        );
        assert!(peak < 12.5, "and cannot exceed their sum, got {peak}");
    }

    #[test]
    fn trimming_brings_the_peak_to_unity() {
        let mut eq = Equaliser::parametric(RATE);
        eq.push_band(FilterBand::peaking(1000.0, 12.0, 1.0));
        eq.trim_to_unity();
        let peak = eq.peak_gain_db();
        assert!(peak.abs() < 0.1, "after trimming the peak was {peak} dB");
        assert!(eq.preamp_db() < -11.0);
    }

    /// Cutting rather than boosting must not be trimmed at all.
    #[test]
    fn trimming_leaves_a_cut_only_equaliser_alone() {
        let mut eq = Equaliser::parametric(RATE);
        eq.push_band(FilterBand::peaking(1000.0, -12.0, 1.0));
        eq.trim_to_unity();
        assert_eq!(eq.preamp_db(), 0.0);
    }

    /// The running filter must match the predicted curve. This is the check
    /// that the drawn curve is the truth, which is the whole point of the
    /// prediction path being separate from the application path.
    #[test]
    fn the_running_equaliser_matches_its_predicted_curve() {
        let mut eq = Equaliser::parametric(RATE);
        eq.push_band(FilterBand::peaking(1000.0, 8.0, 2.0));
        eq.push_band(FilterBand {
            kind: FilterKind::HighShelf,
            hz: 5000.0,
            gain_db: -6.0,
            q: 0.707,
            enabled: true,
        });
        eq.set_preamp_db(-3.0);

        for hz in [100.0_f32, 1000.0, 3000.0, 10_000.0] {
            let predicted = eq.magnitude_db_at(hz);

            eq.reset();
            let frames = 48_000;
            let mut input = vec![0.0_f32; frames];
            for (frame, sample) in input.iter_mut().enumerate() {
                *sample = (std::f32::consts::TAU * hz * frame as f32 / RATE).sin();
            }
            eq.process(&mut input);

            // RMS over the settled tail; a peak reading would be biased by
            // wherever the samples happen to fall on the waveform.
            let tail = &input[frames / 2..];
            let sum: f64 = tail.iter().map(|s| f64::from(*s) * f64::from(*s)).sum();
            let rms = (sum / tail.len() as f64).sqrt() as f32;
            let measured = decibels(rms / std::f32::consts::FRAC_1_SQRT_2);

            assert!(
                (measured - predicted).abs() < 0.2,
                "at {hz} Hz predicted {predicted} dB but measured {measured} dB"
            );
        }
    }

    /// A preset written at one rate must survive being loaded at another.
    #[test]
    fn changing_the_sample_rate_redesigns_the_bands() {
        let mut eq = Equaliser::parametric(RATE);
        eq.push_band(FilterBand::peaking(1000.0, 6.0, 2.0));
        assert!((eq.magnitude_db_at(1000.0) - 6.0).abs() < 0.05);

        eq.set_sample_rate(44_100.0);
        assert!(
            (eq.magnitude_db_at(1000.0) - 6.0).abs() < 0.05,
            "the band moved when the rate changed"
        );
    }

    /// A band above the new Nyquist must go quiet, not produce NaNs.
    #[test]
    fn a_band_above_nyquist_becomes_a_pass_through() {
        let mut eq = Equaliser::parametric(96_000.0);
        eq.push_band(FilterBand::peaking(30_000.0, 9.0, 2.0));
        eq.set_sample_rate(44_100.0);
        for hz in [100.0_f32, 1000.0, 20_000.0] {
            let db = eq.magnitude_db_at(hz);
            assert!(db.is_finite(), "{hz} Hz produced {db}");
            assert!(db.abs() < 0.01);
        }
    }

    /// Flattening clears the gains and the trim but keeps the layout.
    #[test]
    fn flattening_clears_gains_but_keeps_bands() {
        let mut eq = Equaliser::graphic(RATE);
        eq.set_gain_db(3, 8.0);
        eq.set_preamp_db(-8.0);
        eq.flatten();
        assert_eq!(eq.bands().len(), 10);
        assert_eq!(eq.preamp_db(), 0.0);
        assert!(eq.magnitude_db_at(250.0).abs() < 1e-4);
        assert_eq!(eq.bands()[3].hz, 250.0, "the band is still where it was");
    }

    /// Gain is meaningless for the pass and reject shapes, and a UI needs to
    /// know that rather than offering a control that does nothing.
    #[test]
    fn only_the_gain_shapes_use_gain() {
        assert!(FilterKind::Peaking.uses_gain());
        assert!(FilterKind::LowShelf.uses_gain());
        assert!(FilterKind::HighShelf.uses_gain());
        for kind in [
            FilterKind::LowPass,
            FilterKind::HighPass,
            FilterKind::BandPass,
            FilterKind::Notch,
            FilterKind::AllPass,
        ] {
            assert!(!kind.uses_gain(), "{kind:?} should not use gain");
        }
    }

    /// A highpass must filter even though its gain is zero, which the
    /// transparency shortcut must not skip.
    #[test]
    fn a_zero_gain_highpass_still_filters() {
        let mut eq = Equaliser::parametric(RATE);
        eq.push_band(FilterBand {
            kind: FilterKind::HighPass,
            hz: 1000.0,
            gain_db: 0.0,
            q: 0.707,
            enabled: true,
        });
        assert!(
            eq.magnitude_db_at(100.0) < -30.0,
            "a highpass with no gain still has to roll off"
        );
    }

    /// Moving a fader must not restart the filter's state, or every move would
    /// click.
    #[test]
    fn changing_a_gain_preserves_filter_state() {
        let mut eq = Equaliser::graphic(RATE);
        eq.set_gain_db(5, 6.0);
        let mut ramp: Vec<f32> = (0..512).map(|i| (i as f32 / 512.0) * 0.5).collect();
        eq.process(&mut ramp);

        // Change a different band; the running state must survive.
        eq.set_gain_db(2, 3.0);
        let mut next = vec![0.0_f32; 8];
        eq.process(&mut next);
        assert!(
            next.iter().any(|s| s.abs() > 1e-6),
            "the tail of the previous block was thrown away"
        );
    }

    /// Out-of-range indices are a UI race, not a bug worth crashing over.
    #[test]
    fn out_of_range_indices_are_ignored() {
        let mut eq = Equaliser::graphic(RATE);
        eq.set_gain_db(99, 12.0);
        eq.set_band(99, FilterBand::peaking(100.0, 12.0, 1.0));
        assert_eq!(eq.band_magnitude_db_at(99, 1000.0), 0.0);
        for hz in [100.0_f32, 1000.0] {
            assert!(eq.magnitude_db_at(hz).abs() < 1e-4);
        }
    }
}
