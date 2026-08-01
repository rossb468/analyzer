//! Reducing a spectrum to one value per pixel column.
//!
//! An FFT produces linearly-spaced bins; a log frequency axis needs points
//! spaced logarithmically. The mismatch runs in both directions at once, which
//! is what makes this more than a resample:
//!
//! - **High frequencies are over-sampled.** At 48 kHz with a 4096-point FFT the
//!   top octave holds over a thousand bins inside maybe 150 pixels. Several bins
//!   land in every column and have to be combined.
//! - **Low frequencies are under-sampled.** Below a couple of hundred hertz the
//!   bins are further apart than the pixels, so most columns contain no bin at
//!   all and have to be interpolated.
//!
//! Getting either case wrong is visible. Picking one bin per column in the dense
//! region makes narrow peaks flicker in and out as the display resizes; leaving
//! empty columns in the sparse region draws a staircase in the bass.

use crate::axis::FrequencyAxis;

/// How to combine several bins landing in one pixel column.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Reduction {
    /// Loudest bin in the column.
    ///
    /// The default, because losing a narrow peak is the more visible error. A
    /// resonance one bin wide is exactly what a measurement is looking for, and
    /// averaging it away in the display would hide it.
    #[default]
    Max,
    /// Mean level across the column, averaged in the power domain.
    ///
    /// Averaging decibels directly is wrong — it is a geometric mean of power
    /// and reads several dB low on a peaky spectrum — so this converts, averages
    /// and converts back.
    Mean,
}

/// One reduced trace, ready to be turned into geometry.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct Trace {
    /// One level per pixel column, in decibels.
    pub points: Vec<f32>,
}

impl Trace {
    /// An empty trace sized for `width` columns.
    pub fn with_width(width: usize) -> Self {
        Self {
            points: vec![f32::NEG_INFINITY; width],
        }
    }

    /// Number of columns.
    pub fn len(&self) -> usize {
        self.points.len()
    }

    /// Whether the trace has no columns.
    pub fn is_empty(&self) -> bool {
        self.points.is_empty()
    }
}

/// Reduce `bins` onto the axis, writing one value per pixel column.
///
/// `bins` are levels in decibels, bin `k` sitting at `k * bin_spacing_hz`.
/// The output is resized to `columns`.
///
/// Bin 0 is skipped: it is DC, has no place on a log frequency axis, and would
/// otherwise be smeared across the leftmost column.
pub fn reduce(
    bins: &[f32],
    bin_spacing_hz: f32,
    axis: &FrequencyAxis,
    columns: usize,
    mode: Reduction,
    out: &mut Trace,
) {
    out.points.clear();
    out.points.resize(columns, f32::NEG_INFINITY);

    if bins.len() < 2 || bin_spacing_hz <= 0.0 || columns == 0 {
        return;
    }

    let column_width = axis.width() / columns as f32;
    let floor_db = f32::NEG_INFINITY;

    // Walk columns rather than bins. Bins can be visited more than once (sparse
    // region) or many times per column (dense region), so the column is the
    // thing that must be covered exactly once.
    for (index, slot) in out.points.iter_mut().enumerate() {
        let x_left = index as f32 * column_width;
        let x_right = x_left + column_width;
        let hz_left = axis.x_to_freq(x_left);
        let hz_right = axis.x_to_freq(x_right);

        // Bin indices spanning this column, skipping DC.
        let first = (hz_left / bin_spacing_hz).ceil().max(1.0) as usize;
        let last = (hz_right / bin_spacing_hz).floor() as usize;
        let last = last.min(bins.len().saturating_sub(1));

        if first <= last {
            // Dense region: combine every bin in the column.
            let slice = bins.get(first..=last).unwrap_or(&[]);
            *slot = match mode {
                Reduction::Max => slice.iter().copied().fold(floor_db, f32::max),
                Reduction::Mean => mean_db(slice),
            };
        } else {
            // Sparse region: no bin falls inside, so interpolate between the two
            // that straddle the column centre.
            let centre_hz = axis.x_to_freq((x_left + x_right) * 0.5);
            *slot = interpolate(bins, bin_spacing_hz, centre_hz);
        }
    }
}

/// How to combine values that are not decibels.
///
/// [`Reduction`] converts through the power domain, which is right for levels
/// and nonsense for anything else. Coherence is a ratio and phase is an angle;
/// running either through `10^(x/10)` produces a number with no meaning.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum LinearReduction {
    /// Smallest value in the column.
    ///
    /// The default, and the right one for coherence: showing the best value in
    /// a pixel column would hide exactly the dropouts a user is looking for.
    #[default]
    Min,
    /// Arithmetic mean.
    Mean,
    /// Circular mean, for angles in degrees.
    ///
    /// Averages unit vectors rather than numbers, so a column holding +179 deg
    /// and -179 deg reads 180 rather than 0.
    Circular,
}

/// Reduce non-decibel `values` onto the axis, one per pixel column.
///
/// Mirrors [`reduce`] exactly - same column walk, same DC skip, same sparse
/// interpolation - and differs only in never treating a value as a level.
/// Columns with no data read `fill` instead of negative infinity, since zero is
/// a meaningful coherence and negative infinity is not.
pub fn reduce_linear(
    values: &[f32],
    bin_spacing_hz: f32,
    axis: &FrequencyAxis,
    columns: usize,
    mode: LinearReduction,
    fill: f32,
    out: &mut Trace,
) {
    out.points.clear();
    out.points.resize(columns, fill);

    if values.len() < 2 || bin_spacing_hz <= 0.0 || columns == 0 {
        return;
    }

    let column_width = axis.width() / columns as f32;

    for (index, slot) in out.points.iter_mut().enumerate() {
        let x_left = index as f32 * column_width;
        let x_right = x_left + column_width;
        let hz_left = axis.x_to_freq(x_left);
        let hz_right = axis.x_to_freq(x_right);

        let first = (hz_left / bin_spacing_hz).ceil().max(1.0) as usize;
        let last =
            ((hz_right / bin_spacing_hz).floor() as usize).min(values.len().saturating_sub(1));

        if first <= last {
            let slice = values.get(first..=last).unwrap_or(&[]);
            *slot = match mode {
                LinearReduction::Min => slice.iter().copied().fold(f32::INFINITY, f32::min),
                LinearReduction::Mean => slice.iter().sum::<f32>() / slice.len() as f32,
                LinearReduction::Circular => circular_mean_degrees(slice),
            };
        } else {
            let centre_hz = axis.x_to_freq((x_left + x_right) * 0.5);
            *slot = match mode {
                // Interpolating an angle linearly wraps badly, so the nearest
                // bin is used instead. In a sparse region adjacent bins are
                // more than a pixel apart, and the error is invisible.
                LinearReduction::Circular => nearest(values, bin_spacing_hz, centre_hz, fill),
                _ => interpolate_linear(values, bin_spacing_hz, centre_hz, fill),
            };
        }
    }
}

/// Mean direction of angles in degrees.
fn circular_mean_degrees(degrees: &[f32]) -> f32 {
    let (mut x, mut y) = (0.0_f32, 0.0_f32);
    for angle in degrees {
        let radians = angle.to_radians();
        x += radians.cos();
        y += radians.sin();
    }
    if x == 0.0 && y == 0.0 {
        // Perfectly opposed angles cancel and have no mean direction. Zero is
        // as good an answer as any, and does not produce a NaN.
        return 0.0;
    }
    y.atan2(x).to_degrees()
}

fn nearest(values: &[f32], bin_spacing_hz: f32, hz: f32, fill: f32) -> f32 {
    let index = (hz / bin_spacing_hz).round().max(1.0) as usize;
    values.get(index).copied().unwrap_or(fill)
}

fn interpolate_linear(values: &[f32], bin_spacing_hz: f32, hz: f32, fill: f32) -> f32 {
    let exact = hz / bin_spacing_hz;
    if exact <= 1.0 {
        return values.get(1).copied().unwrap_or(fill);
    }
    let lower = exact.floor() as usize;
    let (Some(a), Some(b)) = (values.get(lower), values.get(lower + 1)) else {
        return values.last().copied().unwrap_or(fill);
    };
    a + (b - a) * (exact - lower as f32)
}

/// Mean of decibel values, averaged as power.
fn mean_db(levels: &[f32]) -> f32 {
    if levels.is_empty() {
        return f32::NEG_INFINITY;
    }
    let sum: f32 = levels.iter().map(|db| 10.0_f32.powf(db / 10.0)).sum();
    let mean = sum / levels.len() as f32;
    if mean > 0.0 {
        10.0 * mean.log10()
    } else {
        f32::NEG_INFINITY
    }
}

/// Linear interpolation between the bins either side of `hz`.
///
/// Interpolating in decibels rather than power is deliberate here: this is a
/// display value between two known points, and dB-linear is what looks right on
/// a dB axis.
fn interpolate(bins: &[f32], bin_spacing_hz: f32, hz: f32) -> f32 {
    let exact = hz / bin_spacing_hz;
    if exact <= 1.0 {
        return bins.get(1).copied().unwrap_or(f32::NEG_INFINITY);
    }
    let lower = exact.floor() as usize;
    let upper = lower + 1;
    let (Some(a), Some(b)) = (bins.get(lower), bins.get(upper)) else {
        return bins.last().copied().unwrap_or(f32::NEG_INFINITY);
    };
    let t = exact - lower as f32;
    a + (b - a) * t
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::indexing_slicing)]
mod tests {
    use super::*;

    const SPACING: f32 = 48_000.0 / 4096.0; // 11.71875 Hz

    /// Coherence must show the worst value in a column, not the best.
    #[test]
    fn linear_min_keeps_the_dropout() {
        let axis = FrequencyAxis::audible(1000.0);
        let mut values = vec![1.0_f32; 2049];
        values[1000] = 0.2;
        let mut trace = Trace::default();
        // Few columns, so many bins land in each and the dropout must survive.
        reduce_linear(
            &values,
            SPACING,
            &axis,
            64,
            LinearReduction::Min,
            0.0,
            &mut trace,
        );
        let lowest = trace.points.iter().copied().fold(f32::INFINITY, f32::min);
        assert!(
            (lowest - 0.2).abs() < 1e-6,
            "dropout was lost, got {lowest}"
        );
    }

    /// The wrap case that a plain arithmetic mean gets exactly backwards.
    #[test]
    fn circular_mean_averages_across_the_wrap() {
        assert!((circular_mean_degrees(&[179.0, -179.0]).abs() - 180.0).abs() < 0.01);
        assert!(circular_mean_degrees(&[10.0, -10.0]).abs() < 0.01);
        assert!((circular_mean_degrees(&[90.0, 0.0]) - 45.0).abs() < 0.01);
    }

    /// Opposed angles have no mean direction; the answer must still be a number.
    #[test]
    fn circular_mean_of_opposed_angles_is_not_nan() {
        assert!(circular_mean_degrees(&[0.0, 180.0]).is_finite());
    }

    /// An empty column reads the fill value, because zero coherence is a real
    /// reading and negative infinity is not.
    #[test]
    fn linear_reduction_fills_rather_than_using_negative_infinity() {
        let axis = FrequencyAxis::audible(1000.0);
        let mut trace = Trace::default();
        reduce_linear(
            &[],
            SPACING,
            &axis,
            32,
            LinearReduction::Min,
            0.0,
            &mut trace,
        );
        assert_eq!(trace.len(), 32);
        assert!(trace.points.iter().all(|v| *v == 0.0));
    }

    /// A constant field survives the round trip regardless of column density.
    #[test]
    fn a_flat_linear_field_stays_flat() {
        let axis = FrequencyAxis::audible(1000.0);
        let mut trace = Trace::default();
        for columns in [37, 512, 4000] {
            reduce_linear(
                &vec![0.75_f32; 2049],
                SPACING,
                &axis,
                columns,
                LinearReduction::Mean,
                0.0,
                &mut trace,
            );
            for (i, value) in trace.points.iter().enumerate() {
                assert!(
                    (value - 0.75).abs() < 1e-4,
                    "{columns} columns, column {i} read {value}"
                );
            }
        }
    }

    fn flat_bins(level: f32) -> Vec<f32> {
        vec![level; 2049]
    }

    #[test]
    fn a_flat_spectrum_stays_flat() {
        let axis = FrequencyAxis::audible(1000.0);
        let mut trace = Trace::default();
        reduce(
            &flat_bins(-40.0),
            SPACING,
            &axis,
            1000,
            Reduction::Max,
            &mut trace,
        );

        assert_eq!(trace.len(), 1000);
        for (i, level) in trace.points.iter().enumerate() {
            assert!(
                (level - -40.0).abs() < 0.01,
                "column {i} read {level}, expected -40"
            );
        }
    }

    /// The property the whole module exists for: no column may be left unfilled,
    /// including down in the bass where bins are sparser than pixels.
    #[test]
    fn every_column_is_filled_even_where_bins_are_sparse() {
        let axis = FrequencyAxis::audible(1600.0);
        let mut trace = Trace::default();
        reduce(
            &flat_bins(-30.0),
            SPACING,
            &axis,
            1600,
            Reduction::Max,
            &mut trace,
        );

        for (i, level) in trace.points.iter().enumerate() {
            assert!(
                level.is_finite(),
                "column {i} ({:.1} Hz) was never filled",
                axis.x_to_freq(i as f32)
            );
        }
    }

    /// A single loud bin must survive reduction at any width. Losing it is the
    /// failure mode that makes a display untrustworthy.
    #[test]
    fn a_narrow_peak_survives_at_every_width() {
        let mut bins = flat_bins(-90.0);
        // Bin 853 is about 10 kHz, deep in the over-sampled region.
        bins[853] = 0.0;

        for width in [200.0_f32, 640.0, 1000.0, 1440.0, 3000.0] {
            let axis = FrequencyAxis::audible(width);
            let mut trace = Trace::default();
            reduce(
                &bins,
                SPACING,
                &axis,
                width as usize,
                Reduction::Max,
                &mut trace,
            );

            let loudest = trace
                .points
                .iter()
                .copied()
                .fold(f32::NEG_INFINITY, f32::max);
            assert!(
                loudest > -1.0,
                "peak lost at width {width}: loudest was {loudest}"
            );
        }
    }

    /// Mean must average power, not decibels. A column holding 0 dB and -20 dB
    /// averages to -2.4 dB in power terms; averaging the decibels would give -10.
    #[test]
    fn mean_averages_power_not_decibels() {
        let levels = [0.0_f32, -20.0];
        let mean = mean_db(&levels);
        let expected = 10.0 * ((1.0 + 0.01) / 2.0f32).log10();
        assert!(
            (mean - expected).abs() < 1e-4,
            "got {mean}, want {expected}"
        );
        assert!(mean > -10.0, "power mean must exceed the decibel mean");
    }

    #[test]
    fn max_and_mean_agree_on_a_flat_spectrum() {
        let axis = FrequencyAxis::audible(800.0);
        let mut peak = Trace::default();
        let mut mean = Trace::default();
        reduce(
            &flat_bins(-55.0),
            SPACING,
            &axis,
            800,
            Reduction::Max,
            &mut peak,
        );
        reduce(
            &flat_bins(-55.0),
            SPACING,
            &axis,
            800,
            Reduction::Mean,
            &mut mean,
        );

        for (a, b) in peak.points.iter().zip(&mean.points) {
            assert!((a - b).abs() < 0.01);
        }
    }

    /// Max must dominate mean wherever a column holds a peak among quiet bins.
    #[test]
    fn max_reads_higher_than_mean_on_a_peaky_spectrum() {
        let mut bins = flat_bins(-90.0);
        for k in (100..2000).step_by(37) {
            bins[k] = -10.0;
        }
        let axis = FrequencyAxis::audible(400.0);
        let mut peak = Trace::default();
        let mut mean = Trace::default();
        reduce(&bins, SPACING, &axis, 400, Reduction::Max, &mut peak);
        reduce(&bins, SPACING, &axis, 400, Reduction::Mean, &mut mean);

        let peak_total: f32 = peak.points.iter().sum();
        let mean_total: f32 = mean.points.iter().sum();
        assert!(peak_total > mean_total, "{peak_total} vs {mean_total}");
    }

    #[test]
    fn a_rising_spectrum_stays_monotonic_through_reduction() {
        // Level rising with bin index must not develop dips.
        let bins: Vec<f32> = (0..2049).map(|k| -100.0 + k as f32 * 0.04).collect();
        let axis = FrequencyAxis::audible(900.0);
        let mut trace = Trace::default();
        reduce(&bins, SPACING, &axis, 900, Reduction::Max, &mut trace);

        for pair in trace.points.windows(2) {
            assert!(
                pair[1] >= pair[0] - 0.01,
                "reduction introduced a dip: {} -> {}",
                pair[0],
                pair[1]
            );
        }
    }

    #[test]
    fn dc_is_excluded() {
        let mut bins = flat_bins(-80.0);
        // A huge DC offset must not leak into the visible bass.
        bins[0] = 40.0;
        let axis = FrequencyAxis::audible(500.0);
        let mut trace = Trace::default();
        reduce(&bins, SPACING, &axis, 500, Reduction::Max, &mut trace);

        let loudest = trace
            .points
            .iter()
            .copied()
            .fold(f32::NEG_INFINITY, f32::max);
        assert!(loudest < -70.0, "DC leaked into the trace: {loudest}");
    }

    #[test]
    fn degenerate_inputs_produce_an_empty_but_sized_trace() {
        let axis = FrequencyAxis::audible(100.0);
        let mut trace = Trace::default();

        reduce(&[], SPACING, &axis, 100, Reduction::Max, &mut trace);
        assert_eq!(trace.len(), 100);

        reduce(
            &flat_bins(-20.0),
            0.0,
            &axis,
            100,
            Reduction::Max,
            &mut trace,
        );
        assert_eq!(trace.len(), 100);

        reduce(
            &flat_bins(-20.0),
            SPACING,
            &axis,
            0,
            Reduction::Max,
            &mut trace,
        );
        assert!(trace.is_empty());
    }

    /// Reuse must not leave stale values behind — the trace is recycled every
    /// frame, so a shrinking width could otherwise show last frame's tail.
    #[test]
    fn reusing_a_trace_does_not_leak_old_data() {
        let axis = FrequencyAxis::audible(1000.0);
        let mut trace = Trace::default();
        reduce(
            &flat_bins(0.0),
            SPACING,
            &axis,
            1000,
            Reduction::Max,
            &mut trace,
        );
        reduce(
            &flat_bins(-80.0),
            SPACING,
            &axis,
            40,
            Reduction::Max,
            &mut trace,
        );

        assert_eq!(trace.len(), 40);
        assert!(trace.points.iter().all(|l| (*l - -80.0).abs() < 0.5));
    }
}
