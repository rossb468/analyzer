// Axis mappings between data and screen space.
//
// These live in the core rather than the UI on purpose. Cursor readout,
// hit-testing, marker placement and the drawn geometry must all agree exactly,
// and the only way to guarantee that is for one implementation to answer every
// question. A Swift or C# layer reimplementing "which frequency is under this
// pixel" will drift from what was actually plotted, and the resulting
// disagreement is subtle enough to survive a long time.
//
// Screen space follows the usual convention: x increases rightwards, y
// increases **downwards**, so the loudest level sits at y = 0.

#pragma once

#include <string>
#include <vector>

namespace analyzer::plot {

// A gridline.
struct Tick {
    // Value in data units - hertz or decibels.
    float value = 0.0f;
    // Position in pixels along the axis.
    float position = 0.0f;
    // Whether this deserves a label and a heavier line.
    bool major = false;

    friend bool operator==(const Tick&, const Tick&) = default;
};

// Logarithmic frequency axis.
//
// Log spacing is not cosmetic: hearing is roughly logarithmic in frequency, so
// a linear axis wastes most of its width on the top octave and crushes the
// bass into nothing.
//
// A small value type: copy it freely, resize or zoom it by building a new one.
class FrequencyAxis {
public:
    // Build an axis spanning min_hz..=max_hz across `width` pixels.
    //
    // Both frequencies must be positive, max_hz must be above min_hz, and
    // `width` must be positive. A log axis through zero has no meaning, and
    // silently substituting a default would hide a caller's bug.
    FrequencyAxis(float min_hz, float max_hz, float width);

    // The usual audio span, 20 Hz to 20 kHz.
    static FrequencyAxis audible(float width);

    // Lowest frequency shown.
    float min_hz() const noexcept { return min_hz_; }

    // Highest frequency shown.
    float max_hz() const noexcept { return max_hz_; }

    // Axis width in pixels.
    float width() const noexcept { return width_; }

    // Pixel position of a frequency. Values outside the range extrapolate
    // rather than clamp, so a caller can decide whether to cull or draw.
    // Zero and negative frequencies cannot be placed and map to negative
    // infinity.
    float freq_to_x(float hz) const noexcept;

    // Frequency at a pixel position.
    float x_to_freq(float x) const noexcept;

    // Resize without changing the frequency range.
    [[nodiscard]] FrequencyAxis with_width(float width) const;

    // Zoom to a new frequency range, keeping the width.
    [[nodiscard]] FrequencyAxis with_range(float min_hz, float max_hz) const;

    // Gridlines at every integer step within each decade (1, 2, 3 ... 9).
    //
    // Major ticks land on decades. This is the convention every audio analyser
    // uses, and matching it matters more than any argument about tick density.
    // Returned in ascending pixel order.
    std::vector<Tick> ticks() const;

    friend bool operator==(const FrequencyAxis&, const FrequencyAxis&) = default;

private:
    float min_hz_;
    float max_hz_;
    float width_;
    float log_min_;
    float log_span_;
};

// Format a frequency the way an audio user expects: "20", "500", "2k", "20k".
std::string format_frequency(float hz);

// Linear decibel axis, with y increasing downwards.
class LevelAxis {
public:
    // Build an axis spanning min_db..=max_db across `height` pixels.
    //
    // max_db must be above min_db, and `height` must be positive.
    LevelAxis(float min_db, float max_db, float height);

    // A sensible default for dBFS: -120 to 0, the top being full scale.
    static LevelAxis full_scale(float height);

    // Lowest level shown.
    float min_db() const noexcept { return min_db_; }

    // Highest level shown.
    float max_db() const noexcept { return max_db_; }

    // Axis height in pixels.
    float height() const noexcept { return height_; }

    // Pixel position of a level. Zero is the top of the axis.
    float db_to_y(float db) const noexcept;

    // Level at a pixel position.
    float y_to_db(float y) const noexcept;

    // Resize without changing the level range.
    [[nodiscard]] LevelAxis with_height(float height) const;

    // Rescale to a new level range, keeping the height.
    [[nodiscard]] LevelAxis with_range(float min_db, float max_db) const;

    // Gridlines every `step` decibels, aligned to multiples of `step` rather
    // than to the axis ends, so the grid does not shift as the range is zoomed.
    //
    // `step` must be positive. An absurdly small step is capped at 1025 ticks
    // rather than producing millions.
    std::vector<Tick> ticks(float step) const;

    friend bool operator==(const LevelAxis&, const LevelAxis&) = default;

private:
    float min_db_;
    float max_db_;
    float height_;
};

}  // namespace analyzer::plot
