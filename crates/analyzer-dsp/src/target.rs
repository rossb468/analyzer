//! Target curves: the response a correction is aiming at.
//!
//! A measurement on its own says what a room does. It does not say what it
//! should do, and "flat" is the wrong answer for a room: a loudspeaker measured
//! anechoically flat sounds thin in a room, because the ear expects the bass
//! lift that a real space produces. Every serious correction is fitted against a
//! target that is not flat.
//!
//! Three parameterised shapes plus a file:
//!
//! - [`TargetShape::Flat`] — the reference, and what a transfer function of a
//!   single loudspeaker measured close up should aim at.
//! - [`TargetShape::Tilt`] — a constant slope in decibels per octave. The whole
//!   of some house curves.
//! - [`TargetShape::Room`] — a bass shelf plus a tilt, which is the shape of
//!   most published room targets.
//! - [`TargetShape::Custom`] — points from a file, interpolated in decibels
//!   against **log** frequency for the same reason calibration files are: the
//!   points are sparse and roughly log-spaced, so interpolating against linear
//!   frequency badly misplaces everything below a few hundred hertz.
//!
//! The parameters are exposed rather than baked in. These shapes are drawn from
//! common practice and the defaults are reasonable, but this does not claim to
//! reproduce any specific published target exactly, and a curve that claimed to
//! would be wrong the moment its author revised it.
//!
//! ## Targets are relative
//!
//! A target says nothing about absolute level: +6 dB of bass lift is lift
//! relative to the rest of the curve, not an absolute sound pressure. Drawing
//! one against a measurement therefore needs an offset, and
//! [`TargetCurve::aligned_to`] picks the one that makes the average difference
//! over a chosen band zero. Without it the target floats somewhere unrelated to
//! the measurement and every error the optimiser computes is dominated by that
//! constant.

/// Where a tilt pivots, and the band a room target is flat in.
pub const REFERENCE_HZ: f32 = 1000.0;

/// How sharply the room shelf turns over.
///
/// Two makes the transition a gentle first-order-like shelf on a log axis,
/// reaching half the shelf gain at the transition frequency. Higher would be a
/// harder knee than any real room boundary produces.
const SHELF_ORDER: f32 = 2.0;

/// The shape of a target.
#[derive(Debug, Clone, PartialEq, Default)]
pub enum TargetShape {
    /// Flat at every frequency.
    #[default]
    Flat,
    /// A constant slope, zero at [`REFERENCE_HZ`].
    Tilt {
        /// Decibels per octave. Negative slopes downward with frequency, which
        /// is the direction room targets go.
        db_per_octave: f32,
    },
    /// A bass shelf with an optional tilt above it.
    Room {
        /// Lift at the bottom of the band, in decibels.
        shelf_db: f32,
        /// Where the shelf reaches half its lift.
        transition_hz: f32,
        /// Slope applied across the whole range, zero at [`REFERENCE_HZ`].
        db_per_octave: f32,
    },
    /// `(hz, db)` points, sorted ascending and interpolated in log frequency.
    Custom {
        /// Sorted by frequency, with non-positive frequencies removed.
        points: Vec<(f32, f32)>,
    },
}

impl TargetShape {
    /// A room target with defaults drawn from common practice.
    pub fn room() -> Self {
        Self::Room {
            shelf_db: 6.0,
            transition_hz: 105.0,
            db_per_octave: -0.5,
        }
    }

    /// Build a custom shape, sorting and dropping unusable points.
    ///
    /// A file listing a DC row, or arriving unsorted, still works - the same
    /// tolerance calibration files get, and for the same reason: these are
    /// other people's exports.
    pub fn custom(mut points: Vec<(f32, f32)>) -> Self {
        points.retain(|(hz, db)| *hz > 0.0 && hz.is_finite() && db.is_finite());
        points.sort_by(|a, b| a.0.total_cmp(&b.0));
        Self::Custom { points }
    }

    /// Level in decibels at `hz`, before any alignment offset.
    pub fn db_at(&self, hz: f32) -> f32 {
        if !hz.is_finite() || hz <= 0.0 {
            return 0.0;
        }
        match self {
            Self::Flat => 0.0,
            Self::Tilt { db_per_octave } => tilt(*db_per_octave, hz),
            Self::Room {
                shelf_db,
                transition_hz,
                db_per_octave,
            } => {
                let shelf = if *transition_hz > 0.0 {
                    shelf_db / (1.0 + (hz / transition_hz).powf(SHELF_ORDER))
                } else {
                    0.0
                };
                shelf + tilt(*db_per_octave, hz)
            }
            Self::Custom { points } => interpolate(points, hz),
        }
    }
}

/// A slope in decibels per octave, zero at the reference frequency.
fn tilt(db_per_octave: f32, hz: f32) -> f32 {
    db_per_octave * (hz / REFERENCE_HZ).log2()
}

/// Linear interpolation in decibels against log frequency.
///
/// Outside the listed range the curve holds its end value rather than
/// extrapolating. A calibration or target file that stops at 20 kHz says nothing
/// about 22 kHz, and inventing a continued slope there would be fabricating
/// data at exactly the frequencies where it is least reliable.
fn interpolate(points: &[(f32, f32)], hz: f32) -> f32 {
    match points {
        [] => 0.0,
        [(_, db)] => *db,
        _ => {
            let first = points[0];
            let last = points[points.len() - 1];
            if hz <= first.0 {
                return first.1;
            }
            if hz >= last.0 {
                return last.1;
            }

            let index = points.partition_point(|(point_hz, _)| *point_hz <= hz);
            let (low_hz, low_db) = points[index - 1];
            let (high_hz, high_db) = points[index];
            if high_hz <= low_hz {
                return low_db;
            }

            let t = (hz / low_hz).ln() / (high_hz / low_hz).ln();
            low_db + t * (high_db - low_db)
        }
    }
}

/// A target curve: a shape plus the offset that puts it on a measurement.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct TargetCurve {
    shape: TargetShape,
    offset_db: f32,
}

impl TargetCurve {
    /// A target with no offset applied.
    pub fn new(shape: TargetShape) -> Self {
        Self {
            shape,
            offset_db: 0.0,
        }
    }

    /// The shape being evaluated.
    pub fn shape(&self) -> &TargetShape {
        &self.shape
    }

    /// The alignment offset currently applied.
    pub fn offset_db(&self) -> f32 {
        self.offset_db
    }

    /// Replace the shape, keeping the offset.
    pub fn set_shape(&mut self, shape: TargetShape) {
        self.shape = shape;
    }

    /// Set the offset by hand.
    pub fn set_offset_db(&mut self, offset_db: f32) {
        if offset_db.is_finite() {
            self.offset_db = offset_db;
        }
    }

    /// Level in decibels at `hz`, including the offset.
    pub fn db_at(&self, hz: f32) -> f32 {
        self.shape.db_at(hz) + self.offset_db
    }

    /// Evaluate across `frequencies`, writing into `out`.
    ///
    /// Writes `min(frequencies.len(), out.len())` values, so a caller cannot
    /// overrun either side by getting the lengths out of step.
    pub fn write_levels(&self, frequencies: &[f32], out: &mut [f32]) {
        for (level, hz) in out.iter_mut().zip(frequencies) {
            *level = self.db_at(*hz);
        }
    }

    /// Choose the offset that makes the mean difference over a band zero.
    ///
    /// Only frequencies inside `[from_hz, to_hz]` with a finite measured level
    /// count. A band containing nothing usable leaves the offset alone rather
    /// than moving the curve somewhere arbitrary.
    ///
    /// The default band deliberately excludes the bass, where the room's own
    /// modes swing the measurement by more than the whole target does, and the
    /// top octave, where a microphone's own response is least trustworthy.
    /// Aligning across the full range would let one 15 dB null decide where the
    /// target sits.
    pub fn align_to(&mut self, frequencies: &[f32], measured_db: &[f32], from_hz: f32, to_hz: f32) {
        let mut sum = 0.0f64;
        let mut count = 0u32;

        for (hz, measured) in frequencies.iter().zip(measured_db) {
            if *hz < from_hz || *hz > to_hz || !measured.is_finite() {
                continue;
            }
            sum += f64::from(*measured - self.shape.db_at(*hz));
            count += 1;
        }

        if count > 0 {
            let offset = (sum / f64::from(count)) as f32;
            if offset.is_finite() {
                self.offset_db = offset;
            }
        }
    }

    /// The same, returning a new curve.
    pub fn aligned_to(
        mut self,
        frequencies: &[f32],
        measured_db: &[f32],
        from_hz: f32,
        to_hz: f32,
    ) -> Self {
        self.align_to(frequencies, measured_db, from_hz, to_hz);
        self
    }

    /// How far the measurement sits above the target, per frequency.
    ///
    /// This is the sign an equaliser has to undo: a positive error is too much
    /// energy and wants a cut.
    pub fn write_error(&self, frequencies: &[f32], measured_db: &[f32], out: &mut [f32]) {
        for ((error, hz), measured) in out.iter_mut().zip(frequencies).zip(measured_db) {
            *error = measured - self.db_at(*hz);
        }
    }
}

/// Band the alignment uses unless told otherwise.
pub const ALIGN_FROM_HZ: f32 = 200.0;
/// Upper end of the default alignment band.
pub const ALIGN_TO_HZ: f32 = 2000.0;

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    fn log_sweep(from: f32, to: f32, count: usize) -> Vec<f32> {
        (0..count)
            .map(|i| {
                let t = i as f32 / (count - 1) as f32;
                from * (to / from).powf(t)
            })
            .collect()
    }

    #[test]
    fn flat_is_flat_everywhere() {
        let target = TargetCurve::new(TargetShape::Flat);
        for hz in [20.0, 100.0, 1000.0, 20_000.0] {
            assert_eq!(target.db_at(hz), 0.0);
        }
    }

    /// A tilt is defined per octave, so a doubling must move it by exactly the
    /// stated amount.
    #[test]
    fn a_tilt_moves_by_its_slope_per_octave() {
        let target = TargetCurve::new(TargetShape::Tilt {
            db_per_octave: -1.5,
        });
        assert!((target.db_at(REFERENCE_HZ)).abs() < 1e-6);
        assert!((target.db_at(2000.0) + 1.5).abs() < 1e-5);
        assert!((target.db_at(500.0) - 1.5).abs() < 1e-5);
        assert!((target.db_at(4000.0) + 3.0).abs() < 1e-5);
    }

    #[test]
    fn a_room_shelf_reaches_half_its_lift_at_the_transition() {
        let shape = TargetShape::Room {
            shelf_db: 6.0,
            transition_hz: 100.0,
            db_per_octave: 0.0,
        };
        assert!((shape.db_at(100.0) - 3.0).abs() < 1e-5);
        // Well below, essentially the full shelf; well above, essentially none.
        assert!(shape.db_at(10.0) > 5.9);
        assert!(shape.db_at(2000.0) < 0.1);
    }

    #[test]
    fn the_default_room_target_lifts_the_bass_and_falls_with_frequency() {
        let shape = TargetShape::room();
        assert!(shape.db_at(20.0) > shape.db_at(200.0));
        assert!(shape.db_at(1000.0) > shape.db_at(10_000.0));
    }

    #[test]
    fn custom_points_are_sorted_and_cleaned() {
        let shape = TargetShape::custom(vec![
            (1000.0, -2.0),
            (0.0, 99.0),
            (100.0, 3.0),
            (f32::NAN, 1.0),
        ]);
        let TargetShape::Custom { points } = &shape else {
            panic!("expected custom");
        };
        assert_eq!(points, &[(100.0, 3.0), (1000.0, -2.0)]);
    }

    /// Interpolating in linear frequency would put the midpoint at 550 Hz;
    /// in log frequency it is at about 316 Hz, which is where a sparse
    /// log-spaced file actually means it.
    #[test]
    fn custom_interpolation_is_logarithmic() {
        let shape = TargetShape::custom(vec![(100.0, 0.0), (1000.0, 10.0)]);
        let midpoint = shape.db_at(316.227_77);
        assert!((midpoint - 5.0).abs() < 0.01, "got {midpoint}");
    }

    /// A file that stops at 20 kHz says nothing about 22 kHz.
    #[test]
    fn custom_curves_hold_their_ends_rather_than_extrapolating() {
        let shape = TargetShape::custom(vec![(100.0, 3.0), (1000.0, -2.0)]);
        assert_eq!(shape.db_at(20.0), 3.0);
        assert_eq!(shape.db_at(20_000.0), -2.0);
    }

    #[test]
    fn an_empty_or_single_point_custom_curve_is_usable() {
        assert_eq!(TargetShape::custom(vec![]).db_at(1000.0), 0.0);
        assert_eq!(TargetShape::custom(vec![(500.0, 4.0)]).db_at(1000.0), 4.0);
    }

    /// Alignment is what makes a relative target drawable against an absolute
    /// measurement.
    #[test]
    fn alignment_centres_the_target_on_the_measurement() {
        let frequencies = log_sweep(20.0, 20_000.0, 512);
        let measured: Vec<f32> = frequencies.iter().map(|_| -35.0).collect();

        let target = TargetCurve::new(TargetShape::Flat).aligned_to(
            &frequencies,
            &measured,
            ALIGN_FROM_HZ,
            ALIGN_TO_HZ,
        );
        assert!((target.offset_db() + 35.0).abs() < 1e-4);
        assert!((target.db_at(1000.0) + 35.0).abs() < 1e-4);
    }

    /// Only the alignment band counts, so a deep null outside it must not drag
    /// the whole curve down.
    #[test]
    fn alignment_ignores_everything_outside_the_band() {
        let frequencies = log_sweep(20.0, 20_000.0, 512);
        let measured: Vec<f32> = frequencies
            .iter()
            .map(|hz| if *hz < 100.0 { -90.0 } else { -30.0 })
            .collect();

        let target = TargetCurve::new(TargetShape::Flat).aligned_to(
            &frequencies,
            &measured,
            ALIGN_FROM_HZ,
            ALIGN_TO_HZ,
        );
        assert!(
            (target.offset_db() + 30.0).abs() < 0.5,
            "{}",
            target.offset_db()
        );
    }

    #[test]
    fn alignment_over_an_empty_band_leaves_the_offset_alone() {
        let mut target = TargetCurve::new(TargetShape::Flat);
        target.set_offset_db(-12.0);
        target.align_to(&[100.0], &[-40.0], 1000.0, 2000.0);
        assert_eq!(target.offset_db(), -12.0);
    }

    #[test]
    fn alignment_skips_non_finite_measurements() {
        let frequencies = [500.0, 1000.0, 1500.0];
        let measured = [f32::NEG_INFINITY, -20.0, f32::NAN];
        let target = TargetCurve::new(TargetShape::Flat).aligned_to(
            &frequencies,
            &measured,
            ALIGN_FROM_HZ,
            ALIGN_TO_HZ,
        );
        assert!((target.offset_db() + 20.0).abs() < 1e-4);
    }

    /// Positive error means too much energy, which is what wants a cut.
    #[test]
    fn error_is_measured_minus_target() {
        let frequencies = [100.0, 1000.0];
        let measured = [-20.0, -30.0];
        let mut target = TargetCurve::new(TargetShape::Flat);
        target.set_offset_db(-25.0);

        let mut error = [0.0; 2];
        target.write_error(&frequencies, &measured, &mut error);
        assert!((error[0] - 5.0).abs() < 1e-5);
        assert!((error[1] + 5.0).abs() < 1e-5);
    }

    /// Mismatched lengths must truncate rather than panic or overrun.
    #[test]
    fn writing_levels_stops_at_the_shorter_side() {
        let target = TargetCurve::new(TargetShape::Flat);
        let mut out = [7.0; 4];
        target.write_levels(&[100.0, 200.0], &mut out);
        assert_eq!(out[0], 0.0);
        assert_eq!(out[1], 0.0);
        assert_eq!(out[2], 7.0, "beyond the frequencies given, untouched");
    }

    #[test]
    fn a_non_positive_frequency_evaluates_to_zero_rather_than_nan() {
        for shape in [
            TargetShape::Flat,
            TargetShape::room(),
            TargetShape::Tilt {
                db_per_octave: -1.0,
            },
        ] {
            assert_eq!(shape.db_at(0.0), 0.0);
            assert_eq!(shape.db_at(-100.0), 0.0);
            assert!(shape.db_at(f32::NAN) == 0.0);
        }
    }

    #[test]
    fn a_non_finite_offset_is_refused() {
        let mut target = TargetCurve::new(TargetShape::Flat);
        target.set_offset_db(-10.0);
        target.set_offset_db(f32::NAN);
        assert_eq!(target.offset_db(), -10.0);
    }
}
