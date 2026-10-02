// Target curves: the response a correction is aiming at.
//
// A measurement on its own says what a room does. It does not say what it
// should do, and "flat" is the wrong answer for a room: a loudspeaker measured
// anechoically flat sounds thin in a room, because the ear expects the bass lift
// that a real space produces. Every serious correction is fitted against a
// target that is not flat.
//
// Three parameterised shapes plus a file:
//
// - Flat - the reference, and what a transfer function of a single loudspeaker
//   measured close up should aim at.
// - Tilt - a constant slope in decibels per octave. The whole of some house
//   curves.
// - Room - a bass shelf plus a tilt, which is the shape of most published room
//   targets.
// - Custom - points from a file, interpolated in decibels against **log**
//   frequency for the same reason calibration files are: the points are sparse
//   and roughly log-spaced, so interpolating against linear frequency badly
//   misplaces everything below a few hundred hertz.
//
// The parameters are exposed rather than baked in. These shapes are drawn from
// common practice and the defaults are reasonable, but this does not claim to
// reproduce any specific published target exactly, and a curve that claimed to
// would be wrong the moment its author revised it.
//
// Targets are relative
//
// A target says nothing about absolute level: +6 dB of bass lift is lift
// relative to the rest of the curve, not an absolute sound pressure. Drawing one
// against a measurement therefore needs an offset, and TargetCurve::aligned_to()
// picks the one that makes the average difference over a chosen band zero.
// Without it the target floats somewhere unrelated to the measurement and every
// error the optimiser computes is dominated by that constant.

#pragma once

#include <span>
#include <utility>
#include <vector>

namespace analyzer::dsp {

// Where a tilt pivots, and the band a room target is flat in.
inline constexpr float kReferenceHz = 1000.0f;

// Band the alignment uses unless told otherwise.
inline constexpr float kAlignFromHz = 200.0f;
// Upper end of the default alignment band.
inline constexpr float kAlignToHz = 2000.0f;

// The shape of a target.
//
// A small value type rather than a bare enum because the shapes carry
// parameters. Only the fields named for a kind are read; build one with the
// named constructors:
//
//   TargetShape::flat()
//   TargetShape::tilt(-1.5f)
//   TargetShape::room()
//   TargetShape::custom({{100.0f, 3.0f}, {1000.0f, -2.0f}})
struct TargetShape {
    enum class Kind {
        // Flat at every frequency.
        Flat,
        // A constant slope, zero at kReferenceHz. Uses db_per_octave.
        Tilt,
        // A bass shelf with an optional tilt above it. Uses shelf_db,
        // transition_hz and db_per_octave.
        Room,
        // (hz, db) points, sorted ascending and interpolated in log frequency.
        // Uses points.
        Custom,
    };

    Kind kind = Kind::Flat;
    // Tilt, Room: decibels per octave, zero at kReferenceHz. Negative slopes
    // downward with frequency, which is the direction room targets go.
    float db_per_octave = 0.0f;
    // Room: lift at the bottom of the band, in decibels.
    float shelf_db = 0.0f;
    // Room: where the shelf reaches half its lift.
    float transition_hz = 0.0f;
    // Custom: sorted by frequency, with non-positive frequencies removed. Build
    // through custom() to get that guarantee.
    std::vector<std::pair<float, float>> points;

    static TargetShape flat() { return {}; }

    static TargetShape tilt(float db_per_octave) {
        TargetShape s;
        s.kind = Kind::Tilt;
        s.db_per_octave = db_per_octave;
        return s;
    }

    // A room target with defaults drawn from common practice: 6 dB of lift
    // reaching half at 105 Hz, falling 0.5 dB per octave.
    static TargetShape room() { return room(6.0f, 105.0f, -0.5f); }

    static TargetShape room(float shelf_db, float transition_hz, float db_per_octave) {
        TargetShape s;
        s.kind = Kind::Room;
        s.shelf_db = shelf_db;
        s.transition_hz = transition_hz;
        s.db_per_octave = db_per_octave;
        return s;
    }

    // Build a custom shape, sorting and dropping unusable points.
    //
    // A file listing a DC row, or arriving unsorted, still works - the same
    // tolerance calibration files get, and for the same reason: these are other
    // people's exports.
    static TargetShape custom(std::vector<std::pair<float, float>> points);

    // Level in decibels at `hz`, before any alignment offset. A frequency that
    // is not positive and finite evaluates to 0 rather than NaN.
    float db_at(float hz) const noexcept;

    friend bool operator==(const TargetShape&, const TargetShape&) = default;
};

// A target curve: a shape plus the offset that puts it on a measurement.
class TargetCurve {
public:
    // A flat target with no offset.
    TargetCurve() = default;

    // A target with no offset applied.
    explicit TargetCurve(TargetShape shape) : shape_(std::move(shape)) {}

    // The shape being evaluated.
    const TargetShape& shape() const noexcept { return shape_; }

    // The alignment offset currently applied.
    float offset_db() const noexcept { return offset_db_; }

    // Replace the shape, keeping the offset.
    void set_shape(TargetShape shape) { shape_ = std::move(shape); }

    // Set the offset by hand. A non-finite offset is ignored.
    void set_offset_db(float offset_db) noexcept;

    // Level in decibels at `hz`, including the offset.
    float db_at(float hz) const noexcept { return shape_.db_at(hz) + offset_db_; }

    // Evaluate across `frequencies`, writing into `out`.
    //
    // Writes min(frequencies.size(), out.size()) values, so a caller cannot
    // overrun either side by getting the lengths out of step.
    void write_levels(std::span<const float> frequencies, std::span<float> out) const noexcept;

    // Choose the offset that makes the mean difference over a band zero.
    //
    // Only frequencies inside [from_hz, to_hz] with a finite measured level
    // count. A band containing nothing usable leaves the offset alone rather
    // than moving the curve somewhere arbitrary.
    //
    // The default band (kAlignFromHz to kAlignToHz) deliberately excludes the
    // bass, where the room's own modes swing the measurement by more than the
    // whole target does, and the top octave, where a microphone's own response
    // is least trustworthy. Aligning across the full range would let one 15 dB
    // null decide where the target sits.
    void align_to(std::span<const float> frequencies, std::span<const float> measured_db,
                  float from_hz, float to_hz) noexcept;

    // The same, returning a new curve and leaving this one alone.
    [[nodiscard]] TargetCurve aligned_to(std::span<const float> frequencies,
                                         std::span<const float> measured_db, float from_hz,
                                         float to_hz) const;

    // How far the measurement sits above the target, per frequency.
    //
    // This is the sign an equaliser has to undo: a positive error is too much
    // energy and wants a cut. Writes as many values as the shortest of the
    // three spans.
    void write_error(std::span<const float> frequencies, std::span<const float> measured_db,
                     std::span<float> out) const noexcept;

    friend bool operator==(const TargetCurve&, const TargetCurve&) = default;

private:
    TargetShape shape_;
    float offset_db_ = 0.0f;
};

}  // namespace analyzer::dsp
