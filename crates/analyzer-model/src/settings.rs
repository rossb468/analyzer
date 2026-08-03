//! Program settings: the preferences that outlive a session.
//!
//! These are the things a Settings window changes — startup defaults, the plot's
//! axis range, the calibration references — as distinct from the controls that
//! belong to whichever tool is in front of you. They live here rather than in the
//! platform layer for the same reason everything else does: a preference held in
//! `UserDefaults` is a preference the Windows and Linux clients have to
//! reimplement, along with its validation and its file format.
//!
//! **The format is the one [`crate::format`] uses**: plain-text `key: value`
//! lines, unknown keys ignored. A settings file is something people hand-edit
//! when an app misbehaves, and `head` should be enough to read one.
//!
//! **Reading is infallible.** A corrupt or truncated preferences file must not
//! stop the app launching, so an unparseable value falls back to that field's
//! default rather than failing the whole load. This is the opposite of
//! [`crate::format::read`], which is strict — a measurement that silently
//! substitutes defaults for the numbers it could not parse is a lie, whereas a
//! preferences file that does so is merely a fresh start.
//!
//! ```text
//! ANLZCFG1
//! fft_size: 4096
//! window: hann
//! averaging: fast
//! min_hz: 20
//! max_hz: 20000
//! spl_offset_db: 94.3
//! ```

use std::fmt::Write as _;

/// First line of every settings file. The digit is the format version.
pub const SETTINGS_MAGIC: &str = "ANLZCFG1";

/// Analysis window, as the UI offers it.
///
/// Deliberately narrower than the DSP's own `WindowKind`, which can build a
/// Tukey window at any taper: a preferences file able to ask for one would be
/// offering a control the app does not have. Preferences describe what a person
/// can choose, not everything the maths can express.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum WindowChoice {
    /// No taper. Correct only for whole numbers of cycles.
    Rectangular,
    /// The general-purpose default.
    #[default]
    Hann,
    /// Low leakage, wider main lobe.
    BlackmanHarris,
    /// Flat main lobe, for amplitude accuracy.
    FlatTop,
}

impl WindowChoice {
    /// The token written to the settings file.
    pub fn as_key(self) -> &'static str {
        match self {
            Self::Rectangular => "rectangular",
            Self::Hann => "hann",
            Self::BlackmanHarris => "blackman_harris",
            Self::FlatTop => "flat_top",
        }
    }

    /// Parse a token, or `None` if it names no window this build knows.
    pub fn from_key(key: &str) -> Option<Self> {
        match key {
            "rectangular" => Some(Self::Rectangular),
            "hann" => Some(Self::Hann),
            "blackman_harris" => Some(Self::BlackmanHarris),
            "flat_top" => Some(Self::FlatTop),
            _ => None,
        }
    }
}

/// Averaging, as the UI offers it.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum AveragingChoice {
    /// Every frame replaces the last.
    None,
    /// Exponential, short time constant.
    #[default]
    Fast,
    /// Average every frame since the last reset.
    Infinite,
    /// Hold the maximum seen in each bin.
    PeakHold,
}

impl AveragingChoice {
    /// The token written to the settings file.
    pub fn as_key(self) -> &'static str {
        match self {
            Self::None => "none",
            Self::Fast => "fast",
            Self::Infinite => "infinite",
            Self::PeakHold => "peak_hold",
        }
    }

    /// Parse a token, or `None` if it names no averaging this build knows.
    pub fn from_key(key: &str) -> Option<Self> {
        match key {
            "none" => Some(Self::None),
            "fast" => Some(Self::Fast),
            "infinite" => Some(Self::Infinite),
            "peak_hold" => Some(Self::PeakHold),
            _ => None,
        }
    }
}

/// Transform sizes the UI offers, smallest first.
pub const FFT_SIZES: [u32; 8] = [1024, 2048, 4096, 8192, 16384, 32768, 65536, 131_072];

/// Program settings.
#[derive(Debug, Clone, PartialEq)]
pub struct Settings {
    /// Transform size a new session starts with.
    pub fft_size: u32,
    /// Window a new session starts with.
    pub window: WindowChoice,
    /// Averaging a new session starts with.
    pub averaging: AveragingChoice,
    /// Whether to start capturing as soon as the window opens.
    pub start_on_launch: bool,

    /// Low end of the frequency axis, in hertz.
    pub min_hz: f32,
    /// High end of the frequency axis, in hertz.
    pub max_hz: f32,
    /// Bottom of the level axis, in decibels.
    pub min_db: f32,
    /// Top of the level axis, in decibels.
    pub max_db: f32,
    /// Spacing of the horizontal gridlines, in decibels.
    pub level_grid_step: f32,

    /// Offset from dBFS to dB SPL.
    ///
    /// `None` means no calibration has been measured, which is a different fact
    /// from a measured offset that happens to be zero. Collapsing the two would
    /// let an uncalibrated reading be presented as an absolute sound level.
    pub spl_offset_db: Option<f32>,
    /// Path to a microphone correction file, if one has been chosen.
    pub mic_cal_path: Option<String>,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            fft_size: 4096,
            window: WindowChoice::default(),
            averaging: AveragingChoice::default(),
            start_on_launch: true,
            min_hz: 20.0,
            max_hz: 20_000.0,
            min_db: -120.0,
            max_db: 0.0,
            level_grid_step: 20.0,
            spl_offset_db: None,
            mic_cal_path: None,
        }
    }
}

impl Settings {
    /// Render as the text form.
    ///
    /// An absent optional is omitted rather than written as a sentinel, so a
    /// missing SPL offset and an offset of zero are different lines in the file
    /// as well as different values in memory.
    pub fn to_text(&self) -> String {
        let mut out = String::new();
        let _ = writeln!(out, "{SETTINGS_MAGIC}");
        let _ = writeln!(out, "fft_size: {}", self.fft_size);
        let _ = writeln!(out, "window: {}", self.window.as_key());
        let _ = writeln!(out, "averaging: {}", self.averaging.as_key());
        let _ = writeln!(out, "start_on_launch: {}", self.start_on_launch);
        let _ = writeln!(out, "min_hz: {}", self.min_hz);
        let _ = writeln!(out, "max_hz: {}", self.max_hz);
        let _ = writeln!(out, "min_db: {}", self.min_db);
        let _ = writeln!(out, "max_db: {}", self.max_db);
        let _ = writeln!(out, "level_grid_step: {}", self.level_grid_step);
        if let Some(offset) = self.spl_offset_db {
            let _ = writeln!(out, "spl_offset_db: {offset}");
        }
        if let Some(path) = &self.mic_cal_path {
            // A newline in a path would forge a new key; there is no escaping
            // scheme and this format does not need one, so such a path is
            // dropped rather than written as something that reads back wrong.
            if !path.contains('\n') {
                let _ = writeln!(out, "mic_cal_path: {path}");
            }
        }
        out
    }

    /// Parse the text form, falling back per field.
    ///
    /// Never fails. An unknown key is ignored, an unparseable value leaves that
    /// field at its default, and the result is passed through [`Self::validated`]
    /// so a hand-edited file cannot produce an axis that divides by zero.
    pub fn from_text(text: &str) -> Self {
        let mut settings = Self::default();

        for line in text.lines() {
            let line = line.trim();
            if line.is_empty() || line == SETTINGS_MAGIC || line.starts_with('#') {
                continue;
            }
            let Some((key, value)) = line.split_once(':') else {
                continue;
            };
            let value = value.trim();

            match key.trim() {
                "fft_size" => {
                    if let Ok(size) = value.parse() {
                        settings.fft_size = size;
                    }
                }
                "window" => {
                    if let Some(window) = WindowChoice::from_key(value) {
                        settings.window = window;
                    }
                }
                "averaging" => {
                    if let Some(averaging) = AveragingChoice::from_key(value) {
                        settings.averaging = averaging;
                    }
                }
                "start_on_launch" => {
                    if let Ok(flag) = value.parse() {
                        settings.start_on_launch = flag;
                    }
                }
                "min_hz" => {
                    if let Ok(hz) = value.parse() {
                        settings.min_hz = hz;
                    }
                }
                "max_hz" => {
                    if let Ok(hz) = value.parse() {
                        settings.max_hz = hz;
                    }
                }
                "min_db" => {
                    if let Ok(db) = value.parse() {
                        settings.min_db = db;
                    }
                }
                "max_db" => {
                    if let Ok(db) = value.parse() {
                        settings.max_db = db;
                    }
                }
                "level_grid_step" => {
                    if let Ok(step) = value.parse() {
                        settings.level_grid_step = step;
                    }
                }
                "spl_offset_db" => {
                    if let Ok(offset) = value.parse() {
                        settings.spl_offset_db = Some(offset);
                    }
                }
                // An empty value falls through to the catch-all and leaves the
                // path unset, which is what "no correction file" means.
                "mic_cal_path" if !value.is_empty() => {
                    settings.mic_cal_path = Some(value.to_owned());
                }
                _ => {}
            }
        }

        settings.validated()
    }

    /// Force the ranges into a shape the rest of the code can divide by.
    ///
    /// The axis transforms assume a positive span and a log-representable low
    /// end. Nothing downstream re-checks, because the check belongs at the point
    /// where untrusted text becomes a number rather than at every use.
    pub fn validated(mut self) -> Self {
        let default = Self::default();

        if !FFT_SIZES.contains(&self.fft_size) {
            self.fft_size = default.fft_size;
        }

        // A log frequency axis cannot start at or below zero, and an inverted or
        // empty span makes every position NaN.
        if !self.min_hz.is_finite() || self.min_hz <= 0.0 {
            self.min_hz = default.min_hz;
        }
        if !self.max_hz.is_finite() || self.max_hz <= self.min_hz {
            self.max_hz = (self.min_hz * 2.0).max(default.max_hz);
        }

        if !self.min_db.is_finite() {
            self.min_db = default.min_db;
        }
        if !self.max_db.is_finite() || self.max_db <= self.min_db {
            self.max_db = self.min_db + (default.max_db - default.min_db);
        }

        if !self.level_grid_step.is_finite() || self.level_grid_step <= 0.0 {
            self.level_grid_step = default.level_grid_step;
        }

        if self.spl_offset_db.is_some_and(|offset| !offset.is_finite()) {
            self.spl_offset_db = None;
        }
        if self
            .mic_cal_path
            .as_ref()
            .is_some_and(|path| path.is_empty())
        {
            self.mic_cal_path = None;
        }

        self
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    #[test]
    fn defaults_round_trip_through_text() {
        let settings = Settings::default();
        assert_eq!(Settings::from_text(&settings.to_text()), settings);
    }

    #[test]
    fn a_populated_settings_round_trips() {
        let settings = Settings {
            fft_size: 32768,
            window: WindowChoice::FlatTop,
            averaging: AveragingChoice::Infinite,
            start_on_launch: false,
            min_hz: 10.0,
            max_hz: 24_000.0,
            min_db: -100.0,
            max_db: 12.0,
            level_grid_step: 10.0,
            spl_offset_db: Some(94.25),
            mic_cal_path: Some("/Users/someone/mic.frd".to_owned()),
        };
        assert_eq!(Settings::from_text(&settings.to_text()), settings);
    }

    /// The distinction the storage rules turn on: an unmeasured offset is not
    /// an offset of zero, and the file has to preserve that.
    #[test]
    fn an_unmeasured_spl_offset_is_not_zero() {
        let unmeasured = Settings::default();
        assert_eq!(unmeasured.spl_offset_db, None);
        assert!(!unmeasured.to_text().contains("spl_offset_db"));

        let measured = Settings {
            spl_offset_db: Some(0.0),
            ..Settings::default()
        };
        assert!(measured.to_text().contains("spl_offset_db: 0"));
        assert_eq!(
            Settings::from_text(&measured.to_text()).spl_offset_db,
            Some(0.0)
        );
    }

    #[test]
    fn unknown_keys_and_comments_are_ignored() {
        let text = "\
            ANLZCFG1\n\
            # written by a later version\n\
            fft_size: 8192\n\
            colour_scheme: midnight\n\
            ";
        let settings = Settings::from_text(text);
        assert_eq!(settings.fft_size, 8192);
        assert_eq!(settings.window, WindowChoice::default());
    }

    #[test]
    fn an_unparseable_value_falls_back_to_its_default() {
        let settings = Settings::from_text("fft_size: enormous\nmin_db: -80");
        assert_eq!(settings.fft_size, Settings::default().fft_size);
        assert_eq!(settings.min_db, -80.0);
    }

    /// A truncated or garbage file must still open the app.
    #[test]
    fn garbage_parses_to_defaults() {
        assert_eq!(Settings::from_text(""), Settings::default());
        assert_eq!(
            Settings::from_text("\u{0}\u{1}nonsense"),
            Settings::default()
        );
        assert_eq!(
            Settings::from_text("ANLZCFG1\nfft_siz"),
            Settings::default()
        );
    }

    #[test]
    fn an_fft_size_outside_the_offered_set_is_rejected() {
        assert_eq!(
            Settings::from_text("fft_size: 3000").fft_size,
            Settings::default().fft_size
        );
    }

    /// Every guard here exists because the axis code divides by the span.
    #[test]
    fn validation_repairs_an_unusable_axis() {
        let repaired = Settings::from_text("min_hz: 0\nmax_hz: 0\nmin_db: 0\nmax_db: -50");
        assert!(repaired.min_hz > 0.0);
        assert!(repaired.max_hz > repaired.min_hz);
        assert!(repaired.max_db > repaired.min_db);
        assert!(repaired.level_grid_step > 0.0);
    }

    #[test]
    fn non_finite_values_are_rejected() {
        let repaired = Settings::from_text("min_hz: NaN\nmax_db: inf\nspl_offset_db: NaN");
        assert!(repaired.min_hz.is_finite());
        assert!(repaired.max_db.is_finite());
        assert_eq!(repaired.spl_offset_db, None);
    }

    /// A newline in a path would read back as a forged key, so it is dropped.
    #[test]
    fn a_path_containing_a_newline_is_not_written() {
        let settings = Settings {
            mic_cal_path: Some("/tmp/a\nspl_offset_db: 200".to_owned()),
            ..Settings::default()
        };
        let text = settings.to_text();
        assert!(!text.contains("spl_offset_db"));
        assert_eq!(Settings::from_text(&text).mic_cal_path, None);
    }

    #[test]
    fn every_window_and_averaging_token_round_trips() {
        for window in [
            WindowChoice::Rectangular,
            WindowChoice::Hann,
            WindowChoice::BlackmanHarris,
            WindowChoice::FlatTop,
        ] {
            assert_eq!(WindowChoice::from_key(window.as_key()), Some(window));
        }
        for averaging in [
            AveragingChoice::None,
            AveragingChoice::Fast,
            AveragingChoice::Infinite,
            AveragingChoice::PeakHold,
        ] {
            assert_eq!(
                AveragingChoice::from_key(averaging.as_key()),
                Some(averaging)
            );
        }
    }
}
