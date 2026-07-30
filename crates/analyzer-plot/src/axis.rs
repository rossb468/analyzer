//! Axis mappings between data and screen space.
//!
//! These live in the core rather than the UI on purpose. Cursor readout,
//! hit-testing, marker placement and the drawn geometry must all agree exactly,
//! and the only way to guarantee that is for one implementation to answer every
//! question. A Swift or C# layer reimplementing "which frequency is under this
//! pixel" will drift from what was actually plotted, and the resulting
//! disagreement is subtle enough to survive a long time.
//!
//! Screen space follows the usual convention: x increases rightwards, y
//! increases **downwards**, so the loudest level sits at y = 0.

use std::fmt;

/// A gridline.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Tick {
    /// Value in data units — hertz or decibels.
    pub value: f32,
    /// Position in pixels along the axis.
    pub position: f32,
    /// Whether this deserves a label and a heavier line.
    pub major: bool,
}

/// Logarithmic frequency axis.
///
/// Log spacing is not cosmetic: hearing is roughly logarithmic in frequency, so
/// a linear axis wastes most of its width on the top octave and crushes the
/// bass into nothing.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FrequencyAxis {
    min_hz: f32,
    max_hz: f32,
    width: f32,
    log_min: f32,
    log_span: f32,
}

impl FrequencyAxis {
    /// Build an axis spanning `min_hz..=max_hz` across `width` pixels.
    ///
    /// # Panics
    ///
    /// Panics if either frequency is non-positive, if `max_hz` is not above
    /// `min_hz`, or if `width` is not positive. A log axis through zero has no
    /// meaning, and silently substituting a default would hide a caller's bug.
    pub fn new(min_hz: f32, max_hz: f32, width: f32) -> Self {
        assert!(
            min_hz > 0.0 && max_hz > min_hz,
            "need 0 < min_hz < max_hz, got {min_hz}..{max_hz}"
        );
        assert!(width > 0.0, "width must be positive, got {width}");
        let log_min = min_hz.log10();
        Self {
            min_hz,
            max_hz,
            width,
            log_min,
            log_span: max_hz.log10() - log_min,
        }
    }

    /// The usual audio span, 20 Hz to 20 kHz.
    pub fn audible(width: f32) -> Self {
        Self::new(20.0, 20_000.0, width)
    }

    /// Lowest frequency shown.
    pub fn min_hz(&self) -> f32 {
        self.min_hz
    }

    /// Highest frequency shown.
    pub fn max_hz(&self) -> f32 {
        self.max_hz
    }

    /// Axis width in pixels.
    pub fn width(&self) -> f32 {
        self.width
    }

    /// Pixel position of a frequency. Values outside the range extrapolate
    /// rather than clamp, so a caller can decide whether to cull or draw.
    pub fn freq_to_x(&self, hz: f32) -> f32 {
        if hz <= 0.0 {
            return f32::NEG_INFINITY;
        }
        (hz.log10() - self.log_min) / self.log_span * self.width
    }

    /// Frequency at a pixel position.
    pub fn x_to_freq(&self, x: f32) -> f32 {
        10.0_f32.powf(self.log_min + (x / self.width) * self.log_span)
    }

    /// Resize without changing the frequency range.
    pub fn with_width(&self, width: f32) -> Self {
        Self::new(self.min_hz, self.max_hz, width)
    }

    /// Zoom to a new frequency range, keeping the width.
    pub fn with_range(&self, min_hz: f32, max_hz: f32) -> Self {
        Self::new(min_hz, max_hz, self.width)
    }

    /// Gridlines at 1-2-5 positions within each decade.
    ///
    /// Major ticks land on decades. This is the convention every audio analyser
    /// uses, and matching it matters more than any argument about tick density.
    pub fn ticks(&self) -> Vec<Tick> {
        const STEPS: [f32; 9] = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0];
        let first = self.min_hz.log10().floor() as i32;
        let last = self.max_hz.log10().ceil() as i32;

        let mut ticks = Vec::new();
        for decade in first..=last {
            let base = 10.0_f32.powi(decade);
            for step in STEPS {
                let value = base * step;
                if value < self.min_hz || value > self.max_hz {
                    continue;
                }
                ticks.push(Tick {
                    value,
                    position: self.freq_to_x(value),
                    major: (step - 1.0).abs() < f32::EPSILON,
                });
            }
        }
        ticks
    }
}

/// Format a frequency the way an audio user expects: `20`, `500`, `2k`, `20k`.
pub fn format_frequency(hz: f32) -> String {
    if hz >= 1000.0 {
        let k = hz / 1000.0;
        if (k - k.round()).abs() < 0.05 {
            format!("{}k", k.round() as i32)
        } else {
            format!("{k:.1}k")
        }
    } else if hz >= 10.0 {
        format!("{}", hz.round() as i32)
    } else {
        format!("{hz:.1}")
    }
}

/// Linear decibel axis, with y increasing downwards.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct LevelAxis {
    min_db: f32,
    max_db: f32,
    height: f32,
}

impl LevelAxis {
    /// Build an axis spanning `min_db..=max_db` across `height` pixels.
    ///
    /// # Panics
    ///
    /// Panics if `max_db` is not above `min_db`, or if `height` is not positive.
    pub fn new(min_db: f32, max_db: f32, height: f32) -> Self {
        assert!(
            max_db > min_db,
            "need min_db < max_db, got {min_db}..{max_db}"
        );
        assert!(height > 0.0, "height must be positive, got {height}");
        Self {
            min_db,
            max_db,
            height,
        }
    }

    /// A sensible default for dBFS: -120 to 0, the top being full scale.
    pub fn full_scale(height: f32) -> Self {
        Self::new(-120.0, 0.0, height)
    }

    /// Lowest level shown.
    pub fn min_db(&self) -> f32 {
        self.min_db
    }

    /// Highest level shown.
    pub fn max_db(&self) -> f32 {
        self.max_db
    }

    /// Axis height in pixels.
    pub fn height(&self) -> f32 {
        self.height
    }

    /// Pixel position of a level. Zero is the top of the axis.
    pub fn db_to_y(&self, db: f32) -> f32 {
        (self.max_db - db) / (self.max_db - self.min_db) * self.height
    }

    /// Level at a pixel position.
    pub fn y_to_db(&self, y: f32) -> f32 {
        self.max_db - (y / self.height) * (self.max_db - self.min_db)
    }

    /// Resize without changing the level range.
    pub fn with_height(&self, height: f32) -> Self {
        Self::new(self.min_db, self.max_db, height)
    }

    /// Rescale to a new level range, keeping the height.
    pub fn with_range(&self, min_db: f32, max_db: f32) -> Self {
        Self::new(min_db, max_db, self.height)
    }

    /// Gridlines every `step` decibels, aligned to multiples of `step` rather
    /// than to the axis ends, so the grid does not shift as the range is zoomed.
    ///
    /// # Panics
    ///
    /// Panics if `step` is not positive.
    pub fn ticks(&self, step: f32) -> Vec<Tick> {
        assert!(step > 0.0, "tick step must be positive, got {step}");

        let first = (self.min_db / step).ceil() as i32;
        let last = (self.max_db / step).floor() as i32;
        // Guard against an absurd step producing millions of ticks.
        let count = (last - first).clamp(0, 1024);

        (0..=count)
            .map(|i| {
                let value = (first + i) as f32 * step;
                Tick {
                    value,
                    position: self.db_to_y(value),
                    // Every 10th of a decade of dB reads as the significant one.
                    major: (value / (step * 2.0)).fract().abs() < 1e-4,
                }
            })
            .filter(|t| t.value >= self.min_db - 1e-3 && t.value <= self.max_db + 1e-3)
            .collect()
    }
}

impl fmt::Display for Tick {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:.1} @ {:.1}px", self.value, self.position)
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    #[test]
    fn frequency_endpoints_map_to_the_edges() {
        let axis = FrequencyAxis::audible(1000.0);
        assert!(axis.freq_to_x(20.0).abs() < 1e-3);
        assert!((axis.freq_to_x(20_000.0) - 1000.0).abs() < 1e-3);
    }

    /// Each decade must occupy equal width. Three decades across 900 px is
    /// 300 px each, which is the whole point of a log axis.
    #[test]
    fn decades_are_evenly_spaced() {
        let axis = FrequencyAxis::new(20.0, 20_000.0, 900.0);
        let a = axis.freq_to_x(20.0);
        let b = axis.freq_to_x(200.0);
        let c = axis.freq_to_x(2000.0);
        let d = axis.freq_to_x(20_000.0);
        assert!((b - a - 300.0).abs() < 0.01);
        assert!((c - b - 300.0).abs() < 0.01);
        assert!((d - c - 300.0).abs() < 0.01);
    }

    #[test]
    fn frequency_round_trips_through_pixels() {
        let axis = FrequencyAxis::audible(1440.0);
        for hz in [20.0, 63.0, 100.0, 440.0, 1000.0, 4700.0, 19_000.0] {
            let back = axis.x_to_freq(axis.freq_to_x(hz));
            assert!(
                (back - hz).abs() / hz < 1e-4,
                "{hz} Hz came back as {back} Hz"
            );
        }
    }

    #[test]
    fn frequency_outside_the_range_extrapolates_rather_than_clamping() {
        let axis = FrequencyAxis::audible(1000.0);
        assert!(axis.freq_to_x(10.0) < 0.0);
        assert!(axis.freq_to_x(40_000.0) > 1000.0);
        // Zero and negatives cannot be placed at all.
        assert_eq!(axis.freq_to_x(0.0), f32::NEG_INFINITY);
    }

    #[test]
    fn frequency_ticks_cover_the_expected_decades() {
        let axis = FrequencyAxis::audible(1000.0);
        let ticks = axis.ticks();

        let majors: Vec<f32> = ticks.iter().filter(|t| t.major).map(|t| t.value).collect();
        assert_eq!(majors, vec![100.0, 1000.0, 10_000.0]);

        assert!(ticks.iter().all(|t| t.value >= 20.0 && t.value <= 20_000.0));
        // Ticks must be in ascending pixel order for a renderer to stride them.
        for pair in ticks.windows(2) {
            assert!(pair[0].position < pair[1].position);
        }
    }

    #[test]
    fn frequency_labels_use_audio_conventions() {
        assert_eq!(format_frequency(20.0), "20");
        assert_eq!(format_frequency(500.0), "500");
        assert_eq!(format_frequency(1000.0), "1k");
        assert_eq!(format_frequency(2000.0), "2k");
        assert_eq!(format_frequency(20_000.0), "20k");
        assert_eq!(format_frequency(1500.0), "1.5k");
        assert_eq!(format_frequency(6.3), "6.3");
    }

    #[test]
    fn level_top_is_zero_and_bottom_is_the_height() {
        let axis = LevelAxis::full_scale(600.0);
        assert!(axis.db_to_y(0.0).abs() < 1e-3);
        assert!((axis.db_to_y(-120.0) - 600.0).abs() < 1e-3);
        // Halfway in level is halfway down the pixels.
        assert!((axis.db_to_y(-60.0) - 300.0).abs() < 1e-3);
    }

    #[test]
    fn level_round_trips_through_pixels() {
        let axis = LevelAxis::new(-90.0, 6.0, 480.0);
        for db in [6.0, 0.0, -12.5, -60.0, -89.9] {
            let back = axis.y_to_db(axis.db_to_y(db));
            assert!((back - db).abs() < 1e-3, "{db} came back as {back}");
        }
    }

    #[test]
    fn level_ticks_align_to_multiples_not_to_the_range() {
        // A range starting at -117 must still tick on multiples of 10.
        let axis = LevelAxis::new(-117.0, 3.0, 600.0);
        let ticks = axis.ticks(10.0);
        assert!(ticks.iter().all(|t| (t.value % 10.0).abs() < 1e-3));
        assert_eq!(ticks.first().map(|t| t.value), Some(-110.0));
        assert_eq!(ticks.last().map(|t| t.value), Some(0.0));
    }

    #[test]
    fn level_ticks_stay_inside_the_range() {
        let axis = LevelAxis::full_scale(600.0);
        for tick in axis.ticks(10.0) {
            assert!(tick.value >= -120.0 && tick.value <= 0.0);
            assert!(tick.position >= -1e-3 && tick.position <= 600.0 + 1e-3);
        }
    }

    #[test]
    fn an_absurd_tick_step_does_not_explode() {
        let axis = LevelAxis::new(-200.0, 0.0, 600.0);
        assert!(axis.ticks(0.0001).len() <= 1025);
    }

    #[test]
    fn resizing_preserves_the_data_range() {
        let axis = FrequencyAxis::audible(800.0).with_width(1600.0);
        assert_eq!(axis.width(), 1600.0);
        assert!((axis.freq_to_x(20_000.0) - 1600.0).abs() < 1e-3);

        let level = LevelAxis::full_scale(300.0).with_height(900.0);
        assert!((level.db_to_y(-120.0) - 900.0).abs() < 1e-3);
    }

    #[test]
    #[should_panic(expected = "need 0 < min_hz < max_hz")]
    fn a_frequency_axis_through_zero_is_rejected() {
        let _ = FrequencyAxis::new(0.0, 20_000.0, 100.0);
    }

    #[test]
    #[should_panic(expected = "need min_db < max_db")]
    fn an_inverted_level_axis_is_rejected() {
        let _ = LevelAxis::new(0.0, -120.0, 100.0);
    }
}
