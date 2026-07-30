//! Harmonic distortion from a spectrum containing a steady tone.
//!
//! Feed a sine through a system, look at what comes back, and everything that is
//! not the sine is something the system added. The harmonics say what kind of
//! nonlinearity it is — a symmetric one like clipping produces odd orders, an
//! asymmetric one produces even — and the total says how much.
//!
//! # Summing lobes, not bins
//!
//! A tone almost never lands exactly on a bin centre, and a windowed tone that
//! does not spreads across its window's main lobe. Reading a single bin
//! therefore under-reads by up to the window's scalloping loss, which for Hann is
//! 1.4 dB and swings with frequency. Every peak here is summed across its lobe
//! instead, which is why a measured harmonic level does not drift as the
//! fundamental moves between bins.
//!
//! # Harmonics above Nyquist
//!
//! At 48 kHz a 5 kHz fundamental has its fifth harmonic at 25 kHz, above Nyquist.
//! It does not vanish: it aliases back to 23 kHz, where reading it would report a
//! completely fictional distortion product. Orders beyond Nyquist are excluded
//! and [`Distortion::orders_above_nyquist`] says how many, so a display can show
//! "THD (to H4)" rather than implying it measured all ten.

/// Floor for decibel output.
pub const DISTORTION_FLOOR_DB: f32 = -200.0;

/// One harmonic of the fundamental.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Harmonic {
    /// Harmonic number: 2 is the octave above the fundamental.
    pub order: u32,
    /// Where it was actually found, which may differ slightly from `order × f₀`.
    pub hz: f32,
    /// Absolute level in the same reference as the input spectrum.
    pub level_db: f32,
    /// Level relative to the fundamental. Always negative for a sane system.
    pub relative_db: f32,
    /// The same ratio as a percentage of the fundamental's amplitude.
    pub percent: f32,
}

/// A distortion measurement.
#[derive(Debug, Clone, PartialEq)]
pub struct Distortion {
    /// Fundamental frequency, refined from the spectrum.
    pub fundamental_hz: f32,
    /// Fundamental level.
    pub fundamental_db: f32,
    /// Harmonics found, ascending by order.
    pub harmonics: Vec<Harmonic>,
    /// Total harmonic distortion as a percentage of the fundamental.
    pub thd_percent: f32,
    /// The same figure in decibels relative to the fundamental.
    pub thd_db: f32,
    /// Total harmonic distortion plus noise: everything that is not the
    /// fundamental, including hum, hiss and any non-harmonic product.
    pub thd_n_percent: f32,
    /// The same figure in decibels.
    pub thd_n_db: f32,
    /// Median level of the bins that are neither fundamental nor harmonic.
    ///
    /// A median rather than a mean, because a mean is dragged upwards by any
    /// spur the analysis did not classify.
    pub noise_floor_db: f32,
    /// Harmonic orders that would fall above Nyquist and were therefore not
    /// measured. Non-zero means the reported THD covers fewer orders than asked
    /// for.
    pub orders_above_nyquist: u32,
}

impl Distortion {
    /// Highest harmonic order actually measured.
    pub fn highest_order(&self) -> u32 {
        self.harmonics.last().map(|h| h.order).unwrap_or(1)
    }

    /// A harmonic by order.
    pub fn harmonic(&self, order: u32) -> Option<&Harmonic> {
        self.harmonics.iter().find(|h| h.order == order)
    }

    /// A label honest about how many orders the figure covers.
    pub fn thd_label(&self) -> String {
        if self.orders_above_nyquist > 0 {
            format!(
                "THD {:.4}% (to H{})",
                self.thd_percent,
                self.highest_order()
            )
        } else {
            format!("THD {:.4}%", self.thd_percent)
        }
    }
}

/// How to look for harmonics.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct DistortionConfig {
    /// Highest harmonic order to look for.
    pub max_order: u32,
    /// How far either side of `order × f₀` to search for the peak, in bins.
    ///
    /// Needed because a real system's harmonics sit exactly at multiples of the
    /// fundamental, but the *measured* fundamental has bin-quantisation error
    /// that multiplies with order — a half-bin error on f₀ is a five-bin error
    /// on H10.
    pub search_bins: usize,
    /// How many bins either side of a peak to sum, covering the window's main
    /// lobe.
    pub lobe_bins: usize,
}

impl Default for DistortionConfig {
    fn default() -> Self {
        Self {
            max_order: 10,
            search_bins: 4,
            lobe_bins: 3,
        }
    }
}

/// Measure distortion in a spectrum.
///
/// `power_bins` is **linear mean-square power per bin**, as produced by
/// [`crate::SpectrumAnalyzer::power`], not decibels. Summing decibels would be a
/// geometric mean and wrong by several dB on anything peaky.
///
/// `fundamental_hz` may be given when it is known; otherwise the loudest bin is
/// used. Supplying it matters when the distortion is severe enough that a
/// harmonic rivals the fundamental.
///
/// Returns `None` if the spectrum is empty, the bin spacing is not positive, or
/// no fundamental can be found.
pub fn analyse(
    power_bins: &[f32],
    bin_spacing_hz: f32,
    fundamental_hz: Option<f32>,
    config: &DistortionConfig,
) -> Option<Distortion> {
    if power_bins.len() < 4 || bin_spacing_hz <= 0.0 {
        return None;
    }

    // Locate the fundamental. Bin 0 is DC and is never a tone.
    let fundamental_bin = match fundamental_hz {
        Some(hz) => {
            let nominal = (hz / bin_spacing_hz).round() as usize;
            peak_near(power_bins, nominal, config.search_bins)?
        }
        None => power_bins
            .iter()
            .enumerate()
            .skip(1)
            .max_by(|a, b| a.1.total_cmp(b.1))
            .map(|(bin, _)| bin)?,
    };
    if fundamental_bin == 0 {
        return None;
    }

    let fundamental_power = lobe_power(power_bins, fundamental_bin, config.lobe_bins);
    if fundamental_power <= 0.0 {
        return None;
    }

    // Refine the frequency by the power centroid of the lobe, which recovers
    // most of the sub-bin position a bare peak index throws away.
    let fundamental_hz = centroid_hz(
        power_bins,
        fundamental_bin,
        config.lobe_bins,
        bin_spacing_hz,
    );

    let nyquist = power_bins.len() as f32 * bin_spacing_hz;
    let mut harmonics = Vec::new();
    let mut harmonic_power = 0.0_f32;
    let mut claimed = vec![false; power_bins.len()];
    mark(&mut claimed, fundamental_bin, config.lobe_bins);
    let mut above_nyquist = 0;

    for order in 2..=config.max_order {
        let expected = fundamental_hz * order as f32;
        if expected >= nyquist {
            // Aliases back into the band if measured, so it is not measured.
            above_nyquist += 1;
            continue;
        }
        let nominal = (expected / bin_spacing_hz).round() as usize;
        let Some(bin) = peak_near(power_bins, nominal, config.search_bins) else {
            continue;
        };
        let power = lobe_power(power_bins, bin, config.lobe_bins);
        mark(&mut claimed, bin, config.lobe_bins);
        harmonic_power += power;

        let ratio = (power / fundamental_power).sqrt();
        harmonics.push(Harmonic {
            order,
            hz: centroid_hz(power_bins, bin, config.lobe_bins, bin_spacing_hz),
            level_db: to_db(power),
            relative_db: amplitude_db(ratio),
            percent: ratio * 100.0,
        });
    }

    // Everything except DC and the fundamental's own lobe. Harmonics stay in,
    // because THD+N is by definition distortion *and* noise - excluding them
    // would make it smaller than THD, which is impossible.
    let residual_power: f32 = power_bins
        .iter()
        .enumerate()
        .skip(1)
        .filter(|(bin, _)| bin.abs_diff(fundamental_bin) > config.lobe_bins)
        .map(|(_, power)| *power)
        .sum();

    // Noise floor as a median over the unclaimed bins, so a stray spur does not
    // drag it up the way a mean would.
    let mut unclaimed: Vec<f32> = power_bins
        .iter()
        .enumerate()
        .skip(1)
        .filter(|(bin, _)| !claimed.get(*bin).copied().unwrap_or(false))
        .map(|(_, power)| *power)
        .collect();
    let noise_floor = if unclaimed.is_empty() {
        0.0
    } else {
        unclaimed.sort_by(f32::total_cmp);
        unclaimed.get(unclaimed.len() / 2).copied().unwrap_or(0.0)
    };

    let thd_ratio = (harmonic_power / fundamental_power).sqrt();
    let thd_n_ratio = (residual_power / fundamental_power).sqrt();

    Some(Distortion {
        fundamental_hz,
        fundamental_db: to_db(fundamental_power),
        harmonics,
        thd_percent: thd_ratio * 100.0,
        thd_db: amplitude_db(thd_ratio),
        thd_n_percent: thd_n_ratio * 100.0,
        thd_n_db: amplitude_db(thd_n_ratio),
        noise_floor_db: to_db(noise_floor),
        orders_above_nyquist: above_nyquist,
    })
}

/// Loudest bin within `window` of `centre`, skipping DC.
fn peak_near(power_bins: &[f32], centre: usize, window: usize) -> Option<usize> {
    let low = centre.saturating_sub(window).max(1);
    let high = (centre + window).min(power_bins.len().saturating_sub(1));
    if low > high {
        return None;
    }
    (low..=high).max_by(|a, b| {
        power_bins
            .get(*a)
            .unwrap_or(&0.0)
            .total_cmp(power_bins.get(*b).unwrap_or(&0.0))
    })
}

/// Total power across a peak's main lobe.
fn lobe_power(power_bins: &[f32], centre: usize, lobe: usize) -> f32 {
    let low = centre.saturating_sub(lobe);
    let high = (centre + lobe).min(power_bins.len().saturating_sub(1));
    power_bins.get(low..=high).unwrap_or(&[]).iter().sum()
}

/// Power-weighted centre of a lobe, in hertz.
fn centroid_hz(power_bins: &[f32], centre: usize, lobe: usize, spacing: f32) -> f32 {
    let low = centre.saturating_sub(lobe);
    let high = (centre + lobe).min(power_bins.len().saturating_sub(1));
    let mut weight = 0.0_f32;
    let mut moment = 0.0_f32;
    for bin in low..=high {
        let power = power_bins.get(bin).copied().unwrap_or(0.0);
        weight += power;
        moment += power * bin as f32;
    }
    if weight > 0.0 {
        moment / weight * spacing
    } else {
        centre as f32 * spacing
    }
}

fn mark(claimed: &mut [bool], centre: usize, lobe: usize) {
    let low = centre.saturating_sub(lobe);
    let high = (centre + lobe).min(claimed.len().saturating_sub(1));
    for slot in claimed.get_mut(low..=high).unwrap_or(&mut []) {
        *slot = true;
    }
}

/// Power to decibels.
fn to_db(power: f32) -> f32 {
    if power > 0.0 {
        (10.0 * power.log10()).max(DISTORTION_FLOOR_DB)
    } else {
        DISTORTION_FLOOR_DB
    }
}

/// An amplitude ratio to decibels.
fn amplitude_db(ratio: f32) -> f32 {
    if ratio > 0.0 {
        (20.0 * ratio.log10()).max(DISTORTION_FLOOR_DB)
    } else {
        DISTORTION_FLOOR_DB
    }
}

#[cfg(test)]
#[allow(clippy::indexing_slicing, clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::{Averaging, Overlap, SpectrumAnalyzer, SpectrumConfig, WindowKind};
    use std::f32::consts::TAU;

    const RATE: f32 = 48_000.0;
    const SIZE: usize = 8192;

    /// Synthesise a tone with specified harmonics, measure it, and return the
    /// power spectrum. Going through the real analyzer rather than a synthetic
    /// spectrum means the test exercises windowing and leakage too.
    fn spectrum_of(fundamental_hz: f32, harmonics: &[(u32, f32)], noise: f32) -> (Vec<f32>, f32) {
        let count = SIZE * 8;
        let mut samples = vec![0.0_f32; count];
        let mut rng = 0x1234_5678_u64;

        for (index, sample) in samples.iter_mut().enumerate() {
            let t = index as f32 / RATE;
            let mut value = 0.5 * (TAU * fundamental_hz * t).sin();
            for (order, amplitude) in harmonics {
                value += 0.5 * amplitude * (TAU * fundamental_hz * *order as f32 * t).sin();
            }
            if noise > 0.0 {
                rng ^= rng >> 12;
                rng ^= rng << 25;
                rng ^= rng >> 27;
                let r =
                    ((rng.wrapping_mul(0x2545_F491_4F6C_DD1D) >> 40) as f32 / 8_388_608.0) - 1.0;
                value += r * noise;
            }
            *sample = value;
        }

        let mut analyzer = SpectrumAnalyzer::new(SpectrumConfig {
            sample_rate: RATE,
            size: SIZE,
            // Blackman-Harris: harmonics can sit 100 dB down, and Hann's
            // sidelobes would bury them in the fundamental's own leakage.
            window: WindowKind::BlackmanHarris,
            overlap: Overlap::Half,
            averaging: Averaging::Infinite,
        });
        analyzer.push(&samples);
        (analyzer.power().to_vec(), analyzer.bin_spacing_hz())
    }

    /// A clean sine has no harmonics, so THD must be tiny rather than merely
    /// small - this is the measurement's own noise floor.
    #[test]
    fn a_clean_sine_measures_near_zero_distortion() {
        let (power, spacing) = spectrum_of(1000.0, &[], 0.0);
        let d = analyse(&power, spacing, None, &DistortionConfig::default()).unwrap();

        assert!(
            d.thd_percent < 0.01,
            "clean sine measured {:.4}% THD",
            d.thd_percent
        );
        assert!(
            (d.fundamental_hz - 1000.0).abs() < 5.0,
            "{}",
            d.fundamental_hz
        );
    }

    /// The defining case: a second harmonic 40 dB down is exactly 1% THD.
    #[test]
    fn a_single_harmonic_gives_the_expected_percentage() {
        // -40 dB is an amplitude ratio of 0.01.
        let (power, spacing) = spectrum_of(1000.0, &[(2, 0.01)], 0.0);
        let d = analyse(&power, spacing, None, &DistortionConfig::default()).unwrap();

        assert!(
            (d.thd_percent - 1.0).abs() < 0.05,
            "expected 1% THD, got {:.4}%",
            d.thd_percent
        );
        assert!(
            (d.thd_db - -40.0).abs() < 0.5,
            "expected -40 dB, got {:.2}",
            d.thd_db
        );

        let h2 = d.harmonic(2).unwrap();
        assert!((h2.percent - 1.0).abs() < 0.05);
        assert!((h2.hz - 2000.0).abs() < 10.0, "H2 at {}", h2.hz);
    }

    /// Harmonics combine in power, so two equal ones are sqrt(2) times one.
    #[test]
    fn harmonics_combine_as_the_root_sum_of_squares() {
        let (power, spacing) = spectrum_of(1000.0, &[(2, 0.01), (3, 0.01)], 0.0);
        let d = analyse(&power, spacing, None, &DistortionConfig::default()).unwrap();

        let expected = (0.01_f32 * 0.01 + 0.01 * 0.01).sqrt() * 100.0;
        assert!(
            (d.thd_percent - expected).abs() < 0.1,
            "expected {expected:.4}%, got {:.4}%",
            d.thd_percent
        );
    }

    #[test]
    fn each_harmonic_is_reported_at_its_own_level() {
        let (power, spacing) = spectrum_of(1000.0, &[(2, 0.01), (3, 0.003), (5, 0.001)], 0.0);
        let d = analyse(&power, spacing, None, &DistortionConfig::default()).unwrap();

        for (order, amplitude) in [(2_u32, 0.01_f32), (3, 0.003), (5, 0.001)] {
            let h = d
                .harmonic(order)
                .unwrap_or_else(|| panic!("H{order} should have been found"));
            let expected = amplitude * 100.0;
            assert!(
                (h.percent - expected).abs() < expected * 0.15,
                "H{order}: got {:.4}%, expected {expected:.4}%",
                h.percent
            );
            assert!(
                (h.hz - 1000.0 * order as f32).abs() < 15.0,
                "H{order} at {}",
                h.hz
            );
        }
    }

    /// Odd-order-only distortion is what symmetric clipping produces, and the
    /// analysis must not invent even orders that are not there.
    #[test]
    fn absent_orders_are_not_invented() {
        let (power, spacing) = spectrum_of(1000.0, &[(3, 0.02), (5, 0.01)], 0.0);
        let d = analyse(&power, spacing, None, &DistortionConfig::default()).unwrap();

        let h2 = d.harmonic(2).unwrap();
        assert!(
            h2.percent < 0.05,
            "H2 should be negligible, got {:.4}%",
            h2.percent
        );
        assert!(d.harmonic(3).unwrap().percent > 1.0);
        assert!(d.harmonic(5).unwrap().percent > 0.5);
    }

    /// THD ignores noise; THD+N does not.
    ///
    /// The two combine in quadrature, which is worth stating because it makes
    /// the numbers unintuitive: 1% distortion with 0.33% noise gives 1.05%
    /// THD+N, not 1.33%. An earlier version of this test used noise that quiet
    /// and then asserted a 50% increase, which the physics does not allow.
    #[test]
    fn thd_plus_noise_exceeds_thd_when_noise_is_present() {
        let quiet = analyse(
            &spectrum_of(1000.0, &[(2, 0.01)], 0.0).0,
            RATE / SIZE as f32,
            None,
            &DistortionConfig::default(),
        )
        .unwrap();

        let (power, spacing) = spectrum_of(1000.0, &[(2, 0.01)], 0.02);
        let noisy = analyse(&power, spacing, None, &DistortionConfig::default()).unwrap();

        // Distortion itself is unchanged - that is the point of measuring both.
        assert!(
            (noisy.thd_percent - quiet.thd_percent).abs() < 0.1,
            "noise must not change THD: {:.4}% vs {:.4}%",
            quiet.thd_percent,
            noisy.thd_percent
        );
        assert!(
            noisy.thd_n_percent > noisy.thd_percent * 1.5,
            "THD+N {:.4}% should clearly exceed THD {:.4}%",
            noisy.thd_n_percent,
            noisy.thd_percent
        );
        assert!(noisy.noise_floor_db > DISTORTION_FLOOR_DB);
        assert!(
            noisy.noise_floor_db > quiet.noise_floor_db + 6.0,
            "the measured floor should rise with the noise"
        );
    }

    #[test]
    fn the_two_figures_converge_without_noise() {
        let (power, spacing) = spectrum_of(1000.0, &[(2, 0.02)], 0.0);
        let d = analyse(&power, spacing, None, &DistortionConfig::default()).unwrap();

        assert!(
            (d.thd_n_percent - d.thd_percent).abs() < d.thd_percent * 0.5,
            "without noise they should be close: {:.4}% vs {:.4}%",
            d.thd_percent,
            d.thd_n_percent
        );
    }

    /// The aliasing trap. A 5 kHz fundamental at 48 kHz has H5 at 25 kHz, above
    /// Nyquist, where it would fold back to 23 kHz and be read as a real
    /// product. It must be excluded and the exclusion reported.
    #[test]
    fn harmonics_above_nyquist_are_excluded_and_counted() {
        let (power, spacing) = spectrum_of(5000.0, &[(2, 0.01)], 0.0);
        let d = analyse(&power, spacing, None, &DistortionConfig::default()).unwrap();

        // Nyquist is 24 kHz, so orders 5 through 10 are out.
        assert_eq!(d.orders_above_nyquist, 6, "H5..H10 should be excluded");
        assert_eq!(d.highest_order(), 4);
        assert!(d.harmonics.iter().all(|h| h.hz < 24_000.0));
        assert!(d.thd_label().contains("to H4"));
    }

    #[test]
    fn a_low_fundamental_keeps_every_order() {
        let (power, spacing) = spectrum_of(200.0, &[(2, 0.01)], 0.0);
        let d = analyse(&power, spacing, None, &DistortionConfig::default()).unwrap();
        assert_eq!(d.orders_above_nyquist, 0);
        assert_eq!(d.highest_order(), 10);
        assert!(!d.thd_label().contains("to H"));
    }

    /// Summing across the lobe rather than reading one bin is what keeps the
    /// answer stable as the tone moves between bin centres.
    #[test]
    fn the_reading_is_stable_across_bin_boundaries() {
        let spacing = RATE / SIZE as f32;
        let mut readings = Vec::new();
        // Walk a tone across one bin in fifths.
        for step in 0..5 {
            let hz = 1000.0 + spacing * step as f32 / 5.0;
            let (power, bin_spacing) = spectrum_of(hz, &[(2, 0.01)], 0.0);
            let d = analyse(&power, bin_spacing, None, &DistortionConfig::default()).unwrap();
            readings.push(d.thd_percent);
        }
        let max = readings.iter().copied().fold(f32::MIN, f32::max);
        let min = readings.iter().copied().fold(f32::MAX, f32::min);
        assert!(
            max - min < 0.15,
            "THD swung from {min:.4}% to {max:.4}% across one bin: {readings:?}"
        );
    }

    /// Supplying the fundamental matters when a harmonic rivals it, because the
    /// loudest-bin heuristic would latch onto the wrong peak.
    #[test]
    fn an_explicit_fundamental_overrides_the_loudest_bin() {
        // A second harmonic louder than the fundamental - severe, but real in a
        // badly driven system.
        let (power, spacing) = spectrum_of(1000.0, &[(2, 2.0)], 0.0);

        let guessed = analyse(&power, spacing, None, &DistortionConfig::default()).unwrap();
        assert!(
            (guessed.fundamental_hz - 2000.0).abs() < 20.0,
            "without a hint it should latch onto the loudest peak, got {}",
            guessed.fundamental_hz
        );

        let told = analyse(&power, spacing, Some(1000.0), &DistortionConfig::default()).unwrap();
        assert!(
            (told.fundamental_hz - 1000.0).abs() < 20.0,
            "with a hint it should use it, got {}",
            told.fundamental_hz
        );
        assert!(
            told.thd_percent > 100.0,
            "H2 above the fundamental is >100% THD"
        );
    }

    #[test]
    fn max_order_is_respected() {
        let config = DistortionConfig {
            max_order: 3,
            ..DistortionConfig::default()
        };
        let (power, spacing) = spectrum_of(1000.0, &[(2, 0.01), (3, 0.01), (4, 0.01)], 0.0);
        let d = analyse(&power, spacing, None, &config).unwrap();

        assert_eq!(d.highest_order(), 3);
        assert!(d.harmonic(4).is_none(), "H4 was not asked for");
    }

    /// The FFI hands this module power recovered from the published decibels
    /// rather than the analyzer's own power array, because the frame carries dB.
    /// That inversion has to be lossless enough not to move the answer.
    #[test]
    fn power_recovered_from_decibels_gives_the_same_answer() {
        let (power, spacing) = spectrum_of(1000.0, &[(2, 0.01), (3, 0.003)], 0.0);

        // Exactly what the engine publishes, then exactly what the FFI does to
        // get back: db = 10*log10(2*power), power = 10^(db/10)/2.
        let round_tripped: Vec<f32> = power
            .iter()
            .map(|p| {
                let db = if *p > 0.0 {
                    (10.0 * (2.0 * p).log10()).max(-200.0)
                } else {
                    -200.0
                };
                10.0_f32.powf(db / 10.0) / 2.0
            })
            .collect();

        let config = DistortionConfig::default();
        let direct = analyse(&power, spacing, None, &config).unwrap();
        let via_db = analyse(&round_tripped, spacing, None, &config).unwrap();

        assert!(
            (direct.thd_percent - via_db.thd_percent).abs() < 0.01,
            "THD moved through the round trip: {:.4}% vs {:.4}%",
            direct.thd_percent,
            via_db.thd_percent
        );
        assert!((direct.fundamental_hz - via_db.fundamental_hz).abs() < 1.0);
        assert_eq!(direct.harmonics.len(), via_db.harmonics.len());
    }

    #[test]
    fn degenerate_input_is_refused() {
        let config = DistortionConfig::default();
        assert!(analyse(&[], 5.86, None, &config).is_none());
        assert!(analyse(&[1.0; 100], 0.0, None, &config).is_none());
        assert!(analyse(&[0.0; 100], 5.86, None, &config).is_none());
    }

    #[test]
    fn silence_produces_no_measurement() {
        let power = vec![0.0_f32; 1024];
        assert!(analyse(&power, 5.86, None, &DistortionConfig::default()).is_none());
    }

    #[test]
    fn every_reported_number_is_finite() {
        let (power, spacing) = spectrum_of(1000.0, &[(2, 0.01), (3, 0.001)], 0.0001);
        let d = analyse(&power, spacing, None, &DistortionConfig::default()).unwrap();

        assert!(d.thd_percent.is_finite() && d.thd_db.is_finite());
        assert!(d.thd_n_percent.is_finite() && d.thd_n_db.is_finite());
        assert!(d.fundamental_db.is_finite() && d.noise_floor_db.is_finite());
        for h in &d.harmonics {
            assert!(h.level_db.is_finite() && h.relative_db.is_finite() && h.percent.is_finite());
        }
    }
}
