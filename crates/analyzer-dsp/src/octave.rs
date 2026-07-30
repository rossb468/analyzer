//! Fractional-octave band analysis, following IEC 61260.
//!
//! An FFT gives linearly spaced bins; hearing works in octaves. Octave bands
//! bridge the two by summing bin power into logarithmically spaced buckets,
//! which is how noise measurements have been reported for decades and what makes
//! two measurements of the same room comparable.
//!
//! # Base-ten, not base-two
//!
//! IEC 61260 defines the octave ratio as `G = 10^(3/10) ≈ 1.9953`, not exactly 2.
//! The difference looks pedantic and is not: over the ten octaves of the audio
//! band the two conventions drift far enough apart that band centres stop
//! matching published tables, and a measurement that cannot be compared to
//! anyone else's is worth much less.
//!
//! # Bands narrower than the analysis
//!
//! At 48 kHz with a 4096-point FFT the bins are 11.7 Hz apart, while a
//! third-octave band centred at 25 Hz is only 5.8 Hz wide. No FFT bin falls
//! inside it, and no amount of arithmetic can recover what was never resolved.
//! [`OctaveBands::is_resolvable`] reports that honestly so a display can grey the
//! band out, rather than printing a confident number derived from a neighbouring
//! bin.

/// The IEC 61260 octave ratio, `10^(3/10)`.
pub const OCTAVE_RATIO: f32 = 1.995_262_3;

/// Floor for a band with no energy.
pub const BAND_FLOOR_DB: f32 = -200.0;

/// One frequency band.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Band {
    /// Exact centre frequency.
    pub centre_hz: f32,
    /// Lower band edge.
    pub lower_hz: f32,
    /// Upper band edge.
    pub upper_hz: f32,
}

impl Band {
    /// Width in hertz.
    pub fn width_hz(&self) -> f32 {
        self.upper_hz - self.lower_hz
    }

    /// The nominal frequency this band is known by.
    ///
    /// Exact centres are awkward numbers — a third-octave band sits at 1258.9 Hz
    /// and everyone calls it 1250. This rounds to the preferred series so labels
    /// match what is printed on every other analyser.
    pub fn nominal_hz(&self) -> f32 {
        const PREFERRED: [f32; 30] = [
            1.0, 1.25, 1.6, 2.0, 2.5, 3.15, 4.0, 5.0, 6.3, 8.0, 10.0, 12.5, 16.0, 20.0, 25.0, 31.5,
            40.0, 50.0, 63.0, 80.0, 100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0, 500.0, 630.0,
            800.0,
        ];
        if self.centre_hz <= 0.0 {
            return self.centre_hz;
        }
        // Reduce to the 1..10 decade, snap, then scale back.
        let decade = self.centre_hz.log10().floor();
        let scale = 10.0_f32.powf(decade);
        let mantissa = self.centre_hz / scale;

        let best = PREFERRED
            .iter()
            .take(10)
            .copied()
            .min_by(|a, b| (a - mantissa).abs().total_cmp(&(b - mantissa).abs()))
            .unwrap_or(mantissa);
        best * scale
    }
}

/// A set of fractional-octave bands.
#[derive(Debug, Clone, PartialEq)]
pub struct OctaveBands {
    fraction: u32,
    bands: Vec<Band>,
}

impl OctaveBands {
    /// Build bands of `1/fraction` octave covering `min_hz..=max_hz`.
    ///
    /// # Panics
    ///
    /// Panics if `fraction` is zero, or the frequency range is not positive and
    /// ascending.
    pub fn new(fraction: u32, min_hz: f32, max_hz: f32) -> Self {
        assert!(fraction > 0, "fraction must be at least 1");
        assert!(
            min_hz > 0.0 && max_hz > min_hz,
            "need 0 < min_hz < max_hz, got {min_hz}..{max_hz}"
        );

        let n = fraction as f32;
        let half_width = OCTAVE_RATIO.powf(1.0 / (2.0 * n));
        let mut bands = Vec::new();

        // IEC 61260 indexes bands from 1 kHz. Odd fractions centre a band on the
        // reference; even fractions straddle it, which is why the exponent
        // differs between the two.
        let even = fraction.is_multiple_of(2);
        for index in -600_i32..=600 {
            let exponent = if even {
                (2.0 * index as f32 + 1.0) / (2.0 * n)
            } else {
                index as f32 / n
            };
            let centre = 1000.0 * OCTAVE_RATIO.powf(exponent);
            // One percent of slack, because nominal and exact centres differ.
            // The band everyone calls "20 Hz" actually sits at 19.95, and a
            // strict bound would drop it from a 20 Hz..20 kHz request - while
            // the slack stays far too small to admit the next band down.
            if centre < min_hz * 0.99 || centre > max_hz * 1.01 {
                continue;
            }
            bands.push(Band {
                centre_hz: centre,
                lower_hz: centre / half_width,
                upper_hz: centre * half_width,
            });
        }

        bands.sort_by(|a, b| a.centre_hz.total_cmp(&b.centre_hz));
        Self { fraction, bands }
    }

    /// Third-octave bands across the audio band, the usual default.
    pub fn third_octave() -> Self {
        Self::new(3, 20.0, 20_000.0)
    }

    /// The bands, ascending.
    pub fn bands(&self) -> &[Band] {
        &self.bands
    }

    /// How many bands there are.
    pub fn len(&self) -> usize {
        self.bands.len()
    }

    /// Whether there are no bands.
    pub fn is_empty(&self) -> bool {
        self.bands.is_empty()
    }

    /// The fraction these bands divide an octave into.
    pub fn fraction(&self) -> u32 {
        self.fraction
    }

    /// Whether band `index` is wide enough for the analysis to resolve.
    ///
    /// A band narrower than the bin spacing contains no bin, and a level reported
    /// for it would be borrowed from a neighbour rather than measured.
    pub fn is_resolvable(&self, index: usize, bin_spacing_hz: f32) -> bool {
        self.bands
            .get(index)
            .is_some_and(|band| band.width_hz() >= bin_spacing_hz)
    }

    /// The lowest band the analysis can actually resolve.
    ///
    /// A UI wanting a single cut-off rather than a per-band check can start here.
    pub fn first_resolvable(&self, bin_spacing_hz: f32) -> Option<usize> {
        (0..self.bands.len()).find(|index| self.is_resolvable(*index, bin_spacing_hz))
    }

    /// Sum a spectrum into bands.
    ///
    /// `bins_db` holds one level per FFT bin, bin `k` at `k * bin_spacing_hz`.
    /// Power is summed within each band and converted back to decibels, which is
    /// the correct operation — adding decibels directly would be a geometric mean
    /// and read low wherever a band is peaky.
    ///
    /// Bands too narrow to hold a bin take the nearest bin's level.
    /// [`OctaveBands::is_resolvable`] says which those are.
    ///
    /// # Panics
    ///
    /// Panics if `out` is not one element per band.
    pub fn apply(&self, bins_db: &[f32], bin_spacing_hz: f32, out: &mut [f32]) {
        assert_eq!(out.len(), self.bands.len(), "output must be one per band");
        if bins_db.is_empty() || bin_spacing_hz <= 0.0 {
            out.fill(BAND_FLOOR_DB);
            return;
        }

        for (slot, band) in out.iter_mut().zip(&self.bands) {
            // Skip bin 0: DC belongs to no band.
            let first = (band.lower_hz / bin_spacing_hz).ceil().max(1.0) as usize;
            let last = ((band.upper_hz / bin_spacing_hz).floor() as usize)
                .min(bins_db.len().saturating_sub(1));

            if first <= last {
                let power: f32 = bins_db
                    .get(first..=last)
                    .unwrap_or(&[])
                    .iter()
                    .map(|db| 10.0_f32.powf(db / 10.0))
                    .sum();
                *slot = if power > 0.0 {
                    10.0 * power.log10()
                } else {
                    BAND_FLOOR_DB
                };
            } else {
                // Narrower than the resolution: report the nearest bin rather
                // than nothing, and let is_resolvable flag it as approximate.
                let nearest = ((band.centre_hz / bin_spacing_hz).round() as usize)
                    .clamp(1, bins_db.len().saturating_sub(1));
                *slot = bins_db.get(nearest).copied().unwrap_or(BAND_FLOOR_DB);
            }
        }
    }
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::{
        Averaging, Generator, Overlap, Signal, SpectrumAnalyzer, SpectrumConfig, WindowKind,
    };

    const RATE: f32 = 48_000.0;
    const SIZE: usize = 8192;

    /// Published octave centres. These are the numbers on every analyser and
    /// every noise report, so they are the real specification.
    #[test]
    fn full_octave_centres_match_the_published_series() {
        let bands = OctaveBands::new(1, 20.0, 20_000.0);
        let nominal: Vec<f32> = bands.bands().iter().map(Band::nominal_hz).collect();
        assert_eq!(
            nominal,
            vec![
                31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16_000.0
            ]
        );
    }

    #[test]
    fn third_octave_centres_match_the_published_series() {
        let bands = OctaveBands::third_octave();
        let nominal: Vec<f32> = bands.bands().iter().map(Band::nominal_hz).collect();
        assert_eq!(
            nominal,
            vec![
                20.0, 25.0, 31.5, 40.0, 50.0, 63.0, 80.0, 100.0, 125.0, 160.0, 200.0, 250.0, 315.0,
                400.0, 500.0, 630.0, 800.0, 1000.0, 1250.0, 1600.0, 2000.0, 2500.0, 3150.0, 4000.0,
                5000.0, 6300.0, 8000.0, 10_000.0, 12_500.0, 16_000.0, 20_000.0
            ]
        );
    }

    /// 1 kHz is the reference, so an odd fraction must land a band exactly on it.
    #[test]
    fn odd_fractions_centre_a_band_on_one_kilohertz() {
        for fraction in [1_u32, 3, 5] {
            let bands = OctaveBands::new(fraction, 900.0, 1100.0);
            assert!(
                bands
                    .bands()
                    .iter()
                    .any(|b| (b.centre_hz - 1000.0).abs() < 0.1),
                "1/{fraction} octave should have a band at 1 kHz"
            );
        }
    }

    /// Even fractions straddle the reference rather than centring on it.
    #[test]
    fn even_fractions_straddle_one_kilohertz() {
        let bands = OctaveBands::new(6, 900.0, 1100.0);
        assert!(
            !bands
                .bands()
                .iter()
                .any(|b| (b.centre_hz - 1000.0).abs() < 1.0),
            "1/6 octave should straddle 1 kHz, not centre on it"
        );
        // But a band edge should fall very close to it.
        assert!(
            bands
                .bands()
                .iter()
                .any(|b| (b.lower_hz - 1000.0).abs() < 5.0 || (b.upper_hz - 1000.0).abs() < 5.0)
        );
    }

    #[test]
    fn adjacent_bands_meet_without_gaps_or_overlap() {
        for fraction in [1_u32, 3, 6, 12, 24] {
            let bands = OctaveBands::new(fraction, 50.0, 10_000.0);
            for pair in bands.bands().windows(2) {
                let gap = (pair[1].lower_hz - pair[0].upper_hz).abs();
                assert!(
                    gap < pair[0].upper_hz * 1e-4,
                    "1/{fraction}: gap of {gap} Hz between {} and {}",
                    pair[0].centre_hz,
                    pair[1].centre_hz
                );
            }
        }
    }

    #[test]
    fn band_width_follows_the_fraction() {
        for fraction in [1_u32, 3, 6, 12] {
            let bands = OctaveBands::new(fraction, 900.0, 1200.0);
            let band = bands.bands().first().unwrap();
            let ratio = band.upper_hz / band.lower_hz;
            let expected = OCTAVE_RATIO.powf(1.0 / fraction as f32);
            assert!(
                (ratio - expected).abs() < 1e-4,
                "1/{fraction}: ratio {ratio}, expected {expected}"
            );
        }
    }

    /// The definitive test. Pink noise is equal energy per octave, so summed into
    /// octave bands it must come out flat - which simultaneously validates the
    /// band edges and the generator's pink filter.
    #[test]
    fn pink_noise_is_flat_across_octave_bands() {
        let mut generator = Generator::new(RATE, Signal::PinkNoise { amplitude: 0.5 }, 1);
        let mut samples = vec![0.0; SIZE * 32];
        generator.fill(&mut samples);

        let mut analyzer = SpectrumAnalyzer::new(SpectrumConfig {
            sample_rate: RATE,
            size: SIZE,
            window: WindowKind::Hann,
            overlap: Overlap::Half,
            averaging: Averaging::Infinite,
        });
        analyzer.push(&samples);
        let mut bins = vec![0.0; analyzer.bins()];
        analyzer.write_db_fs(&mut bins);

        let bands = OctaveBands::new(1, 63.0, 8000.0);
        let mut levels = vec![0.0; bands.len()];
        bands.apply(&bins, analyzer.bin_spacing_hz(), &mut levels);

        let mean: f32 = levels.iter().sum::<f32>() / levels.len() as f32;
        for (index, level) in levels.iter().enumerate() {
            assert!(
                (level - mean).abs() < 1.5,
                "band {index} at {:.0} Hz read {level:.1}, mean {mean:.1}",
                bands.bands()[index].nominal_hz()
            );
        }
    }

    /// White noise is equal energy per hertz, so each octave up holds twice the
    /// bandwidth and reads 3 dB hotter.
    #[test]
    fn white_noise_rises_three_decibels_per_octave_band() {
        let mut generator = Generator::new(RATE, Signal::WhiteNoise { amplitude: 0.5 }, 2);
        let mut samples = vec![0.0; SIZE * 32];
        generator.fill(&mut samples);

        let mut analyzer = SpectrumAnalyzer::new(SpectrumConfig {
            sample_rate: RATE,
            size: SIZE,
            window: WindowKind::Hann,
            overlap: Overlap::Half,
            averaging: Averaging::Infinite,
        });
        analyzer.push(&samples);
        let mut bins = vec![0.0; analyzer.bins()];
        analyzer.write_db_fs(&mut bins);

        let bands = OctaveBands::new(1, 125.0, 8000.0);
        let mut levels = vec![0.0; bands.len()];
        bands.apply(&bins, analyzer.bin_spacing_hz(), &mut levels);

        for pair in levels.windows(2) {
            let step = pair[1] - pair[0];
            assert!(
                (step - 3.0).abs() < 0.7,
                "expected +3 dB per octave, got {step:.2}"
            );
        }
    }

    /// Power sums, decibels do not. Two equal bins in one band must read 3 dB
    /// above one of them.
    #[test]
    fn band_power_sums_rather_than_averaging_decibels() {
        let spacing = 10.0_f32;
        // Bins at 100 and 110 Hz, both at -20 dB, inside one wide band.
        let mut bins = vec![BAND_FLOOR_DB; 50];
        bins[10] = -20.0;
        bins[11] = -20.0;

        let bands = OctaveBands::new(1, 90.0, 130.0);
        let mut levels = vec![0.0; bands.len()];
        bands.apply(&bins, spacing, &mut levels);

        assert!(
            (levels[0] - -16.9897).abs() < 0.01,
            "two equal bins should sum to +3 dB, got {}",
            levels[0]
        );
    }

    /// Honesty about the resolution limit. A third-octave band at 25 Hz is 5.8 Hz
    /// wide, narrower than an 11.7 Hz bin, and must be reported as unresolvable
    /// rather than given a confident number.
    #[test]
    fn narrow_bands_are_reported_as_unresolvable() {
        let bands = OctaveBands::third_octave();
        let spacing = 48_000.0 / 4096.0;

        assert!(
            !bands.is_resolvable(0, spacing),
            "20 Hz third-octave is 4.6 Hz wide"
        );
        assert!(
            !bands.is_resolvable(1, spacing),
            "25 Hz third-octave is 5.8 Hz wide"
        );

        let first = bands.first_resolvable(spacing).unwrap();
        assert!(
            first > 0,
            "some low bands must be unresolvable at this spacing"
        );
        for index in first..bands.len() {
            assert!(
                bands.is_resolvable(index, spacing),
                "band {index} should be resolvable once the first one is"
            );
        }
        // A much longer FFT resolves everything.
        assert!(bands.is_resolvable(0, 48_000.0 / 65_536.0));
    }

    #[test]
    fn unresolvable_bands_still_produce_a_finite_number() {
        let bands = OctaveBands::third_octave();
        let spacing = 48_000.0 / 4096.0;
        let bins = vec![-40.0_f32; 2049];
        let mut levels = vec![0.0; bands.len()];
        bands.apply(&bins, spacing, &mut levels);
        assert!(levels.iter().all(|l| l.is_finite()));
        assert!(
            (levels[0] - -40.0).abs() < 0.1,
            "nearest bin should be used"
        );
    }

    #[test]
    fn dc_is_excluded_from_the_lowest_band() {
        let mut bins = vec![-90.0_f32; 100];
        bins[0] = 40.0;
        let bands = OctaveBands::new(1, 20.0, 200.0);
        let mut levels = vec![0.0; bands.len()];
        bands.apply(&bins, 10.0, &mut levels);
        assert!(
            levels.iter().all(|l| *l < -60.0),
            "DC leaked into a band: {levels:?}"
        );
    }

    #[test]
    fn degenerate_input_floors_rather_than_panicking() {
        let bands = OctaveBands::third_octave();
        let mut levels = vec![0.0; bands.len()];

        bands.apply(&[], 11.7, &mut levels);
        assert!(levels.iter().all(|l| *l <= BAND_FLOOR_DB + 1e-3));

        bands.apply(&vec![-40.0; 100], 0.0, &mut levels);
        assert!(levels.iter().all(|l| *l <= BAND_FLOOR_DB + 1e-3));
    }

    #[test]
    fn finer_fractions_produce_more_bands() {
        let mut previous = 0;
        for fraction in [1_u32, 3, 6, 12, 24, 48] {
            let count = OctaveBands::new(fraction, 20.0, 20_000.0).len();
            assert!(count > previous, "1/{fraction} gave {count} bands");
            previous = count;
        }
    }

    #[test]
    #[should_panic(expected = "fraction must be at least 1")]
    fn a_zero_fraction_is_rejected() {
        let _ = OctaveBands::new(0, 20.0, 20_000.0);
    }
}
