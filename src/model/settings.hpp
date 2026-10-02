// Program settings: the preferences that outlive a session.
//
// These are the things a Settings window changes - startup defaults, the plot's
// axis range, the calibration references - as distinct from the controls that
// belong to whichever tool is in front of you. They live here rather than in
// the platform layer for the same reason everything else does: a preference
// held in UserDefaults is a preference the Windows and Linux clients have to
// reimplement, along with its validation and its file format.
//
// The format is the one format.hpp uses: plain-text `key: value` lines, unknown
// keys ignored. A settings file is something people hand-edit when an app
// misbehaves, and `head` should be enough to read one.
//
// Reading is infallible. A corrupt or truncated preferences file must not stop
// the app launching, so an unparseable value falls back to that field's default
// rather than failing the whole load. This is the opposite of read_measurement,
// which is strict - a measurement that silently substitutes defaults for the
// numbers it could not parse is a lie, whereas a preferences file that does so
// is merely a fresh start.
//
//   ANLZCFG1
//   fft_size: 4096
//   window: hann
//   averaging: fast
//   min_hz: 20
//   max_hz: 20000
//   spl_offset_db: 94.3

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace analyzer::model {

// First line of every settings file. The digit is the format version.
inline constexpr std::string_view kSettingsMagic = "ANLZCFG1";

// Analysis window, as the UI offers it.
//
// Deliberately narrower than the DSP's own WindowKind, which can build a Tukey
// window at any taper: a preferences file able to ask for one would be offering
// a control the app does not have. Preferences describe what a person can
// choose, not everything the maths can express.
enum class WindowChoice {
    // No taper. Correct only for whole numbers of cycles.
    Rectangular,
    // The general-purpose default.
    Hann,
    // Low leakage, wider main lobe.
    BlackmanHarris,
    // Flat main lobe, for amplitude accuracy.
    FlatTop,
};

// The token written to the settings file.
std::string_view as_key(WindowChoice window) noexcept;

// Parse a token, or nullopt if it names no window this build knows.
std::optional<WindowChoice> window_from_key(std::string_view key) noexcept;

// Averaging, as the UI offers it.
enum class AveragingChoice {
    // Every frame replaces the last.
    None,
    // Exponential, short time constant.
    Fast,
    // Average every frame since the last reset.
    Infinite,
    // Hold the maximum seen in each bin.
    PeakHold,
};

// The token written to the settings file.
std::string_view as_key(AveragingChoice averaging) noexcept;

// Parse a token, or nullopt if it names no averaging this build knows.
std::optional<AveragingChoice> averaging_from_key(std::string_view key) noexcept;

// Transform sizes the UI offers, smallest first.
inline constexpr std::array<std::uint32_t, 8> kFftSizes = {
    1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072,
};

// Program settings.
struct Settings {
    // Transform size a new session starts with.
    std::uint32_t fft_size = 4096;
    // Window a new session starts with.
    WindowChoice window = WindowChoice::Hann;
    // Averaging a new session starts with.
    AveragingChoice averaging = AveragingChoice::Fast;
    // Whether to start capturing as soon as the window opens.
    bool start_on_launch = true;

    // Low end of the frequency axis, in hertz.
    float min_hz = 20.0f;
    // High end of the frequency axis, in hertz.
    float max_hz = 20'000.0f;
    // Bottom of the level axis, in decibels.
    float min_db = -120.0f;
    // Top of the level axis, in decibels.
    float max_db = 0.0f;
    // Spacing of the horizontal gridlines, in decibels.
    float level_grid_step = 20.0f;

    // Offset from dBFS to dB SPL.
    //
    // nullopt means no calibration has been measured, which is a different fact
    // from a measured offset that happens to be zero. Collapsing the two would
    // let an uncalibrated reading be presented as an absolute sound level.
    std::optional<float> spl_offset_db;
    // Path to a microphone correction file, if one has been chosen.
    std::optional<std::string> mic_cal_path;

    // Render as the text form.
    //
    // An absent optional is omitted rather than written as a sentinel, so a
    // missing SPL offset and an offset of zero are different lines in the file
    // as well as different values in memory.
    std::string to_text() const;

    // Parse the text form, falling back per field.
    //
    // Never throws. An unknown key is ignored, an unparseable value leaves that
    // field at its default, and the result is passed through validated() so a
    // hand-edited file cannot produce an axis that divides by zero.
    static Settings from_text(std::string_view contents);

    // Force the ranges into a shape the rest of the code can divide by.
    //
    // The axis transforms assume a positive span and a log-representable low
    // end. Nothing downstream re-checks, because the check belongs at the point
    // where untrusted text becomes a number rather than at every use.
    Settings validated() const;

    friend bool operator==(const Settings&, const Settings&) = default;
};

}  // namespace analyzer::model
